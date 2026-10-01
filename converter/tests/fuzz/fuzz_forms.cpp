// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-type field definitions over a whole plugin.
//
// Separate from fuzz_esm so each gets its own corpus: this one targets fields
// with wrong sizes (STAT DNAM, CELL XCLC, REFR XLKR), which real data rarely
// contains. FormCensus runs every definition and reports overreads as
// `leftover`.
#include "bethconv/record/form_census.hpp"
#include "bethconv/record/plugin.hpp"

#include "fuzz_support.hpp"


extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    static const bethconv::fuzz::ScratchFile scratch(".esp");
    if (!scratch.write({data, size})) {
        return 0;
    }

    auto plugin = bethconv::record::Plugin::open(scratch.path());
    if (!plugin) {
        return 0;
    }

    bethconv::record::FormCensus census;
    // Run both string paths regardless of the header's localized flag; vanilla
    // never exercises FULL-as-text.
    for (const bool localized : {false, true}) {
        census.set_localized(localized);
        (void)plugin->scan(census);
    }
    BETHCONV_FUZZ_CHECK(census.total_parsed() + census.total_failed() >= census.total_parsed());
    return 0;
}
