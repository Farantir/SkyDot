// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes a starting corpus for the fuzz targets.
//
// Seeds come from the synthetic builders in tests/support, never game data.
// About half are malformed so the fuzzer starts near the interesting inputs.
#include "../support/bsa_builder.hpp"
#include "../support/dds_builder.hpp"
#include "../support/esm_builder.hpp"
#include "../support/lod_builder.hpp"
#include "../support/nif_builder.hpp"
#include "../support/pex_builder.hpp"
#include "../support/strings_builder.hpp"

#include "bethconv/pack/asset_store.hpp"
#include "bethconv/pack/snapshot.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using bethconv::test::ByteWriter;
using bethconv::testing::StringEntry;
using bethconv::testing::StringTableSpec;

namespace {

std::size_t written = 0;

void emit(const std::filesystem::path& dir, const std::string& name,
          std::span<const std::byte> bytes) {
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ++written;
}

/// `bytes` truncated to `keep`. Truncation is the most productive mutation for
/// every format here.
std::vector<std::byte> truncated(std::span<const std::byte> bytes, std::size_t keep) {
    return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::min(keep, bytes.size()))};
}

// ---- DDS ------------------------------------------------------------------

void seed_dds(const std::filesystem::path& root) {
    const auto dir = root / "dds";
    using bethconv::testing::DdsSpec;

    const auto full = bethconv::testing::build_dds(
        DdsSpec{.width = 64, .height = 64, .mips = 7, .fourcc = bethconv::io::FourCC{"DXT1"}});
    emit(dir, "dxt1-complete.dds", full);

    // Short chain, as in 31,522 of 31,833 vanilla SE textures.
    emit(dir, "dxt5-short-chain.dds",
         bethconv::testing::build_dds(DdsSpec{.width = 64,
                                              .height = 64,
                                              .mips = 5,
                                              .fourcc = bethconv::io::FourCC{"DXT5"}}));
    emit(dir, "cubemap.dds",
         bethconv::testing::build_dds(DdsSpec{.width = 32,
                                              .height = 32,
                                              .mips = 6,
                                              .fourcc = bethconv::io::FourCC{"DXT1"},
                                              .cubemap = true}));
    emit(dir, "volume.dds", bethconv::testing::build_dds(DdsSpec{.width = 16,
                                                                 .height = 16,
                                                                 .mips = 5,
                                                                 .volume = true,
                                                                 .depth = 16}));
    emit(dir, "bc7-dx10.dds",
         bethconv::testing::build_dds(DdsSpec{.width = 32, .height = 32, .mips = 6, .dx10 = true}));
    emit(dir, "uncompressed-rgba32.dds",
         bethconv::testing::build_dds(
             DdsSpec{.width = 16, .height = 16, .mips = 5, .fourcc = bethconv::io::FourCC{}, .rgb_bit_count = 32}));

    // Malformed: a header lying about its chain, and a truncated file.
    emit(dir, "mips-past-the-surface.dds",
         bethconv::testing::build_dds(
             DdsSpec{.width = 4, .height = 4, .mips = 12, .stored_levels = 1}));
    emit(dir, "truncated-header.dds", truncated(full, 60));
    emit(dir, "truncated-data.dds", truncated(full, 160));
}

// ---- .STRINGS -------------------------------------------------------------

void seed_strings(const std::filesystem::path& root) {
    const auto dir = root / "strings";
    using bethconv::record::StringKind;

    const auto plain = bethconv::testing::build_string_table(
        {.kind = StringKind::plain,
         .entries = {{.id = 1, .text = "The Ratway Vaults"},
                     {.id = 2, .text = "Windhelm Stable Services"},
                     {.id = 3, .text = "", .alias_of = 0}}});
    emit(dir, "plain.strings", plain);

    emit(dir, "prefixed.dlstrings",
         bethconv::testing::build_string_table(
             {.kind = StringKind::description,
              .entries = {{.id = 0x44, .text = "A long description."},
                          {.id = 0x5C, .text = "caf\xC3\xA9 they\xE2\x80\x99re"}}}));

    // Malformed: one per parser rule.
    emit(dir, "count-too-large.strings",
         bethconv::testing::build_string_table({.kind = StringKind::plain,
                                                .entries = {{.id = 1, .text = "a"}},
                                                .override_count = true,
                                                .count = 100000}));
    emit(dir, "offset-past-the-end.strings",
         bethconv::testing::build_string_table(
             {.kind = StringKind::plain,
              .entries = {{.id = 1,
                           .text = "a",
                           .override_offset = true,
                           .offset = 0xFFFF0000u}}}));
    emit(dir, "length-past-the-end.dlstrings",
         bethconv::testing::build_string_table({.kind = StringKind::description,
                                                .entries = {{.id = 1,
                                                             .text = "short",
                                                             .override_length = true,
                                                             .length = 100000}}}));
    emit(dir, "zero-length.dlstrings",
         bethconv::testing::build_string_table(
             {.kind = StringKind::description,
              .entries = {{.id = 1, .text = "", .override_length = true, .length = 0}}}));
    emit(dir, "unterminated.strings",
         bethconv::testing::build_string_table(
             {.kind = StringKind::plain,
              .entries = {{.id = 1, .text = "no terminator", .omit_terminator = true}}}));
    emit(dir, "bad-utf8.strings",
         bethconv::testing::build_string_table(
             {.kind = StringKind::plain,
              .entries = {{.id = 1, .text = "\xC0\xAF overlong \xED\xA0\x80 surrogate \xFF"}}}));
    emit(dir, "truncated.strings", truncated(plain, 12));
}

// ---- .PEX -----------------------------------------------------------------

/// Big-endian, so several seeds target the byte swap: counts and lengths that
/// are consistent one way round and not the other.
void seed_pex(const std::filesystem::path& root) {
    const auto dir = root / "pex";
    using bethconv::testing::PexSpec;

    const auto plain = bethconv::testing::build_pex(PexSpec{});
    emit(dir, "skyrim.pex", plain);

    // Fallout 4 script: valid PEX, little-endian, not converted. The magic
    // decides the byte order.
    emit(dir, "fallout4.pex",
         bethconv::testing::build_pex(PexSpec{.game_id = 2, .little_endian = true}));

    emit(dir, "unknown-game.pex", bethconv::testing::build_pex(PexSpec{.game_id = 0x4242}));
    emit(dir, "empty-table.pex", bethconv::testing::build_pex(PexSpec{.strings = {}}));

    // String count too large and too small.
    emit(dir, "count-too-large.pex",
         bethconv::testing::build_pex(PexSpec{.declared_string_count = 0xFFFF}));
    emit(dir, "count-too-small.pex",
         bethconv::testing::build_pex(PexSpec{.declared_string_count = 1}));

    // A length far larger than the file.
    emit(dir, "long-name.pex",
         bethconv::testing::build_pex(PexSpec{.source_file = std::string(4096, 'p')}));

    // Truncated inside the header, the header strings and the table.
    emit(dir, "truncated-header.pex", bethconv::testing::truncate_pex(plain, 9));
    emit(dir, "truncated-strings.pex", bethconv::testing::truncate_pex(plain, 30));
    emit(dir, "truncated-table.pex",
         bethconv::testing::truncate_pex(plain, plain.size() - 70));
    emit(dir, "empty.pex", {});

    // A whole script: variables of every type, an auto property, a function
    // with a call, a branch and line numbers.
    using namespace bethconv::testing;
    auto spec = empty_script("Seed", "ObjectReference");
    auto& o = spec.objects[0];
    o.variables = {{.name = "::Target_var", .type = "ObjectReference", .initial = none()},
                   {.name = "N", .type = "Int", .initial = integer(7)},
                   {.name = "F", .type = "Float", .initial = floating(0.5F)},
                   {.name = "S", .type = "String", .initial = str("s")},
                   {.name = "B", .type = "Bool", .initial = boolean(true)}};
    o.properties = {{.name = "Target", .type = "ObjectReference", .auto_var = "::Target_var"}};
    PexFunctionSpec f;
    f.name = "OnActivate";
    f.params = {{"akActionRef", "ObjectReference"}};
    f.locals = {{"::temp0", "Bool"}, {"::NoneVar", "None"}};
    f.code = {{.op = 0x0E, .args = {ident("::temp0"), ident("::Target_var")}, .varargs = {}},
              {.op = 0x16, .args = {ident("::temp0"), integer(2)}, .varargs = {}},
              {.op = 0x17,
               .args = {ident("Disable"), ident("::Target_var"), ident("::NoneVar")},
               .varargs = {boolean(false)}}};
    f.lines = {1, 1, 2};
    o.states = {{.name = "", .functions = {f}}};
    emit(dir, "script.pex", build_script(spec));
}

// ---- ESM ------------------------------------------------------------------

/// A plugin with a TES4 header, a top group and a couple of records.
std::vector<std::byte> build_plugin(std::uint32_t flags, bool lie_about_group_size) {
    ByteWriter children;
    for (const auto form : {0x00000800u, 0x00000801u}) {
        ByteWriter payload;
        ByteWriter edid;
        edid.zstring("seed");
        bethconv::test::write_field(payload, "EDID", edid);
        ByteWriter obnd;
        for (int i = 0; i < 6; ++i) {
            obnd.u16(0x0102);
        }
        bethconv::test::write_field(payload, "OBND", obnd);
        ByteWriter dnam;
        dnam.f32(1.0f);
        dnam.u32(0);
        dnam.u32(0); // the 12-byte DNAM variant
        bethconv::test::write_field(payload, "DNAM", dnam);
        bethconv::test::write_record(children, "STAT", form, payload.span());
    }

    ByteWriter file;
    bethconv::test::write_tes4(file, flags, {});
    const auto label = bethconv::io::FourCC{"STAT"}.value;
    if (lie_about_group_size) {
        // A group claiming more bytes than the file has.
        file.tag("GRUP");
        file.u32(0xFFFF0000u);
        file.u32(label);
        file.u32(0);
        file.u16(0);
        file.u16(0);
        file.u16(44);
        file.u16(0);
        file.raw(children.span());
    } else {
        bethconv::test::write_group(file, label, 0, children.span());
    }
    return file.bytes();
}

void seed_esm(const std::filesystem::path& root) {
    const auto dir = root / "esm";
    constexpr std::uint32_t k_master = 0x00000001;
    constexpr std::uint32_t k_localized = 0x00000080;

    const auto plain = build_plugin(k_master, false);
    emit(dir, "plain.esm", plain);
    emit(dir, "localized.esm", build_plugin(k_master | k_localized, false));
    emit(dir, "group-size-lies.esm", build_plugin(k_master, true));
    emit(dir, "truncated-header.esm", truncated(plain, 20));
    emit(dir, "truncated-mid-record.esm", truncated(plain, plain.size() - 9));

    // Compressed record, so the inflate path is reachable.
    {
        ByteWriter payload;
        ByteWriter edid;
        edid.zstring("compressed-seed");
        bethconv::test::write_field(payload, "EDID", edid);
        const auto deflated = bethconv::test::compress_payload(payload.span());
        ByteWriter children;
        bethconv::test::write_record(children, "CELL", 0x00000900, deflated, 0x00040000);
        ByteWriter file;
        bethconv::test::write_tes4(file, k_master, {});
        bethconv::test::write_group(file, bethconv::io::FourCC{"CELL"}.value, 0,
                                    children.span());
        emit(dir, "compressed-record.esm", file.bytes());
    }

    // An activator with a script and one property of each kind, for VMAD.
    {
        ByteWriter vmad;
        const auto wstring = [&](std::string_view s) {
            vmad.u16(static_cast<std::uint16_t>(s.size()));
            vmad.raw(s);
        };
        vmad.u16(5);
        vmad.u16(2);
        vmad.u16(1);
        wstring("seedScript");
        vmad.u8(0);
        vmad.u16(3);
        wstring("Target");
        vmad.u8(1);
        vmad.u8(1);
        vmad.u16(0);
        vmad.u16(0xFFFF);
        vmad.u32(0x00000800);
        wstring("Names");
        vmad.u8(12);
        vmad.u8(1);
        vmad.u32(2);
        wstring("a");
        wstring("bc");
        wstring("Flags");
        vmad.u8(15);
        vmad.u8(1);
        vmad.u32(1);
        vmad.u8(1);
        ByteWriter payload;
        bethconv::test::write_field(payload, "VMAD", vmad);
        ByteWriter children;
        bethconv::test::write_record(children, "ACTI", 0x00000A00, payload.span());
        ByteWriter file;
        bethconv::test::write_tes4(file, k_master, {});
        bethconv::test::write_group(file, bethconv::io::FourCC{"ACTI"}.value, 0,
                                    children.span());
        emit(dir, "scripted.esm", file.bytes());
    }
}

// ---- record snapshot ------------------------------------------------------

/// A real `records.fb`, built through the actual plugin → merge → write
/// pipeline so the seed has the shape the reader will see.
void seed_snapshot(const std::filesystem::path& root) {
    const auto dir = root / "snapshot";
    const auto scratch = root / ".snapshot-build";
    std::filesystem::create_directories(scratch);

    {
        const auto plugin = build_plugin(0x00000001, false);
        std::ofstream out(scratch / "Seed.esm", std::ios::binary);
        out.write(reinterpret_cast<const char*>(plugin.data()),
                  static_cast<std::streamsize>(plugin.size()));
    }

    const auto order = bethconv::record::LoadOrder::from_directory(scratch);
    if (!order) {
        std::filesystem::remove_all(scratch);
        return;
    }
    const auto world = bethconv::record::MergedWorld::build(*order);
    const auto path = scratch / "records.fb";
    if (!bethconv::pack::write_snapshot(world, *order, path)) {
        std::filesystem::remove_all(scratch);
        return;
    }

    std::vector<std::byte> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        char c = 0;
        while (in.get(c)) {
            bytes.push_back(static_cast<std::byte>(c));
        }
    }
    std::filesystem::remove_all(scratch);
    if (bytes.size() < 64) {
        return;
    }

    emit(dir, "plain.fb", bytes);
    emit(dir, "header-only.fb", truncated(bytes, 64));
    emit(dir, "truncated-blob.fb", truncated(bytes, 96));
    // Cut before the index, so the header bounds check must reject it before
    // the verifier runs.
    emit(dir, "no-index.fb", truncated(bytes, bytes.size() / 2));

    // A header whose section size is wrong. Must be rejected on arithmetic
    // before FlatBuffers sees any byte.
    auto lying = bytes;
    for (std::size_t i = 0; i < 8; ++i) {
        lying[24 + i] = static_cast<std::byte>(0xFF); // fb_bytes
    }
    emit(dir, "index-longer-than-file.fb", lying);
}

// ---- NIF ------------------------------------------------------------------

void seed_nif(const std::filesystem::path& root) {
    const auto dir = root / "nif";
    for (const auto flavor :
         {bethconv::test::NifFlavor::le, bethconv::test::NifFlavor::se}) {
        bethconv::test::NifBuilder builder(flavor);
        const auto cube = bethconv::test::make_cube();
        auto* shape = builder.add_shape("seed", cube);
        builder.add_shader(shape, "textures\\seed.dds", "textures\\seed_n.dds");
        const auto bytes = builder.bytes();
        const std::string name =
            flavor == bethconv::test::NifFlavor::le ? "cube-le.nif" : "cube-se.nif";
        emit(dir, name, bytes);
        if (flavor == bethconv::test::NifFlavor::se && !bytes.empty()) {
            emit(dir, "truncated.nif", truncated(bytes, bytes.size() / 2));
            emit(dir, "header-only.nif", truncated(bytes, 96));
        }
    }
}

// ---- BSA ------------------------------------------------------------------

/// Written with rsm-bsa's writer, so these seeds are valid; malformed archives
/// are left to the fuzzer.
void seed_bsa(const std::filesystem::path& root) {
    const auto dir = root / "bsa";
    std::filesystem::create_directories(dir);
    const std::vector<bethconv::test::BsaEntry> entries{
        {"meshes/seed/one.nif", std::string(256, 'a')},
        {"textures/seed/two.dds", std::string(512, 'b')},
        {"strings/seed_english.strings", std::string(64, 'c')},
    };
    // v104 (zlib) and v105 (LZ4) use different block codecs.
    bethconv::test::write_bsa(dir / "tes5-plain.bsa", entries, bsa::tes4::version::tes5, false);
    bethconv::test::write_bsa(dir / "tes5-compressed.bsa", entries, bsa::tes4::version::tes5,
                              true);
    bethconv::test::write_bsa(dir / "sse-plain.bsa", entries, bsa::tes4::version::sse, false);
    bethconv::test::write_bsa(dir / "sse-compressed.bsa", entries, bsa::tes4::version::sse, true);
    written += 4;

    // One truncated archive.
    std::ifstream in(dir / "sse-compressed.bsa", std::ios::binary);
    const std::vector<char> whole{std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>()};
    std::ofstream out(dir / "sse-truncated.bsa", std::ios::binary);
    out.write(whole.data(), static_cast<std::streamsize>(whole.size() / 2));
    ++written;
}

} // namespace

// ---- LOD ------------------------------------------------------------------

void seed_lod(const std::filesystem::path& root) {
    using namespace bethconv::testing;
    const auto dir = root / "lod";
    emit(dir, "settings.lod", build_lod_settings(-96, -96, 256, 4, 32));
    emit(dir, "bad-levels.lod", build_lod_settings(0, 0, 64, 32, 3));
    const auto list = build_tree_list({{.index = 0}, {.index = 1, .width = 90.0F}});
    emit(dir, "types.lst", list);
    emit(dir, "types-truncated.lst", truncated(list, list.size() - 5));
    const auto trees = build_tree_blocks({{0, {{.x = 1.0F}, {.x = 2.0F}}}, {1, {{.y = 3.0F}}}});
    emit(dir, "trees.btt", trees);
    emit(dir, "trees-truncated.btt", truncated(trees, 50));
    auto trailing = trees;
    trailing.resize(trailing.size() + 24, std::byte{0x47});
    emit(dir, "trees-trailing.btt", trailing);
}

void seed_assets(const std::filesystem::path& root) {
    using namespace bethconv::pack;
    const auto dir = root / "assets";
    AssetIndex index;
    index.generation = 3;
    emit(dir, "empty.idx", index.serialize());
    index.blob_bytes = 64;
    for (std::uint8_t i = 0; i < 3; ++i) {
        BlobEntry entry;
        entry.hash.bytes[0] = std::byte{i};
        entry.offset = 16u * i;
        entry.size = 10;
        entry.kind = static_cast<AssetKind>(i);
        index.entries.push_back(entry);
    }
    const auto three = index.serialize();
    emit(dir, "three.idx", three);
    emit(dir, "three-truncated.idx", truncated(three, 50));
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    seed_dds(root);
    seed_strings(root);
    seed_esm(root);
    seed_snapshot(root);
    seed_nif(root);
    seed_pex(root);
    seed_bsa(root);
    seed_lod(root);
    seed_assets(root);
    // fuzz_forms shares the esm corpus.
    std::printf("%zu seed(s) under %s\n", written, root.string().c_str());
    return 0;
}
