// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiled Papyrus (`.pex`).
//
// `convert` passes every script in the mount to `parse_pex`. PEX is the only
// big-endian format here, so a wrong byte swap turns small lengths into large
// ones (0x0100 is 256, not 1). The string table loop is driven by
// `string_count`, so counts and lengths that disagree with the file size are
// the interesting inputs. Files that decode completely are written as script
// assets and read back.
#include "bethconv/pack/script_asset.hpp"
#include "bethconv/script/pex.hpp"

#include "fuzz_support.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    const auto bytes = bethconv::fuzz::as_bytes(data, size);

    if (auto script = bethconv::script::read_pex_script(bytes, "fuzz.pex")) {
        // Every index the reader accepted is inside the table.
        const auto text = bethconv::script::disassemble(*script);
        const auto asset = bethconv::pack::write_script_asset(*script);
        const auto back = bethconv::pack::read_script_asset(asset, "fuzz.pexfb");
        BETHCONV_FUZZ_CHECK(back.has_value());
        BETHCONV_FUZZ_CHECK(bethconv::script::disassemble(*back) == text);
    }

    auto info = bethconv::script::parse_pex(bytes, "fuzz.pex");
    if (!info) {
        return 0;
    }

    // The parse stopped inside the file; otherwise `unparsed_bytes()` would
    // underflow.
    BETHCONV_FUZZ_CHECK(info->parsed_bytes <= info->file_bytes);
    BETHCONV_FUZZ_CHECK(info->file_bytes == size);
    BETHCONV_FUZZ_CHECK(info->unparsed_bytes() <= size);

    // Every string view points inside the input.
    for (const auto text : {info->source_file, info->username, info->machine}) {
        if (!text.empty()) {
            const auto* begin = reinterpret_cast<const std::byte*>(text.data());
            BETHCONV_FUZZ_CHECK(begin >= bytes.data());
            BETHCONV_FUZZ_CHECK(begin + text.size() <= bytes.data() + bytes.size());
        }
    }

    // Each entry needs at least its 2-byte length, after a header of at least
    // 16 bytes.
    BETHCONV_FUZZ_CHECK(static_cast<std::size_t>(info->string_count) * 2 <= size);

    // Only Skyrim scripts (gameID 1) are convertible.
    BETHCONV_FUZZ_CHECK(info->convertible() ==
                        (info->game == bethconv::script::PexGame::skyrim));
    BETHCONV_FUZZ_CHECK(!info->convertible() || info->game_id == 1);

    return 0;
}
