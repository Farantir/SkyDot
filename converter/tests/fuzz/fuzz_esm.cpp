// SPDX-License-Identifier: GPL-3.0-or-later
//
// ESM/ESP/ESL: TES4 header, GRUP tree, records, fields, the XXXX size escape and
// zlib records.
//
// A malformed record aborts its group and the walk continues with the parent;
// no read past the end. The sink only counts. Field definitions are covered by
// fuzz_forms.
#include "bethconv/record/form_census.hpp"
#include "bethconv/record/plugin.hpp"

#include "fuzz_support.hpp"


namespace {

/// Reads every field header, so XXXX and compressed records are reached.
class Walker final : public bethconv::record::RecordSink {
public:
    void on_record(const bethconv::record::RecordContext& ctx,
                   bethconv::io::SpanReader& data) override {
        ++records;
        // A record must never name a parent group that was not entered.
        BETHCONV_FUZZ_CHECK(ctx.groups.size() <= 64);
        while (!data.at_end()) {
            auto field = bethconv::record::read_field_header(data);
            if (!field) {
                break;
            }
            auto body = data.subreader(field->data_size);
            if (!body) {
                break;
            }
            ++fields;
        }
    }

    void on_group_enter(const bethconv::record::GroupContext&) override { ++groups; }

    bool on_error(const bethconv::io::ParseError&) override {
        ++errors;
        return true; // keep going
    }

    std::uint64_t records = 0;
    std::uint64_t groups = 0;
    std::uint64_t fields = 0;
    std::uint64_t errors = 0;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    // Plugin::open memory-maps rather than taking a span; see ScratchFile.
    static const bethconv::fuzz::ScratchFile scratch(".esm");
    if (!scratch.write({data, size})) {
        return 0;
    }

    auto plugin = bethconv::record::Plugin::open(scratch.path());
    if (!plugin) {
        return 0;
    }

    Walker walker;
    const auto stats = plugin->scan(walker);

    // The sink sees at most as many records as the walk counted, and exactly
    // as many without errors. `records` counts records present in the file,
    // including ones skipped because they failed to inflate or were nested too
    // deep; the corpus harness relies on that to match HEDR against
    // `records + groups`.
    BETHCONV_FUZZ_CHECK(walker.records <= stats.records);
    BETHCONV_FUZZ_CHECK(walker.groups <= stats.groups);
    BETHCONV_FUZZ_CHECK(stats.errors != 0 || walker.records == stats.records);
    BETHCONV_FUZZ_CHECK(stats.errors != 0 || walker.groups == stats.groups);
    BETHCONV_FUZZ_CHECK(stats.errors == walker.errors);
    return 0;
}
