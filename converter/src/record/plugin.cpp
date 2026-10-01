// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/plugin.hpp"

#include "bethconv/io/deflate.hpp"

#include <utility>

namespace bethconv::record {
namespace {

/// Valid GRUP nesting is at most six deep (top -> world children -> ext block
/// -> ext sub-block -> cell children -> temporary children). The depth comes
/// from the file, so it is capped to prevent stack overflow.
constexpr std::size_t k_max_group_depth = 32;

} // namespace

FourCC RecordContext::top_group_type() const noexcept {
    if (groups.empty()) {
        return FourCC{};
    }
    const auto& outermost = groups.front();
    if (outermost.header.group_type == GroupType::top) {
        return outermost.header.label.as_type();
    }
    return FourCC{};
}

io::ParseResult<PluginHeader> parse_plugin_header(io::SpanReader& data,
                                                  std::uint32_t flags) {
    PluginHeader header;
    header.flags = flags;

    MasterEntry pending_master;
    bool have_pending_master = false;

    while (!data.at_end()) {
        auto field = read_field_header(data);
        if (!field) {
            return std::unexpected(std::move(field).error());
        }
        auto body = data.subreader(field->data_size);
        if (!body) {
            return std::unexpected(std::move(body).error());
        }

        if (field->type == FourCC{"HEDR"}) {
            // Check each read separately. `if (!a || !b || !c) return a.error();`
            // would read the error of a successful expected (UB) when HEDR is
            // short. Found by fuzz_esm.
            auto version = body->get<float>();
            if (!version) {
                return std::unexpected(std::move(version).error());
            }
            auto count = body->get<std::int32_t>();
            if (!count) {
                return std::unexpected(std::move(count).error());
            }
            auto next_id = body->get<std::uint32_t>();
            if (!next_id) {
                return std::unexpected(std::move(next_id).error());
            }
            header.version = *version;
            header.record_count = *count;
            header.next_object_id = *next_id;
        } else if (field->type == FourCC{"CNAM"}) {
            auto s = body->zstring();
            if (s) {
                header.author = *s;
            }
        } else if (field->type == FourCC{"SNAM"}) {
            auto s = body->zstring();
            if (s) {
                header.description = *s;
            }
        } else if (field->type == FourCC{"MAST"}) {
            // MAST/DATA pairs, MAST first. A MAST without DATA is accepted; a
            // DATA without MAST is ignored.
            if (have_pending_master) {
                header.masters.push_back(std::move(pending_master));
            }
            auto s = body->zstring();
            if (!s) {
                return std::unexpected(std::move(s).error());
            }
            pending_master = MasterEntry{.name = std::string(*s), .size = 0};
            have_pending_master = true;
        } else if (field->type == FourCC{"DATA"} && have_pending_master) {
            auto size = body->get<std::uint64_t>();
            if (size) {
                pending_master.size = *size;
            }
            header.masters.push_back(std::move(pending_master));
            have_pending_master = false;
        } else if (field->type == FourCC{"ONAM"}) {
            const auto count = body->remaining() / sizeof(std::uint32_t);
            auto forms = body->array<std::uint32_t>(count);
            if (forms) {
                header.overrides.reserve(forms->size());
                for (const auto raw : *forms) {
                    header.overrides.push_back(FormId{raw});
                }
            }
        } else if (field->type == FourCC{"INTV"}) {
            auto v = body->get<std::uint32_t>();
            if (v) {
                header.internal_version = *v;
            }
        } else if (field->type == FourCC{"INCC"}) {
            auto v = body->get<std::uint32_t>();
            if (v) {
                header.unknown_incc = *v;
            }
        }
        // Unknown TES4 fields are skipped (subreader already advanced).
    }

    if (have_pending_master) {
        header.masters.push_back(std::move(pending_master));
    }
    return header;
}

io::ParseResult<Plugin> Plugin::open(const std::filesystem::path& path) {
    auto file = io::MappedFile::open(path);
    if (!file) {
        return std::unexpected(std::move(file).error());
    }

    Plugin plugin;
    plugin.file_ = std::move(*file);
    plugin.name_ = path.filename().string();

    auto reader = plugin.file_.reader();
    auto record = read_record_header(reader);
    if (!record) {
        return std::unexpected(std::move(record).error());
    }
    if (record->type != FourCC{"TES4"}) {
        return reader.fail(io::ErrorKind::bad_magic,
                           "first record is " + record->type.to_string() +
                               ", not TES4 -- this is not a plugin file");
    }
    // TES4 is never compressed.
    auto data = reader.subreader(record->data_size);
    if (!data) {
        return std::unexpected(std::move(data).error());
    }

    auto header = parse_plugin_header(*data, record->flags);
    if (!header) {
        return std::unexpected(std::move(header).error());
    }
    plugin.header_ = std::move(*header);
    plugin.body_offset_ = k_record_header_size + record->data_size;

    return plugin;
}

namespace {

/// Recursive walk over one group's payload.
///
/// Returns false only if the sink asked to stop. A parse failure abandons the
/// current group, since after a wrong size there is no way to resync.
bool scan_group_payload(io::SpanReader& reader, RecordSink& sink, ScanStats& stats,
                        std::vector<GroupContext>& stack) {
    while (!reader.at_end()) {
        // Peek the tag to pick the header layout; groups and records share
        // only the first four bytes.
        const auto tag = reader.peek_tag();
        if (!tag) {
            ++stats.errors;
            return sink.on_error(tag.error());
        }

        if (*tag == FourCC{"GRUP"}) {
            if (auto skip = reader.skip(4); !skip) {
                ++stats.errors;
                return sink.on_error(skip.error());
            }
            auto group = read_group_header_body(reader);
            if (!group) {
                ++stats.errors;
                return sink.on_error(group.error());
            }
            auto payload_size = group->payload_size(reader);
            if (!payload_size) {
                ++stats.errors;
                return sink.on_error(payload_size.error());
            }
            auto payload = reader.subreader(*payload_size);
            if (!payload) {
                ++stats.errors;
                return sink.on_error(payload.error());
            }

            ++stats.groups;
            if (stack.size() >= k_max_group_depth) {
                ++stats.errors;
                const auto err = reader.fail(
                    io::ErrorKind::corrupt,
                    "GRUP nesting deeper than " + std::to_string(k_max_group_depth));
                if (!sink.on_error(err.error())) {
                    return false;
                }
                continue; // skip this group, continue with siblings
            }

            stack.push_back(GroupContext{.header = *group});
            sink.on_group_enter(stack.back());
            const bool keep_going = scan_group_payload(*payload, sink, stats, stack);
            sink.on_group_exit(stack.back());
            stack.pop_back();
            if (!keep_going) {
                return false;
            }
            continue;
        }

        // A record; read_record_header consumes the tag.
        auto header = read_record_header(reader);
        if (!header) {
            ++stats.errors;
            return sink.on_error(header.error());
        }
        auto raw = reader.subreader(header->data_size);
        if (!raw) {
            ++stats.errors;
            return sink.on_error(raw.error());
        }

        ++stats.records;
        stats.bytes_scanned += header->data_size;
        if (header->is_deleted()) {
            ++stats.deleted_records;
        }

        if (header->is_compressed()) {
            ++stats.compressed_records;
            // Compressed payload: uint32 uncompressed size, then a zlib stream.
            auto uncompressed_size = raw->get<std::uint32_t>();
            if (!uncompressed_size) {
                ++stats.errors;
                if (!sink.on_error(uncompressed_size.error())) {
                    return false;
                }
                continue;
            }
            auto rest = raw->bytes(raw->remaining());
            if (!rest) {
                ++stats.errors;
                if (!sink.on_error(rest.error())) {
                    return false;
                }
                continue;
            }
            auto inflated = io::inflate_exact(*rest, *uncompressed_size, reader.origin(),
                                              raw->absolute_position());
            if (!inflated) {
                ++stats.errors;
                if (!sink.on_error(inflated.error())) {
                    return false;
                }
                continue; // skip the record, not the file
            }
            stats.bytes_inflated += inflated->size();

            io::SpanReader data(*inflated, reader.origin());
            const RecordContext ctx{.header = *header, .groups = stack};
            sink.on_record(ctx, data);
        } else {
            const RecordContext ctx{.header = *header, .groups = stack};
            sink.on_record(ctx, *raw);
        }
    }
    return true;
}

} // namespace

ScanStats Plugin::scan(RecordSink& sink) const {
    ScanStats stats;
    auto reader = file_.reader();
    if (auto seek = reader.seek(body_offset_); !seek) {
        ++stats.errors;
        sink.on_error(seek.error());
        return stats;
    }

    std::vector<GroupContext> stack;
    stack.reserve(k_max_group_depth);
    scan_group_payload(reader, sink, stats, stack);
    return stats;
}

} // namespace bethconv::record
