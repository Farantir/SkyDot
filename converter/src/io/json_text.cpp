// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/io/json_text.hpp"

#include <cstddef>

namespace bethconv::io {
namespace {

constexpr char k_hex[] = "0123456789ABCDEF";

void append_escaped(std::string& out, unsigned char c) {
    out.push_back('%');
    out.push_back(k_hex[c >> 4]);
    out.push_back(k_hex[c & 0x0F]);
}

} // namespace

std::size_t utf8_sequence_length(std::string_view text, std::size_t i) {
    if (i >= text.size()) {
        return 0;
    }
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const auto continuation = [&](std::size_t k, unsigned char lo, unsigned char hi) {
        return k < text.size() && at(k) >= lo && at(k) <= hi;
    };

    const unsigned char b0 = at(i);
    if (b0 < 0x80) {
        return 1;
    }
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        return continuation(i + 1, 0x80, 0xBF) ? 2 : 0;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        const unsigned char lo = b0 == 0xE0 ? 0xA0 : 0x80;
        const unsigned char hi = b0 == 0xED ? 0x9F : 0xBF;
        return continuation(i + 1, lo, hi) && continuation(i + 2, 0x80, 0xBF) ? 3 : 0;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        const unsigned char lo = b0 == 0xF0 ? 0x90 : 0x80;
        const unsigned char hi = b0 == 0xF4 ? 0x8F : 0xBF;
        return continuation(i + 1, lo, hi) && continuation(i + 2, 0x80, 0xBF) &&
                       continuation(i + 3, 0x80, 0xBF)
                   ? 4
                   : 0;
    }
    return 0;
}

std::string json_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20) {
            append_escaped(out, c);
            ++i;
            continue;
        }
        const std::size_t length = utf8_sequence_length(text, i);
        if (length == 0) {
            append_escaped(out, c);
            ++i;
            continue;
        }
        out.append(text.substr(i, length));
        i += length;
    }
    return out;
}

} // namespace bethconv::io
