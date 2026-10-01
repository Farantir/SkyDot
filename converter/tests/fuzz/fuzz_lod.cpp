// SPDX-License-Identifier: GPL-3.0-or-later
//
// LOD data files (`.lod`, `.lst`, `.btt`). Every input is tried as all three;
// whatever decodes is written as a LOD asset and must read back the same.
#include "bethconv/pack/lod_asset.hpp"

#include "fuzz_support.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    const auto bytes = bethconv::fuzz::as_bytes(data, size);
    for (const char* extension : {".lod", ".lst", ".btt"}) {
        auto decoded = bethconv::pack::read_lod_source(bytes, extension, "fuzz");
        if (!decoded) {
            continue;
        }
        // Each entry took its full size from the input.
        BETHCONV_FUZZ_CHECK(decoded->tree_types.size() * 32 <= size);
        BETHCONV_FUZZ_CHECK(decoded->trees.size() * 32 <= size);
        BETHCONV_FUZZ_CHECK(decoded->trailing_bytes <= size);
        const auto asset = bethconv::pack::write_lod_asset(*decoded);
        const auto back = bethconv::pack::read_lod_asset(asset, "fuzz.lodfb");
        BETHCONV_FUZZ_CHECK(back.has_value());
        BETHCONV_FUZZ_CHECK(back->trees.size() == decoded->trees.size());
        BETHCONV_FUZZ_CHECK(back->tree_types.size() == decoded->tree_types.size());
        BETHCONV_FUZZ_CHECK(back->settings.has_value() == decoded->settings.has_value());
    }
    return 0;
}
