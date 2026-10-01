// SPDX-License-Identifier: GPL-3.0-or-later
//
// The only translation unit in the parser path that touches raw bytes
// (enforced by tools/ci/check-raw-access.sh).
#include "bethconv/io/span_reader.hpp"

#include <algorithm>

namespace bethconv::io {

std::string_view to_string(ErrorKind kind) noexcept {
    switch (kind) {
    case ErrorKind::truncated:    return "truncated";
    case ErrorKind::out_of_range: return "out-of-range";
    case ErrorKind::unterminated: return "unterminated";
    case ErrorKind::bad_magic:    return "bad-magic";
    case ErrorKind::bad_value:    return "bad-value";
    case ErrorKind::unsupported:  return "unsupported";
    case ErrorKind::too_large:    return "too-large";
    case ErrorKind::corrupt:      return "corrupt";
    }
    return "unknown";
}

namespace {

std::string hex(std::size_t v) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out = "0x";
    bool leading = true;
    for (int shift = static_cast<int>(sizeof(v)) * 8 - 4; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((v >> shift) & 0xF);
        if (nibble != 0 || !leading || shift == 0) {
            leading = false;
            out.push_back(digits[nibble]);
        }
    }
    return out;
}

} // namespace

std::string ParseError::to_string() const {
    std::string out = origin;
    out += '+';
    out += hex(offset);
    out += ": ";
    out += io::to_string(kind);
    if (!detail.empty()) {
        out += ": ";
        out += detail;
    }
    return out;
}

std::string FourCC::to_string() const {
    std::string out(4, '\0');
    for (int i = 0; i < 4; ++i) {
        const auto c = static_cast<char>((value >> (i * 8)) & 0xFF);
        // Tags come from untrusted files and end up in logs; mask control bytes.
        out[static_cast<std::size_t>(i)] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    return out;
}

std::unexpected<ParseError> SpanReader::fail(ErrorKind kind, std::string detail) const {
    return std::unexpected(ParseError{
        .origin = std::string(origin_),
        .offset = absolute_position(),
        .kind = kind,
        .detail = std::move(detail),
    });
}

std::string SpanReader::need_detail(std::size_t n) const {
    return "need " + std::to_string(n) + " bytes, have " + std::to_string(remaining());
}

ParseResult<FourCC> SpanReader::tag() noexcept {
    if (remaining() < 4) {
        return fail(ErrorKind::truncated, need_detail(4));
    }
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        raw |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(data_[pos_ + i]))
               << (i * 8);
    }
    pos_ += 4;
    return FourCC{raw};
}

ParseResult<std::span<const std::byte>> SpanReader::bytes(std::size_t n) noexcept {
    if (remaining() < n) {
        return fail(ErrorKind::truncated, need_detail(n));
    }
    auto out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
}

ParseResult<std::string_view> SpanReader::chars(std::size_t n) noexcept {
    auto raw = bytes(n);
    if (!raw) {
        return std::unexpected(std::move(raw).error());
    }
    return std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size());
}

ParseResult<std::string_view> SpanReader::fixed_string(std::size_t n) noexcept {
    auto sv_or = chars(n);
    if (!sv_or) {
        return std::unexpected(std::move(sv_or).error());
    }
    const std::string_view sv = *sv_or;
    // Fixed fields are padded with NULs, sometimes spaces.
    const auto end = sv.find_last_not_of(std::string_view("\0 ", 2));
    return end == std::string_view::npos ? std::string_view{} : sv.substr(0, end + 1);
}

ParseResult<std::string_view> SpanReader::zstring() noexcept {
    const auto tail = data_.subspan(pos_);
    const auto nul = std::ranges::find(tail, std::byte{0});
    if (nul == tail.end()) {
        return fail(ErrorKind::unterminated,
                    "no NUL in remaining " + std::to_string(remaining()) + " bytes");
    }
    const auto len = static_cast<std::size_t>(nul - tail.begin());
    std::string_view sv(reinterpret_cast<const char*>(tail.data()), len);
    pos_ += len + 1; // consume the terminator
    return sv;
}

ParseResult<std::string_view> SpanReader::bzstring() noexcept {
    auto len = get<std::uint8_t>();
    if (!len) {
        return std::unexpected(std::move(len).error());
    }
    if (*len == 0) {
        return std::string_view{};
    }
    // The count includes the trailing NUL.
    auto raw = bytes(*len);
    if (!raw) {
        return std::unexpected(std::move(raw).error());
    }
    std::string_view sv(reinterpret_cast<const char*>(raw->data()), raw->size());
    if (!sv.empty() && sv.back() == '\0') {
        sv.remove_suffix(1);
    }
    return sv;
}

ParseResult<std::string_view> SpanReader::wstring() noexcept {
    auto len = get<std::uint16_t>();
    if (!len) {
        return std::unexpected(std::move(len).error());
    }
    return chars(*len);
}

ParseResult<void> SpanReader::seek(std::size_t offset) noexcept {
    if (offset > data_.size()) {
        return fail(ErrorKind::out_of_range,
                    "seek to " + std::to_string(offset) + " in " +
                        std::to_string(data_.size()) + " bytes");
    }
    pos_ = offset;
    return {};
}

ParseResult<void> SpanReader::skip(std::size_t n) noexcept {
    if (remaining() < n) {
        return fail(ErrorKind::truncated, need_detail(n));
    }
    pos_ += n;
    return {};
}

ParseResult<SpanReader> SpanReader::subreader(std::size_t n) noexcept {
    if (remaining() < n) {
        return fail(ErrorKind::truncated, need_detail(n));
    }
    SpanReader child(data_.subspan(pos_, n), origin_, base_ + pos_);
    pos_ += n;
    return child;
}

ParseResult<SpanReader> SpanReader::subreader_at(std::size_t offset,
                                                 std::size_t n) const noexcept {
    if (offset > data_.size() || data_.size() - offset < n) {
        return fail(ErrorKind::out_of_range,
                    "sub-range [" + std::to_string(offset) + ", +" + std::to_string(n) +
                        ") in " + std::to_string(data_.size()) + " bytes");
    }
    return SpanReader(data_.subspan(offset, n), origin_, base_ + offset);
}

} // namespace bethconv::io
