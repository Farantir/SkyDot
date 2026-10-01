// SPDX-License-Identifier: GPL-3.0-or-later
//
// DDS: header arithmetic and the mip-tail fix.
//
// If a header parses, its numbers must fit the file, and the fixed output must
// parse again with the same shape. Mip arithmetic bugs do not crash; they
// produce files Godot rejects.
#include "bethconv/texture/dds.hpp"
#include "bethconv/texture/mip_tail.hpp"

#include "fuzz_support.hpp"


extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    const auto bytes = bethconv::fuzz::as_bytes(data, size);

    const auto info = bethconv::texture::parse_dds(bytes, "fuzz.dds");
    if (!info) {
        return 0;
    }

    // A small file can only satisfy these if the header arithmetic is
    // consistent.
    BETHCONV_FUZZ_CHECK(info->faces >= 1);
    BETHCONV_FUZZ_CHECK(info->stored_levels <= info->full_chain_levels);
    BETHCONV_FUZZ_CHECK(info->declared_bytes <= size);

    const auto fixed = bethconv::texture::complete_mip_tail(bytes, *info, "fuzz.dds");
    if (!fixed) {
        return 0;
    }

    if (fixed->outcome != bethconv::texture::TailOutcome::completed) {
        return 0;
    }

    // The output must parse and its chain must reach 1x1.
    //
    // The output can be smaller than the input: bytes past the declared surface
    // are dropped (the CLI reports them), so compare declared content, not file
    // length.
    BETHCONV_FUZZ_CHECK(fixed->data.size() >= info->declared_bytes);
    const auto again = bethconv::texture::parse_dds(fixed->data, "fuzz-fixed.dds");
    BETHCONV_FUZZ_CHECK(again.has_value());
    BETHCONV_FUZZ_CHECK(again->width == info->width);
    BETHCONV_FUZZ_CHECK(again->height == info->height);
    BETHCONV_FUZZ_CHECK(again->faces == info->faces);
    BETHCONV_FUZZ_CHECK(again->stored_levels == again->full_chain_levels);
    return 0;
}
