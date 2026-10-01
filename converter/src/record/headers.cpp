// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/headers.hpp"

namespace bethconv::record {

io::ParseResult<std::uint32_t> GroupHeader::payload_size(const io::SpanReader& at) const {
    if (group_size < k_group_header_size) {
        return at.fail(io::ErrorKind::bad_value,
                       "GRUP declares size " + std::to_string(group_size) +
                           " which is smaller than its own " +
                           std::to_string(k_group_header_size) + "-byte header");
    }
    return group_size - static_cast<std::uint32_t>(k_group_header_size);
}

io::ParseResult<RecordHeader> read_record_header(io::SpanReader& reader) {
    RecordHeader header;

    auto type = reader.tag();
    if (!type) {
        return std::unexpected(std::move(type).error());
    }
    header.type = *type;

    auto data_size = reader.get<std::uint32_t>();
    if (!data_size) {
        return std::unexpected(std::move(data_size).error());
    }
    header.data_size = *data_size;

    auto flags = reader.get<std::uint32_t>();
    if (!flags) {
        return std::unexpected(std::move(flags).error());
    }
    header.flags = *flags;

    auto form_id = reader.get<std::uint32_t>();
    if (!form_id) {
        return std::unexpected(std::move(form_id).error());
    }
    header.form_id = FormId{*form_id};

    auto revision = reader.get<std::uint32_t>();
    if (!revision) {
        return std::unexpected(std::move(revision).error());
    }
    header.revision = *revision;

    auto version = reader.get<std::uint16_t>();
    if (!version) {
        return std::unexpected(std::move(version).error());
    }
    header.version = *version;

    auto unknown = reader.get<std::uint16_t>();
    if (!unknown) {
        return std::unexpected(std::move(unknown).error());
    }
    header.unknown = *unknown;

    if (header.data_size > k_max_record_data_size) {
        return reader.fail(io::ErrorKind::too_large,
                           header.type.to_string() + " " + header.form_id.to_string() +
                               " declares " + std::to_string(header.data_size) +
                               " bytes of data");
    }
    return header;
}

io::ParseResult<GroupHeader> read_group_header_body(io::SpanReader& reader) {
    GroupHeader header;

    auto group_size = reader.get<std::uint32_t>();
    if (!group_size) {
        return std::unexpected(std::move(group_size).error());
    }
    header.group_size = *group_size;

    auto label = reader.get<std::uint32_t>();
    if (!label) {
        return std::unexpected(std::move(label).error());
    }
    header.label = GroupLabel{*label};

    auto group_type = reader.get<std::int32_t>();
    if (!group_type) {
        return std::unexpected(std::move(group_type).error());
    }
    // Unknown group types are kept: skipping only needs group_size, and an
    // unknown type should cost that group, not the file.
    header.group_type = static_cast<GroupType>(*group_type);

    auto stamp = reader.get<std::uint16_t>();
    if (!stamp) {
        return std::unexpected(std::move(stamp).error());
    }
    header.stamp = *stamp;

    auto unknown = reader.get<std::uint16_t>();
    if (!unknown) {
        return std::unexpected(std::move(unknown).error());
    }
    header.unknown = *unknown;

    auto version = reader.get<std::uint16_t>();
    if (!version) {
        return std::unexpected(std::move(version).error());
    }
    header.version = *version;

    auto unknown2 = reader.get<std::uint16_t>();
    if (!unknown2) {
        return std::unexpected(std::move(unknown2).error());
    }
    header.unknown2 = *unknown2;

    return header;
}

io::ParseResult<FieldHeader> read_field_header(io::SpanReader& reader) {
    auto type = reader.tag();
    if (!type) {
        return std::unexpected(std::move(type).error());
    }

    auto size = reader.get<std::uint16_t>();
    if (!size) {
        return std::unexpected(std::move(size).error());
    }

    if (*type != FourCC{"XXXX"}) {
        return FieldHeader{.type = *type, .data_size = *size};
    }

    // XXXX: the payload is a uint32 with the size of the next field, so its own
    // size must be 4.
    if (*size != 4) {
        return reader.fail(io::ErrorKind::bad_value,
                           "XXXX size-override field is " + std::to_string(*size) +
                               " bytes, expected 4");
    }
    auto real_size = reader.get<std::uint32_t>();
    if (!real_size) {
        return std::unexpected(std::move(real_size).error());
    }

    auto next_type = reader.tag();
    if (!next_type) {
        return std::unexpected(std::move(next_type).error());
    }
    auto placeholder = reader.get<std::uint16_t>();
    if (!placeholder) {
        return std::unexpected(std::move(placeholder).error());
    }
    if (*placeholder != 0) {
        return reader.fail(io::ErrorKind::bad_value,
                           "field after XXXX has non-zero size " +
                               std::to_string(*placeholder));
    }
    if (*next_type == FourCC{"XXXX"}) {
        return reader.fail(io::ErrorKind::corrupt, "XXXX chained to another XXXX");
    }
    return FieldHeader{.type = *next_type, .data_size = *real_size};
}

} // namespace bethconv::record
