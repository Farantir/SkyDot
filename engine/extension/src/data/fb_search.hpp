// SPDX-License-Identifier: GPL-3.0-or-later
//
// Searching a FlatBuffers vector that is sorted by a field. When the field is
// unique the schema marks it `(key)` and flatc generates `LookupByKey`;
// `lookup` calls it. A field that can repeat has no key, and
// `first_at_least` finds where the run of one value starts. By index: MSVC
// warns (an error here) inside std::lower_bound over FlatBuffers' iterators,
// whose 32-bit size type narrows its 64-bit difference type.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <cstdint>

namespace skydot {

/// The element whose `(key)` field is `key`, or null; also when the pack has
/// no such vector (`list` is null).
template <typename Vector, typename Key>
auto lookup(const Vector* list, Key key) -> decltype(list->LookupByKey(key)) {
    return list != nullptr ? list->LookupByKey(key) : nullptr;
}

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

} // namespace skydot
