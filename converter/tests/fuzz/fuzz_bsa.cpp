// SPDX-License-Identifier: GPL-3.0-or-later
//
// BSA: mount an archive and read every entry.
//
// rsm-bsa parses these bytes itself; this checks that our wrapper keeps its
// failures contained. The unit-test fixtures come from rsm-bsa's own writer and
// are always valid, so malformed archives only come from here.
#include "bethconv/archive/archive_set.hpp"

#include "fuzz_support.hpp"


extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    // rsm-bsa opens paths, not spans; see ScratchFile.
    static const bethconv::fuzz::ScratchFile scratch(".bsa");
    if (!scratch.write({data, size})) {
        return 0;
    }

    bethconv::archive::ArchiveSet set;
    const auto mounted = set.mount_archive(scratch.path(), 0);
    if (!mounted) {
        return 0;
    }

    // Reading every entry exercises decompression, where lying block sizes
    // land.
    std::size_t seen = 0;
    set.for_each([&](const bethconv::archive::Resolution& resolution) {
        ++seen;
        if (seen > 4096) {
            return; // a hostile header can claim a great many entries
        }
        BETHCONV_FUZZ_CHECK(resolution.winner < set.sources().size());
        (void)set.read(resolution.vpath);
    });
    // The index and the walk must agree, with the same cap.
    BETHCONV_FUZZ_CHECK(seen <= 4096 || set.unique_paths() >= 4096);
    return 0;
}
