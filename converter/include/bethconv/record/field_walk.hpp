// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bethconv/record/headers.hpp"

#include <concepts>

namespace bethconv::record {

/// Walk the fields of a record payload.
///
/// A well-formed record's fields tile its payload exactly, which is a cheap
/// structural check even for undefined types.
///
/// `fn(const FieldHeader&, io::SpanReader& body)` gets a reader confined to the
/// field.
template <typename F>
    requires std::invocable<F, const FieldHeader&, io::SpanReader&>
[[nodiscard]] io::ParseResult<void> for_each_field(io::SpanReader& data, F&& fn) {
    while (!data.at_end()) {
        // Leftover bytes too short for a field header are corruption.
        if (data.remaining() < k_field_header_size) {
            return data.fail(io::ErrorKind::truncated,
                             "trailing " + std::to_string(data.remaining()) +
                                 " bytes are too small for a field header");
        }
        auto field = read_field_header(data);
        if (!field) {
            return std::unexpected(std::move(field).error());
        }
        auto body = data.subreader(field->data_size);
        if (!body) {
            return std::unexpected(std::move(body).error());
        }
        fn(*field, *body);
    }
    return {};
}

} // namespace bethconv::record
