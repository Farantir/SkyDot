// SPDX-License-Identifier: GPL-3.0-or-later
//
// Record snapshot tests.
//
// Writing is checked at scale by the corpus harness; here the special case is a
// persistent cell sharing grid (0, 0) with a real cell.
//
// Reading is untrusted input, so the error cases matter most: headers lying
// about sections, an index that fails verification, payload offsets past the
// blob. None may crash or be accepted.
#include "bethconv/pack/snapshot.hpp"

#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace bethconv;
using bethconv::test::ByteWriter;
using bethconv::test::TempDir;

namespace {

constexpr std::uint32_t k_flag_master = 0x0000'0001;
constexpr std::uint32_t k_flag_persistent = 0x0000'0400;

/// EDID, and for a CELL an XCLC, in the layout a real record has them.
ByteWriter cell_payload(std::string_view editor_id, std::int32_t x, std::int32_t y) {
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring(editor_id);
    bethconv::test::write_field(payload, "EDID", edid);
    ByteWriter xclc;
    xclc.u32(static_cast<std::uint32_t>(x));
    xclc.u32(static_cast<std::uint32_t>(y));
    xclc.u32(0); // land/water flags, unused here
    bethconv::test::write_field(payload, "XCLC", xclc);
    return payload;
}

ByteWriter edid_payload(std::string_view editor_id) {
    ByteWriter payload;
    ByteWriter edid;
    edid.zstring(editor_id);
    bethconv::test::write_field(payload, "EDID", edid);
    return payload;
}

std::uint32_t tag_value(std::string_view type) {
    ByteWriter w;
    w.tag(type);
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        raw |= static_cast<std::uint32_t>(static_cast<unsigned char>(w.bytes()[i])) << (i * 8);
    }
    return raw;
}

/// A CELL with temporary children, nested as in a worldspace:
/// `CELL > GRUP cell_children(CELL) > GRUP cell_temporary_children`.
void write_cell_with_children(ByteWriter& out, std::uint32_t cell_form,
                              const ByteWriter& payload, std::uint32_t cell_flags,
                              const std::vector<std::pair<std::uint32_t, std::string>>& refrs) {
    bethconv::test::write_record(out, "CELL", cell_form, payload.span(), cell_flags);

    ByteWriter children;
    for (const auto& [form, editor_id] : refrs) {
        bethconv::test::write_record(children, "REFR", form, edid_payload(editor_id).span());
    }
    ByteWriter temporary;
    bethconv::test::write_group(temporary, cell_form, 9, children.span());
    ByteWriter cell_children;
    bethconv::test::write_group(cell_children, cell_form, 6, temporary.span());
    out.raw(cell_children.span());
}

/// A master with one worldspace: two exterior cells on different squares, a
/// persistent cell at (0, 0), a real cell also at (0, 0), and some top-level
/// records. Only the persistent flag tells the (0, 0) pair apart.
void make_world_master(const TempDir& dir, std::string_view name) {
    ByteWriter cells;
    write_cell_with_children(cells, 0x0000'0002, cell_payload("RealOrigin", 0, 0), 0,
                             {{0x0000'0003, "RefrAtOrigin"}});
    write_cell_with_children(cells, 0x0000'0004, cell_payload("EastCell", 3, -4), 0,
                             {{0x0000'0005, "RefrEast"}, {0x0000'0006, "RefrEastToo"}});
    write_cell_with_children(cells, 0x0000'0007, cell_payload("Persistent", 0, 0),
                             k_flag_persistent, {{0x0000'0008, "PersistentRefr"}});

    ByteWriter sub_block;
    bethconv::test::write_group(sub_block, 0, 5, cells.span());
    ByteWriter block;
    bethconv::test::write_group(block, 0, 4, sub_block.span());

    ByteWriter world_and_children;
    bethconv::test::write_record(world_and_children, "WRLD", 0x0000'0001,
                                 edid_payload("TestWorld").span());
    ByteWriter world_children;
    bethconv::test::write_group(world_children, 0x0000'0001, 1, block.span());
    world_and_children.raw(world_children.span());

    ByteWriter file;
    bethconv::test::write_tes4(file, k_flag_master, {});
    bethconv::test::write_group(file, tag_value("WRLD"), 0, world_and_children.span());

    ByteWriter stats;
    bethconv::test::write_record(stats, "STAT", 0x0000'0020, edid_payload("BaseStatic").span());
    bethconv::test::write_record(stats, "STAT", 0x0000'0021, edid_payload("OtherStatic").span());
    bethconv::test::write_group(file, tag_value("STAT"), 0, stats.span());

    std::ofstream out(dir / name, std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

/// A plugin overriding one of the master's statics (winner != owner).
void make_patch(const TempDir& dir, std::string_view name, std::string_view master) {
    ByteWriter file;
    bethconv::test::write_tes4(file, 0, {std::string(master)});
    ByteWriter stats;
    bethconv::test::write_record(stats, "STAT", 0x0000'0020, edid_payload("PatchedStatic").span());
    bethconv::test::write_group(file, tag_value("STAT"), 0, stats.span());

    std::ofstream out(dir / name, std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

record::PluginList listed(const std::vector<std::string>& names) {
    record::PluginList list;
    for (const auto& name : names) {
        list.plugins.push_back(record::ListedPlugin{.name = name, .active = true});
    }
    return list;
}

/// Build the fixture order, merge it, and write a snapshot to `out`.
pack::SnapshotStats build_snapshot(const TempDir& dir, const std::filesystem::path& out) {
    make_world_master(dir, "Master.esm");
    make_patch(dir, "Patch.esp", "Master.esm");
    const auto order = record::LoadOrder::build(
        dir.path(), listed({"Master.esm", "Patch.esp"}),
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false});
    REQUIRE(order.problems().empty());
    const auto world = record::MergedWorld::build(order);
    const auto stats = pack::write_snapshot(world, order, out);
    REQUIRE(stats.has_value());
    return *stats;
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<std::byte> bytes;
    char c = 0;
    while (in.get(c)) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

/// Overwrite a little-endian value, for the header tests.
void poke64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
    }
}

void poke32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
    }
}

// Header offsets, written out rather than taken from the writer so the test
// cannot share its mistakes.
constexpr std::size_t k_off_version = 8;
constexpr std::size_t k_off_fb_offset = 16;
constexpr std::size_t k_off_fb_bytes = 24;
constexpr std::size_t k_off_blob_bytes = 40;
constexpr std::size_t k_off_form_count = 48;

} // namespace

// ---- the round trip -------------------------------------------------------

TEST_CASE("a snapshot round-trips the merged world", "[pack][snapshot]") {
    TempDir dir;
    const auto path = dir / "records.fb";
    const auto stats = build_snapshot(dir, path);

    // WRLD, three CELLs, four REFRs, two STATs; the patch's override collapses
    // into the master's STAT.
    CHECK(stats.forms == 10);
    CHECK(stats.file_bytes > 0);
    CHECK(stats.unwalkable == 0);

    const auto snapshot = pack::Snapshot::open(path);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->format_version() == pack::k_snapshot_format_version);
    CHECK(snapshot->size() == stats.forms);
    CHECK(snapshot->plugins().size() == 2);
    CHECK(snapshot->language() == record::k_default_language);
    CHECK(snapshot->blob_intact());

    // Sorted and unique, as `find` requires.
    for (std::size_t i = 1; i < snapshot->size(); ++i) {
        INFO("at index " << i);
        CHECK(snapshot->at(i - 1)->id.value < snapshot->at(i)->id.value);
    }

    // Provenance: won by the patch, owned by the master, one override.
    const auto patched = snapshot->find(record::FormId{0x0000'0020});
    REQUIRE(patched.has_value());
    CHECK(patched->type == io::FourCC{"STAT"});
    CHECK(patched->overrides == 1);
    CHECK(patched->winner == 1);
    CHECK(patched->owner == 0);
    CHECK_FALSE(patched->deleted);
    CHECK_FALSE(patched->injected);

    // The winner's payload is stored; no count would catch the wrong one.
    CHECK(snapshot->editor_id_of(record::FormId{0x0000'0020}) == "PatchedStatic");
    CHECK(snapshot->find_editor_id("PatchedStatic") == record::FormId{0x0000'0020});
    CHECK_FALSE(snapshot->find_editor_id("BaseStatic").has_value());

    CHECK(snapshot->find_editor_id("OtherStatic") == record::FormId{0x0000'0021});
    CHECK_FALSE(snapshot->find_editor_id("NoSuchThing").has_value());
}

TEST_CASE("the indices agree with the form array", "[pack][snapshot]") {
    TempDir dir;
    const auto path = dir / "records.fb";
    (void)build_snapshot(dir, path);
    const auto snapshot = pack::Snapshot::open(path);
    REQUIRE(snapshot.has_value());

    CHECK(snapshot->of_type(io::FourCC{"WRLD"}).size() == 1);
    CHECK(snapshot->of_type(io::FourCC{"CELL"}).size() == 3);
    CHECK(snapshot->of_type(io::FourCC{"REFR"}).size() == 4);
    CHECK(snapshot->of_type(io::FourCC{"STAT"}).size() == 2);
    // A missing type gives an empty result, not an error.
    CHECK(snapshot->of_type(io::FourCC{"NAVM"}).empty());

    // Every listed form has that type.
    for (const auto id : snapshot->of_type(io::FourCC{"CELL"})) {
        const auto form = snapshot->find(record::FormId{id});
        REQUIRE(form.has_value());
        CHECK(form->type == io::FourCC{"CELL"});
    }

    // Parent chain REFR -> CELL -> WRLD (innermost group).
    const auto refr = snapshot->find(record::FormId{0x0000'0005});
    REQUIRE(refr.has_value());
    CHECK(refr->parent == record::FormId{0x0000'0004});
    const auto cell = snapshot->find(record::FormId{0x0000'0004});
    REQUIRE(cell.has_value());
    CHECK(cell->parent == record::FormId{0x0000'0001});

    const auto contents = snapshot->children_of(record::FormId{0x0000'0004});
    REQUIRE(contents.size() == 2);
    CHECK(contents[0] == 0x0000'0005);
    CHECK(contents[1] == 0x0000'0006);

    // No children, or not a parent at all: empty span.
    CHECK(snapshot->children_of(record::FormId{0x0000'0005}).empty());
    CHECK(snapshot->children_of(record::FormId{0xDEAD'BEEF}).empty());

    // All three cells, including the persistent one missing from the grid.
    CHECK(snapshot->children_of(record::FormId{0x0000'0001}).size() == 3);
}

// ---- persistent cell at (0, 0) --------------------------------------------

TEST_CASE("the grid index skips the worldspace's persistent cell", "[pack][snapshot]") {
    TempDir dir;
    const auto path = dir / "records.fb";
    const auto stats = build_snapshot(dir, path);

    // Three exterior CELLs, two squares.
    CHECK(stats.grid_cells == 2);
    CHECK(stats.grid_duplicates == 0);

    const auto snapshot = pack::Snapshot::open(path);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->worlds().size() == 1);
    CHECK(snapshot->worlds()[0] == record::FormId{0x0000'0001});

    const auto world = record::FormId{0x0000'0001};
    // (0, 0) is the real cell. Without the rule it would be 0x00000007, with
    // all counts still correct.
    CHECK(snapshot->cell_at(world, 0, 0) == record::FormId{0x0000'0002});
    CHECK(snapshot->cell_at(world, 3, -4) == record::FormId{0x0000'0004});
    // Negative coordinates: a sign leaking between the packed halves would
    // break the sort.
    CHECK_FALSE(snapshot->cell_at(world, -3, 4).has_value());
    CHECK_FALSE(snapshot->cell_at(world, 99, 99).has_value());
    CHECK_FALSE(snapshot->cell_at(record::FormId{0xDEAD'BEEF}, 0, 0).has_value());

    // Left out of the grid but kept in the world, with its references.
    const auto persistent = snapshot->find(record::FormId{0x0000'0007});
    REQUIRE(persistent.has_value());
    CHECK(persistent->parent == world);
    CHECK(snapshot->children_of(record::FormId{0x0000'0007}).size() == 1);
}

// ---- tampered files -------------------------------------------------------

TEST_CASE("a snapshot that is not one is refused", "[pack][snapshot]") {
    CHECK_FALSE(pack::Snapshot::from_bytes({}, "empty").has_value());

    const std::vector<std::byte> runt(16, std::byte{0});
    CHECK_FALSE(pack::Snapshot::from_bytes(runt, "runt").has_value());

    std::vector<std::byte> wrong(128, std::byte{0});
    const std::string_view magic = "NOTAPACK";
    for (std::size_t i = 0; i < magic.size(); ++i) {
        wrong[i] = static_cast<std::byte>(magic[i]);
    }
    const auto refused = pack::Snapshot::from_bytes(wrong, "wrong-magic");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == io::ErrorKind::bad_magic);
}

TEST_CASE("a header that lies about its own sections is refused", "[pack][snapshot]") {
    TempDir dir;
    const auto path = dir / "records.fb";
    (void)build_snapshot(dir, path);
    const auto original = read_file(path);
    REQUIRE(pack::Snapshot::from_bytes(original, "good").has_value());

    SECTION("a format version this build does not read") {
        auto bytes = original;
        poke32(bytes, k_off_version, pack::k_snapshot_format_version + 1);
        const auto opened = pack::Snapshot::from_bytes(bytes, "future");
        REQUIRE_FALSE(opened.has_value());
        // Version mismatch: a clear error.
        CHECK(opened.error().kind == io::ErrorKind::unsupported);
    }

    SECTION("an index longer than the file") {
        auto bytes = original;
        poke64(bytes, k_off_fb_bytes, 1ULL << 40);
        const auto opened = pack::Snapshot::from_bytes(bytes, "long-index");
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().kind == io::ErrorKind::out_of_range);
    }

    SECTION("an index that starts past the end") {
        auto bytes = original;
        poke64(bytes, k_off_fb_offset, bytes.size() + 4096);
        CHECK_FALSE(pack::Snapshot::from_bytes(bytes, "far-index").has_value());
    }

    SECTION("a blob longer than the file") {
        auto bytes = original;
        poke64(bytes, k_off_blob_bytes, 1ULL << 40);
        CHECK_FALSE(pack::Snapshot::from_bytes(bytes, "long-blob").has_value());
    }

    SECTION("a form count that disagrees with the index") {
        // The form count is stored twice; a mismatch is rejected.
        auto bytes = original;
        poke64(bytes, k_off_form_count, 999999);
        const auto opened = pack::Snapshot::from_bytes(bytes, "miscounted");
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().kind == io::ErrorKind::corrupt);
    }
}

TEST_CASE("a truncated snapshot is refused rather than read", "[pack][snapshot]") {
    TempDir dir;
    const auto path = dir / "records.fb";
    (void)build_snapshot(dir, path);
    const auto original = read_file(path);

    // Every prefix (step 17). Most fail the header bounds check, the rest in the
    // verifier; none may crash (checked under ASan).
    for (std::size_t keep = 0; keep < original.size(); keep += 17) {
        INFO("first " << keep << " bytes of " << original.size());
        const std::vector<std::byte> prefix(original.begin(),
                                            original.begin() + static_cast<long>(keep));
        CHECK_FALSE(pack::Snapshot::from_bytes(prefix, "truncated").has_value());
    }
}

TEST_CASE("an index that does not verify is refused", "[pack][snapshot]") {
    TempDir dir;
    const auto path = dir / "records.fb";
    (void)build_snapshot(dir, path);
    auto bytes = read_file(path);

    // Corrupt the FlatBuffer's root offset: the case the verifier is for.
    std::uint64_t fb_offset = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        fb_offset |= static_cast<std::uint64_t>(bytes[k_off_fb_offset + i]) << (i * 8);
    }
    REQUIRE(fb_offset + 8 < bytes.size());
    poke32(bytes, static_cast<std::size_t>(fb_offset), 0x7FFF'FFFFU);

    const auto opened = pack::Snapshot::from_bytes(bytes, "scribbled");
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().kind == io::ErrorKind::corrupt);
}

TEST_CASE("a changed payload byte moves the blob hash", "[pack][snapshot]") {
    TempDir dir;
    const auto path = dir / "records.fb";
    (void)build_snapshot(dir, path);
    auto bytes = read_file(path);

    // First blob byte, right after the header.
    bytes[64] = static_cast<std::byte>(static_cast<std::uint8_t>(bytes[64]) ^ 0xFF);

    // Still opens: `open` does not hash the blob; `verify --deep` does.
    const auto opened = pack::Snapshot::from_bytes(bytes, "tampered");
    REQUIRE(opened.has_value());
    CHECK_FALSE(opened->blob_intact());
}

// ---- verbatim payloads ----------------------------------------------------
//
// pack-format.md v1 promises a form's payload is the winning record's entire
// field block, including fields nothing decodes (REFR alone has 47 such types;
// CELL 9, WRLD 11). The writer does this today because it never consults a
// definition; this test fails if it ever starts keeping only known fields.
namespace {

/// EDID plus four undecoded fields: two Bethesda ones, the Papyrus blob, and a
/// made-up tag.
ByteWriter undecoded_payload() {
    ByteWriter payload;

    ByteWriter edid;
    edid.zstring("VerbatimStatic");
    bethconv::test::write_field(payload, "EDID", edid);

    // XRGD: ragdoll state (8,601 in vanilla), never read.
    ByteWriter xrgd;
    for (std::uint32_t i = 0; i < 6; ++i) {
        xrgd.f32(static_cast<float>(i) * 1.5F);
    }
    bethconv::test::write_field(payload, "XRGD", xrgd);

    // VMAD: kept raw on every type, never parsed. Contents deliberately invalid.
    ByteWriter vmad;
    vmad.u16(5);
    vmad.u16(2);
    vmad.raw(std::as_bytes(std::span{"\x00\xFF\x7F\x01", 4}).first(4));
    bethconv::test::write_field(payload, "VMAD", vmad);

    ByteWriter xapr;
    xapr.u32(0x0000'0020);
    xapr.f32(0.25F);
    bethconv::test::write_field(payload, "XAPR", xapr);

    // An unknown tag; would be dropped by any whitelist.
    ByteWriter zzzz;
    zzzz.raw(std::as_bytes(std::span{"not a field anyone defines", 26}).first(26));
    bethconv::test::write_field(payload, "ZZZZ", zzzz);

    return payload;
}

/// One plugin with one STAT carrying `undecoded_payload()`. `compressed` goes
/// through zlib, where "verbatim" means the inflated block.
void make_verbatim_master(const TempDir& dir, std::string_view name, bool compressed) {
    const auto payload = undecoded_payload();

    ByteWriter stats;
    if (compressed) {
        const auto deflated = bethconv::test::compress_payload(payload.span());
        REQUIRE_FALSE(deflated.empty());
        bethconv::test::write_record(stats, "STAT", 0x0000'0030,
                                     std::span<const std::byte>(deflated),
                                     static_cast<std::uint32_t>(record::RecordFlag::compressed));
    } else {
        bethconv::test::write_record(stats, "STAT", 0x0000'0030, payload.span());
    }

    ByteWriter file;
    bethconv::test::write_tes4(file, k_flag_master, {});
    bethconv::test::write_group(file, tag_value("STAT"), 0, stats.span());

    std::ofstream out(dir / name, std::ios::binary);
    for (const auto b : file.bytes()) {
        out.put(static_cast<char>(b));
    }
}

} // namespace

TEST_CASE("a payload keeps the fields no definition decodes", "[pack][snapshot]") {
    const bool compressed = GENERATE(false, true);
    INFO("compressed record: " << compressed);

    TempDir dir;
    const auto path = dir / "records.fb";
    make_verbatim_master(dir, "Verbatim.esm", compressed);

    const auto order = record::LoadOrder::build(
        dir.path(), listed({"Verbatim.esm"}),
        record::LoadOrderOptions{.active_only = true, .add_implicit_masters = false});
    REQUIRE(order.problems().empty());
    const auto world = record::MergedWorld::build(order);
    const auto stats = pack::write_snapshot(world, order, path);
    REQUIRE(stats.has_value());
    CHECK(stats->forms == 1);

    const auto snapshot = pack::Snapshot::open(path);
    REQUIRE(snapshot.has_value());

    const auto form = snapshot->find(record::FormId{0x0000'0030});
    REQUIRE(form.has_value());

    // The whole block, byte for byte, in file order.
    const auto expected = undecoded_payload();
    REQUIRE(form->payload.size() == expected.span().size());
    CHECK(std::equal(form->payload.begin(), form->payload.end(), expected.span().begin()));

    // The undecoded tags can be found; checked separately so a failure shows
    // whether contents or length broke.
    const std::string_view bytes(reinterpret_cast<const char*>(form->payload.data()),
                                 form->payload.size());
    for (const auto* tag : {"XRGD", "VMAD", "XAPR", "ZZZZ"}) {
        INFO("field " << tag);
        CHECK(bytes.find(tag) != std::string_view::npos);
    }
}
