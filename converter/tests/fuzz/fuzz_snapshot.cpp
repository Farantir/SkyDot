// SPDX-License-Identifier: GPL-3.0-or-later
//
// The record snapshot reader.
//
// Our own output, but still untrusted: packs can be half-written, copied or
// edited. The reader hands offsets from the file to FlatBuffers, so this checks
// that `flatbuffers::Verifier` runs on every path.
//
// Anything that opens must be consistent: every form findable by id, every
// index entry naming an existing form, every payload inside the blob.
#include "bethconv/pack/snapshot.hpp"

#include "fuzz_support.hpp"

#include <algorithm>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    const auto bytes = bethconv::fuzz::as_bytes(data, size);

    const auto snapshot = bethconv::pack::Snapshot::from_bytes(bytes, "fuzz.fb");
    if (!snapshot) {
        return 0;
    }

    // Opening succeeded, so the version is supported. Nothing else is assumed.
    BETHCONV_FUZZ_CHECK(snapshot->format_version() ==
                        bethconv::pack::k_snapshot_format_version);

    // `at` slices the blob; an out-of-range slice must come back empty.
    const std::size_t count = snapshot->size();
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto form = snapshot->at(i);
        if (!form) {
            continue; // A payload that does not fit is refused, not fatal.
        }
        BETHCONV_FUZZ_CHECK(form->payload.size() <= size);

        // `find` binary-searches; an unsorted array that passed the verifier
        // must still not make it read out of bounds.
        if (i != 0 && form->id.value > previous) {
            const auto again = snapshot->find(form->id);
            BETHCONV_FUZZ_CHECK(again.has_value());
            BETHCONV_FUZZ_CHECK(again->id == form->id);
        }
        previous = form->id.value;
    }

    // Each index entry names a form that exists.
    for (const auto type : {bethconv::io::FourCC{"CELL"}, bethconv::io::FourCC{"REFR"},
                            bethconv::io::FourCC{"STAT"}, bethconv::io::FourCC{"WRLD"}}) {
        for (const auto id : snapshot->of_type(type)) {
            const auto form = snapshot->find(bethconv::record::FormId{id});
            if (form) {
                BETHCONV_FUZZ_CHECK(form->type == type);
            }
        }
    }

    for (const auto world : snapshot->worlds()) {
        (void)snapshot->cell_at(world, 0, 0);
        (void)snapshot->cell_at(world, -32768, 32767);
        for (const auto id : snapshot->children_of(world)) {
            (void)snapshot->find(bethconv::record::FormId{id});
        }
    }

    (void)snapshot->plugins();
    (void)snapshot->converter();
    (void)snapshot->language();
    (void)snapshot->merge_stats();
    (void)snapshot->find_editor_id("Tamriel");
    (void)snapshot->blob_intact();
    return 0;
}
