// SPDX-License-Identifier: GPL-3.0-or-later
//
// Little-endian byte writing and bounds-checked reading, for everything that
// goes into a save. Nothing here touches Godot.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::save {

class ByteWriter {
public:
    void u8(std::uint8_t v) { out.push_back(v); }
    void u32(std::uint32_t v) {
        for (int s = 0; s < 32; s += 8) {
            out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFFu));
        }
    }
    void u64(std::uint64_t v) {
        for (int s = 0; s < 64; s += 8) {
            out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFFu));
        }
    }
    void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
    void text(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    void bytes(const std::vector<std::uint8_t>& b) { out.insert(out.end(), b.begin(), b.end()); }
    std::vector<std::uint8_t> out;
};

/// Reads from bytes it does not own; every read says whether it fit.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<std::uint8_t>& bytes) : data_(bytes.data()), size_(bytes.size()) {}

    bool u8(std::uint8_t& v) {
        if (!need(1)) {
            return false;
        }
        v = data_[pos_++];
        return true;
    }
    bool u32(std::uint32_t& v) {
        if (!need(4)) {
            return false;
        }
        v = 0;
        for (int s = 0; s < 32; s += 8) {
            v |= static_cast<std::uint32_t>(data_[pos_++]) << s;
        }
        return true;
    }
    bool u64(std::uint64_t& v) {
        if (!need(8)) {
            return false;
        }
        v = 0;
        for (int s = 0; s < 64; s += 8) {
            v |= static_cast<std::uint64_t>(data_[pos_++]) << s;
        }
        return true;
    }
    bool f64(double& v) {
        std::uint64_t bits = 0;
        if (!u64(bits)) {
            return false;
        }
        v = std::bit_cast<double>(bits);
        return true;
    }
    bool text(std::string& s) {
        std::uint32_t n = 0;
        if (!u32(n) || !need(n)) {
            return false;
        }
        s.assign(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return true;
    }
    /// A count of items each at least `min_bytes` long; refuses counts the
    /// remaining bytes cannot hold.
    bool count(std::uint32_t& n, std::size_t min_bytes) {
        return u32(n) && static_cast<std::size_t>(n) * min_bytes <= size_ - pos_;
    }
    [[nodiscard]] bool at_end() const { return pos_ == size_; }

private:
    [[nodiscard]] bool need(std::size_t n) const { return size_ - pos_ >= n; }
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

} // namespace skydot::save
