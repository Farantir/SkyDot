// SPDX-License-Identifier: GPL-3.0-or-later
//
// .STRINGS/.DLSTRINGS/.ILSTRINGS.
//
// All three framings from one input. `.strings` entries are NUL-terminated; the
// other two have a uint32 length including the terminator, so a length of 0
// must not underflow.
#include "bethconv/record/strings.hpp"

#include "fuzz_support.hpp"


extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    const auto bytes = bethconv::fuzz::as_bytes(data, size);

    for (const auto kind : bethconv::record::k_string_kinds) {
        auto table = bethconv::record::StringTable::parse(bytes, "fuzz.strings", kind);
        if (!table) {
            continue;
        }
        // Distinct strings never exceed entries, and every entry resolves.
        BETHCONV_FUZZ_CHECK(table->stats().distinct_strings <= table->stats().entries);
        BETHCONV_FUZZ_CHECK(table->size() <= table->stats().entries);

        // Output is valid UTF-8, since it ends up in JSON.
        for (std::uint32_t id = 0; id < 64; ++id) {
            if (const auto* text = table->find(id)) {
                BETHCONV_FUZZ_CHECK(text->find('\xC0') == std::string::npos);
                BETHCONV_FUZZ_CHECK(text->find('\xC1') == std::string::npos);
            }
        }
    }
    return 0;
}
