// SPDX-License-Identifier: GPL-3.0-or-later
//
// `assets.idx`. Whatever parses names only bytes inside the blob it covers,
// in hash order, and serializes back to the same bytes.
#include "bethconv/pack/asset_store.hpp"

#include "fuzz_support.hpp"

#include <algorithm>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    const auto bytes = bethconv::fuzz::as_bytes(data, size);
    const auto index = bethconv::pack::AssetIndex::parse(bytes, "fuzz.idx");
    if (!index) {
        return 0;
    }
    for (const auto& entry : index->entries) {
        BETHCONV_FUZZ_CHECK(entry.offset <= index->blob_bytes);
        BETHCONV_FUZZ_CHECK(entry.size <= index->blob_bytes - entry.offset);
        BETHCONV_FUZZ_CHECK(index->find(entry.hash) == &entry);
    }
    const auto back = index->serialize();
    BETHCONV_FUZZ_CHECK(std::ranges::equal(back, bytes));
    return 0;
}
