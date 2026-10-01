// SPDX-License-Identifier: GPL-3.0-or-later
//
// Binary search over a sorted FlatBuffers vector by index. MSVC warns (an
// error here) inside std::lower_bound over FlatBuffers' iterators, whose
// 32-bit size type narrows its 64-bit difference type.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <cstdint>

namespace skydot {

/// Index of the first element whose `key_of` is not less than `key`;
/// `list->size()` if none. `list` must not be null.
template <typename Vector, typename KeyOf>
flatbuffers::uoffset_t first_at_least(const Vector* list, std::uint32_t key, KeyOf key_of) {
    flatbuffers::uoffset_t lo = 0;
    flatbuffers::uoffset_t hi = list->size();
    while (lo < hi) {
        const flatbuffers::uoffset_t mid = lo + (hi - lo) / 2;
        if (key_of(list->Get(mid)) < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/// The element with exactly this key, or null.
template <typename Vector, typename KeyOf>
auto find_sorted(const Vector* list, std::uint32_t key, KeyOf key_of)
    -> decltype(list->Get(0)) {
    if (list == nullptr) {
        return nullptr;
    }
    const auto i = first_at_least(list, key, key_of);
    return i < list->size() && key_of(list->Get(i)) == key ? list->Get(i) : nullptr;
}

} // namespace skydot
