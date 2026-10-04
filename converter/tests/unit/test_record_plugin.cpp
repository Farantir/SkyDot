// SPDX-License-Identifier: GPL-3.0-or-later
//
// Record-layer tests over synthetic plugins. The corpus harness covers valid
// files; these focus on deliberately malformed ones.
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/histogram.hpp"
#include "bethconv/record/plugin.hpp"

#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace bethconv;
using namespace bethconv::test;

namespace {

/// Writes a byte stream to a temp file so Plugin::open can map it. The file
/// sits in a TempDir: ctest runs each test case as its own process, and a
/// per-process counter in a shared directory made two of them collide.
class TempPlugin {
public:
    explicit TempPlugin(std::span<const std::byte> bytes) : path_(dir_ / "plugin.esp") {
        std::ofstream out(path_, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    TempDir dir_;
    std::filesystem::path path_;
};

/// Collects everything it is given, so a test can assert on the traversal.
class CollectingSink final : public record::RecordSink {
public:
    struct Seen {
        std::string type;
        std::uint32_t form_id;
        std::size_t payload_size;
        std::size_t depth;
        std::string top_group;
    };

    void on_record(const record::RecordContext& ctx, io::SpanReader& data) override {
        records.push_back(Seen{
            .type = ctx.header.type.to_string(),
            .form_id = ctx.header.form_id.value,
            .payload_size = data.size(),
            .depth = ctx.groups.size(),
            .top_group = ctx.top_group_type().to_string(),
        });
    }
    bool on_error(const io::ParseError& error) override {
        errors.push_back(error);
        return keep_going;
    }

    std::vector<Seen> records;
    std::vector<io::ParseError> errors;
    bool keep_going = true;
};

} // namespace

TEST_CASE("a minimal plugin exposes its TES4 header", "[record][plugin]") {
    ByteWriter w;
    write_tes4(w, static_cast<std::uint32_t>(record::RecordFlag::master),
               {"Skyrim.esm", "Update.esm"});
    const TempPlugin file(w.span());

    auto plugin = record::Plugin::open(file.path());
    REQUIRE(plugin.has_value());
    CHECK(plugin->header().is_master());
    CHECK_FALSE(plugin->header().is_light());
    CHECK(plugin->header().author == "bethconv-tests");
    REQUIRE(plugin->header().masters.size() == 2);
    CHECK(plugin->header().masters[0].name == "Skyrim.esm");
    CHECK(plugin->header().masters[1].name == "Update.esm");
}

TEST_CASE("the ESL flag is read from the header, not the file extension",
          "[record][plugin]") {
    // 234 of 612 real plugins have the light flag, only 3 the .esl extension;
    // width must come from the flag (docs/format-notes/esm4-plugins.md).
    ByteWriter w;
    write_tes4(w, static_cast<std::uint32_t>(record::RecordFlag::light_master));
    const TempPlugin file(w.span()); // note: named .esp

    auto plugin = record::Plugin::open(file.path());
    REQUIRE(plugin.has_value());
    CHECK(plugin->header().is_light());
    CHECK(file.path().extension() == ".esp");
}

TEST_CASE("a file whose first record is not TES4 is rejected", "[record][plugin]") {
    ByteWriter w;
    ByteWriter empty;
    write_record(w, "STAT", 0x123, empty.span());
    const TempPlugin file(w.span());

    const auto plugin = record::Plugin::open(file.path());
    REQUIRE_FALSE(plugin.has_value());
    CHECK(plugin.error().kind == io::ErrorKind::bad_magic);
}

TEST_CASE("records inside nested groups carry their context", "[record][plugin]") {
    ByteWriter stat;
    ByteWriter name;
    name.zstring("Rock01");
    write_field(stat, "EDID", name);

    ByteWriter records;
    write_record(records, "STAT", 0x00000801, stat.span());
    write_record(records, "STAT", 0x00000802, stat.span());

    ByteWriter inner;
    write_group(inner, io::FourCC{"STAT"}.value, 0 /* top */, records.span());

    ByteWriter w;
    write_tes4(w);
    w.raw(inner.span());
    const TempPlugin file(w.span());

    auto plugin = record::Plugin::open(file.path());
    REQUIRE(plugin.has_value());

    CollectingSink sink;
    const auto stats = plugin->scan(sink);
    CHECK(stats.records == 2);
    CHECK(stats.groups == 1);
    CHECK(stats.errors == 0);

    REQUIRE(sink.records.size() == 2);
    CHECK(sink.records[0].type == "STAT");
    CHECK(sink.records[0].form_id == 0x00000801);
    CHECK(sink.records[0].depth == 1);
    CHECK(sink.records[0].top_group == "STAT");
    CHECK(sink.records[1].form_id == 0x00000802);
}

TEST_CASE("a compressed record is inflated transparently", "[record][plugin]") {
    ByteWriter payload;
    ByteWriter name;
    // Long and repetitive so it compresses.
    name.zstring(std::string(400, 'a'));
    write_field(payload, "EDID", name);

    const auto compressed = compress_payload(payload.span());
    REQUIRE_FALSE(compressed.empty());

    ByteWriter records;
    write_record(records, "NPC_", 0x00000900, compressed,
                 static_cast<std::uint32_t>(record::RecordFlag::compressed));

    ByteWriter group;
    write_group(group, io::FourCC{"NPC_"}.value, 0, records.span());

    ByteWriter w;
    write_tes4(w);
    w.raw(group.span());
    const TempPlugin file(w.span());

    auto plugin = record::Plugin::open(file.path());
    REQUIRE(plugin.has_value());

    CollectingSink sink;
    const auto stats = plugin->scan(sink);
    CHECK(stats.compressed_records == 1);
    CHECK(stats.errors == 0);
    REQUIRE(sink.records.size() == 1);
    // The sink sees the inflated payload, not the deflated bytes.
    CHECK(sink.records[0].payload_size == payload.size());
    CHECK(stats.bytes_inflated == payload.size());
}

TEST_CASE("a compressed record lying about its size is skipped, not trusted",
          "[record][plugin]") {
    ByteWriter payload;
    ByteWriter name;
    name.zstring(std::string(400, 'a'));
    write_field(payload, "EDID", name);

    auto compressed = compress_payload(payload.span());
    REQUIRE(compressed.size() > 4);
    // Declare a much larger uncompressed size than the stream produces.
    ByteWriter tampered;
    tampered.u32(0x00100000);
    tampered.raw(std::span<const std::byte>(compressed).subspan(4));

    ByteWriter records;
    write_record(records, "NPC_", 0x00000901, tampered.span(),
                 static_cast<std::uint32_t>(record::RecordFlag::compressed));

    ByteWriter group;
    write_group(group, io::FourCC{"NPC_"}.value, 0, records.span());

    ByteWriter w;
    write_tes4(w);
    w.raw(group.span());
    const TempPlugin file(w.span());

    auto plugin = record::Plugin::open(file.path());
    REQUIRE(plugin.has_value());

    CollectingSink sink;
    const auto stats = plugin->scan(sink);
    CHECK(stats.errors == 1);
    CHECK(sink.records.empty());          // the record is dropped ...
    REQUIRE(sink.errors.size() == 1);
    CHECK(sink.errors[0].kind == io::ErrorKind::corrupt);
}

TEST_CASE("a corrupt group costs that group, not the rest of the file",
          "[record][plugin]") {
    // Two sibling top groups; the first has a record whose payload runs past
    // the end of its group.
    ByteWriter bad_records;
    bad_records.tag("STAT");
    bad_records.u32(0xFFFF);  // payload far larger than what follows
    bad_records.u32(0);
    bad_records.u32(0x00000801);
    bad_records.u32(0);
    bad_records.u16(44);
    bad_records.u16(0);
    bad_records.raw("short");

    ByteWriter bad_group;
    write_group(bad_group, io::FourCC{"STAT"}.value, 0, bad_records.span());

    ByteWriter good_records;
    ByteWriter empty;
    write_record(good_records, "DOOR", 0x00000802, empty.span());
    ByteWriter good_group;
    write_group(good_group, io::FourCC{"DOOR"}.value, 0, good_records.span());

    ByteWriter w;
    write_tes4(w);
    w.raw(bad_group.span());
    w.raw(good_group.span());
    const TempPlugin file(w.span());

    auto plugin = record::Plugin::open(file.path());
    REQUIRE(plugin.has_value());

    CollectingSink sink;
    const auto stats = plugin->scan(sink);
    CHECK(stats.errors == 1);
    // The DOOR in the following group must still be found.
    REQUIRE(sink.records.size() == 1);
    CHECK(sink.records[0].type == "DOOR");
    CHECK(sink.records[0].form_id == 0x00000802);
}

TEST_CASE("a sink can stop the scan", "[record][plugin]") {
    ByteWriter bad_records;
    bad_records.tag("STAT");
    bad_records.u32(0xFFFF);
    bad_records.u32(0);
    bad_records.u32(1);
    bad_records.u32(0);
    bad_records.u16(44);
    bad_records.u16(0);

    ByteWriter bad_group;
    write_group(bad_group, io::FourCC{"STAT"}.value, 0, bad_records.span());

    ByteWriter good_records;
    ByteWriter empty;
    write_record(good_records, "DOOR", 2, empty.span());
    ByteWriter good_group;
    write_group(good_group, io::FourCC{"DOOR"}.value, 0, good_records.span());

    ByteWriter w;
    write_tes4(w);
    w.raw(bad_group.span());
    w.raw(good_group.span());
    const TempPlugin file(w.span());

    auto plugin = record::Plugin::open(file.path());
    REQUIRE(plugin.has_value());

    CollectingSink sink;
    sink.keep_going = false;
    const auto stats = plugin->scan(sink);
    CHECK(stats.errors == 1);
    CHECK(sink.records.empty());
}

TEST_CASE("a GRUP smaller than its own header is rejected", "[record][headers]") {
    ByteWriter w;
    write_tes4(w);
    w.tag("GRUP");
    w.u32(8); // less than the 24-byte header
    w.u32(0);
    w.u32(0);
    w.u16(0);
    w.u16(0);
    w.u16(0);
    w.u16(0);
    const TempPlugin file(w.span());

    auto plugin = record::Plugin::open(file.path());
    REQUIRE(plugin.has_value());

    CollectingSink sink;
    const auto stats = plugin->scan(sink);
    CHECK(stats.errors == 1);
    REQUIRE(sink.errors.size() == 1);
    CHECK(sink.errors[0].kind == io::ErrorKind::bad_value);
}

TEST_CASE("the XXXX escape widens the following field's size", "[record][headers]") {
    // Fields over 64 KiB: an XXXX field carries the real uint32 size and the
    // next field's own size is 0 (large NAVM and LAND).
    const std::vector<std::byte> big(70000, std::byte{0xAB});

    ByteWriter payload;
    ByteWriter xxxx;
    xxxx.u32(static_cast<std::uint32_t>(big.size()));
    write_field(payload, "XXXX", xxxx);
    payload.tag("NVNM");
    payload.u16(0); // placeholder size
    payload.raw(std::span<const std::byte>(big));

    io::SpanReader reader(payload.span(), "synthetic");
    std::vector<std::pair<std::string, std::size_t>> fields;
    const auto walk = record::for_each_field(
        reader, [&](const record::FieldHeader& header, io::SpanReader& body) {
            fields.emplace_back(header.type.to_string(), body.size());
        });

    REQUIRE(walk.has_value());
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].first == "NVNM");
    CHECK(fields[0].second == big.size());
}

TEST_CASE("a field running past the record payload is caught", "[record][headers]") {
    ByteWriter payload;
    payload.tag("EDID");
    payload.u16(500); // more than what follows
    payload.raw("short");

    io::SpanReader reader(payload.span(), "synthetic");
    const auto walk = record::for_each_field(
        reader, [](const record::FieldHeader&, io::SpanReader&) {});

    REQUIRE_FALSE(walk.has_value());
    CHECK(walk.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("a trailing runt too small for a field header is corruption",
          "[record][headers]") {
    ByteWriter payload;
    ByteWriter name;
    name.zstring("ok");
    write_field(payload, "EDID", name);
    payload.raw("xy"); // 2 stray bytes, less than a 6-byte field header

    io::SpanReader reader(payload.span(), "synthetic");
    const auto walk = record::for_each_field(
        reader, [](const record::FieldHeader&, io::SpanReader&) {});

    REQUIRE_FALSE(walk.has_value());
    CHECK(walk.error().kind == io::ErrorKind::truncated);
}

TEST_CASE("the histogram counts types and field coverage", "[record][histogram]") {
    ByteWriter stat_payload;
    ByteWriter name;
    name.zstring("Rock01");
    write_field(stat_payload, "EDID", name);

    ByteWriter records;
    write_record(records, "STAT", 1, stat_payload.span());
    write_record(records, "STAT", 2, stat_payload.span());
    write_record(records, "DOOR", 3, stat_payload.span());

    ByteWriter group;
    write_group(group, io::FourCC{"STAT"}.value, 0, records.span());

    ByteWriter w;
    write_tes4(w);
    w.raw(group.span());
    const TempPlugin file(w.span());

    auto plugin = record::Plugin::open(file.path());
    REQUIRE(plugin.has_value());

    record::Histogram histogram;
    const auto scanned = plugin->scan(histogram);
    CHECK(scanned.errors == 0);

    CHECK(histogram.total_records() == 3);
    CHECK(histogram.field_coverage() == 1.0);
    const auto& types = histogram.types();
    REQUIRE(types.contains(io::FourCC{"STAT"}.value));
    CHECK(types.at(io::FourCC{"STAT"}.value).count == 2);
    CHECK(types.at(io::FourCC{"DOOR"}.value).count == 1);
    CHECK(types.at(io::FourCC{"STAT"}.value).fields.at(io::FourCC{"EDID"}.value) == 2);

    const auto report = histogram.report(4);
    CHECK(report.find("STAT") != std::string::npos);
    CHECK(report.find("EDID") != std::string::npos);
}

// ---- the TES4 header -------------------------------------------------------

TEST_CASE("a HEDR shorter than its three fields fails cleanly", "[record][plugin]") {
    // HEDR is a float, an int32 and a uint32. Checking them together
    // (`if (!version || !count || !next_id) return version.error();`) reads the
    // error of a successful expected when a later read failed, which is UB.
    // Found by fuzz_esm on a 145-byte file.
    //
    // Every truncation must return an error, never abort. (Without
    // _GLIBCXX_ASSERTIONS the old bug may still pass.)
    //
    // Removing the `count` check does not fail this test: the reads are
    // sequential, so no HEDR length makes `count` fail while `next_id` would
    // succeed. The check stays because it reports the right offset.
    for (const std::size_t hedr_bytes : {std::size_t{0}, std::size_t{4}, std::size_t{8},
                                         std::size_t{11}}) {
        INFO("HEDR of " << hedr_bytes << " bytes");
        bethconv::test::ByteWriter payload;
        bethconv::test::ByteWriter hedr;
        // A full HEDR is 12 bytes; write a prefix.
        hedr.f32(1.70F);
        hedr.u32(0);
        hedr.u32(0x800);
        const auto full = hedr.bytes();
        bethconv::test::ByteWriter cut;
        for (std::size_t i = 0; i < hedr_bytes; ++i) {
            cut.u8(static_cast<std::uint8_t>(full[i]));
        }
        bethconv::test::write_field(payload, "HEDR", cut);

        auto reader = bethconv::io::SpanReader(payload.span(), "fixture.esm");
        const auto header = bethconv::record::parse_plugin_header(reader, 0);
        REQUIRE_FALSE(header.has_value());
        CHECK(header.error().kind == bethconv::io::ErrorKind::truncated);
    }
}

TEST_CASE("a full HEDR still reads all three fields", "[record][plugin]") {
    bethconv::test::ByteWriter payload;
    bethconv::test::ByteWriter hedr;
    hedr.f32(1.71F);
    hedr.u32(1234);
    hedr.u32(0x0000'0800);
    bethconv::test::write_field(payload, "HEDR", hedr);

    auto reader = bethconv::io::SpanReader(payload.span(), "fixture.esm");
    const auto header = bethconv::record::parse_plugin_header(reader, 0);
    REQUIRE(header.has_value());
    CHECK(header->version == 1.71F);
    CHECK(header->record_count == 1234);
    CHECK(header->next_object_id == 0x0000'0800);
}
