// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets/zip_reader.hpp"

#include <array>

namespace skydot::assets {
namespace {

std::uint32_t le16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8);
}
std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return le16(b, at) | (le16(b, at + 2) << 16);
}

std::uint32_t crc32(std::span<const std::uint8_t> data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFU;
    for (const std::uint8_t b : data) {
        c = table[(c ^ b) & 0xFFU] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

/// Canonical Huffman decoding after zlib's puff.c (counts per length, symbols
/// in code order).
struct Huffman {
    std::array<std::uint16_t, 16> count{};
    std::array<std::uint16_t, 320> symbol{};
};

class Inflater {
public:
    /// `limit`: the most output allowed; more fails the stream.
    Inflater(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t limit)
        : in_(in), out_(out), limit_(limit) {}

    bool run() {
        bool last = false;
        while (!last) {
            last = bits(1) != 0;
            const int type = static_cast<int>(bits(2));
            bool ok = false;
            if (type == 0) {
                ok = stored();
            } else if (type == 1) {
                ok = fixed();
            } else if (type == 2) {
                ok = dynamic();
            }
            if (!ok || error_) {
                return false;
            }
        }
        return true;
    }

private:
    std::span<const std::uint8_t> in_;
    std::vector<std::uint8_t>& out_;
    std::size_t limit_;
    std::size_t pos_ = 0;
    std::uint32_t bitbuf_ = 0;
    int bitcnt_ = 0;
    bool error_ = false;

    std::uint32_t bits(int need) {
        std::uint32_t val = bitbuf_;
        while (bitcnt_ < need) {
            if (pos_ >= in_.size()) {
                error_ = true;
                return 0;
            }
            val |= static_cast<std::uint32_t>(in_[pos_++]) << bitcnt_;
            bitcnt_ += 8;
        }
        bitbuf_ = val >> need;
        bitcnt_ -= need;
        return val & ((1U << need) - 1U);
    }

    bool stored() {
        bitbuf_ = 0;
        bitcnt_ = 0;
        if (pos_ + 4 > in_.size()) {
            return false;
        }
        const std::uint32_t len = le16(in_, pos_);
        const std::uint32_t nlen = le16(in_, pos_ + 2);
        pos_ += 4;
        if (len != (~nlen & 0xFFFFU) || pos_ + len > in_.size() || out_.size() + len > limit_) {
            return false;
        }
        out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(pos_),
                    in_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
        pos_ += len;
        return true;
    }

    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(bits(1));
            if (error_) {
                return -1;
            }
            const int count = h.count[static_cast<std::size_t>(len)];
            if (code - count < first) {
                return h.symbol[static_cast<std::size_t>(index + (code - first))];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    static bool build(Huffman& h, const std::uint16_t* lengths, int n) {
        h.count.fill(0);
        for (int s = 0; s < n; ++s) {
            ++h.count[lengths[s]];
        }
        if (h.count[0] == n) {
            return true; // no codes: only an error if one is used
        }
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h.count[static_cast<std::size_t>(len)];
            if (left < 0) {
                return false;
            }
        }
        std::array<std::uint16_t, 16> offs{};
        for (int len = 1; len < 15; ++len) {
            offs[static_cast<std::size_t>(len + 1)] = static_cast<std::uint16_t>(
                offs[static_cast<std::size_t>(len)] + h.count[static_cast<std::size_t>(len)]);
        }
        for (int s = 0; s < n; ++s) {
            if (lengths[s] != 0) {
                h.symbol[offs[lengths[s]]++] = static_cast<std::uint16_t>(s);
            }
        }
        return true;
    }

    bool codes(const Huffman& lencode, const Huffman& distcode) {
        static constexpr std::array<std::uint16_t, 29> k_lbase{
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::array<std::uint8_t, 29> k_lext{
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::array<std::uint16_t, 30> k_dbase{
            1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::array<std::uint8_t, 30> k_dext{
            0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        while (true) {
            int symbol = decode(lencode);
            if (symbol < 0) {
                return false;
            }
            if (symbol < 256) {
                if (out_.size() >= limit_) {
                    return false;
                }
                out_.push_back(static_cast<std::uint8_t>(symbol));
                continue;
            }
            if (symbol == 256) {
                return true;
            }
            symbol -= 257;
            if (symbol >= 29) {
                return false;
            }
            const auto si = static_cast<std::size_t>(symbol);
            const std::size_t len = k_lbase[si] + bits(k_lext[si]);
            const int ds = decode(distcode);
            if (ds < 0 || ds >= 30) {
                return false;
            }
            const auto di = static_cast<std::size_t>(ds);
            const std::size_t dist = k_dbase[di] + bits(k_dext[di]);
            if (error_ || dist > out_.size() || out_.size() + len > limit_) {
                return false;
            }
            const std::size_t from = out_.size() - dist;
            for (std::size_t i = 0; i < len; ++i) {
                out_.push_back(out_[from + i]);
            }
        }
    }

    bool fixed() {
        static const auto tables = [] {
            std::pair<Huffman, Huffman> t;
            std::array<std::uint16_t, 288> lengths{};
            for (std::size_t s = 0; s < 288; ++s) {
                lengths[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
            }
            build(t.first, lengths.data(), 288);
            lengths.fill(5);
            build(t.second, lengths.data(), 30);
            return t;
        }();
        return codes(tables.first, tables.second);
    }

    bool dynamic() {
        static constexpr std::array<std::uint8_t, 19> k_order{16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                              11, 4,  12, 3, 13, 2, 14, 1, 15};
        const int nlen = static_cast<int>(bits(5)) + 257;
        const int ndist = static_cast<int>(bits(5)) + 1;
        const int ncode = static_cast<int>(bits(4)) + 4;
        if (error_ || nlen > 286 || ndist > 30) {
            return false;
        }
        std::array<std::uint16_t, 320> lengths{};
        for (int i = 0; i < ncode; ++i) {
            lengths[k_order[static_cast<std::size_t>(i)]] = static_cast<std::uint16_t>(bits(3));
        }
        Huffman lencode, distcode;
        if (!build(lencode, lengths.data(), 19)) {
            return false;
        }
        int index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(lencode);
            if (symbol < 0) {
                return false;
            }
            if (symbol < 16) {
                lengths[static_cast<std::size_t>(index++)] = static_cast<std::uint16_t>(symbol);
                continue;
            }
            std::uint16_t len = 0;
            int repeat = 0;
            if (symbol == 16) {
                if (index == 0) {
                    return false;
                }
                len = lengths[static_cast<std::size_t>(index - 1)];
                repeat = 3 + static_cast<int>(bits(2));
            } else if (symbol == 17) {
                repeat = 3 + static_cast<int>(bits(3));
            } else {
                repeat = 11 + static_cast<int>(bits(7));
            }
            if (index + repeat > nlen + ndist) {
                return false;
            }
            while (repeat-- > 0) {
                lengths[static_cast<std::size_t>(index++)] = len;
            }
        }
        if (lengths[256] == 0) {
            return false;
        }
        if (!build(lencode, lengths.data(), nlen) ||
            !build(distcode, lengths.data() + nlen, ndist)) {
            return false;
        }
        return codes(lencode, distcode);
    }
};

} // namespace

std::optional<std::vector<std::uint8_t>> inflate_raw(std::span<const std::uint8_t> in,
                                                     std::size_t size) {
    // Deflate expands at most 1032:1 (a 258 byte match in two 1-bit codes); a larger size is a broken entry.
    constexpr std::size_t k_max_ratio = 1032;
    constexpr std::size_t k_slack = 64;
    if (size > in.size() * k_max_ratio + k_slack) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(size);
    Inflater inflater(in, out, size);
    if (!inflater.run() || out.size() != size) {
        return std::nullopt;
    }
    return out;
}

std::vector<ZipEntry> zip_entries(std::span<const std::uint8_t> zip) {
    std::vector<ZipEntry> out;
    if (zip.size() < 22) {
        return out;
    }
    // End of central directory: the last signature within the comment's reach.
    std::size_t eocd = zip.size() - 22;
    const std::size_t floor = zip.size() > 22 + 0xFFFF ? zip.size() - 22 - 0xFFFF : 0;
    while (le32(zip, eocd) != 0x06054b50U) {
        if (eocd == floor) {
            return out;
        }
        --eocd;
    }
    const std::uint32_t count = le16(zip, eocd + 10);
    std::size_t at = le32(zip, eocd + 16);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 46 > zip.size() || le32(zip, at) != 0x02014b50U) {
            return {};
        }
        ZipEntry e;
        e.method = static_cast<std::uint16_t>(le16(zip, at + 10));
        e.crc = le32(zip, at + 16);
        e.compressed_size = le32(zip, at + 20);
        e.size = le32(zip, at + 24);
        const std::size_t name_len = le16(zip, at + 28);
        const std::size_t extra = le16(zip, at + 30);
        const std::size_t comment = le16(zip, at + 32);
        e.local_offset = le32(zip, at + 42);
        if (at + 46 + name_len > zip.size()) {
            return {};
        }
        e.name.assign(reinterpret_cast<const char*>(zip.data() + at + 46), name_len);
        out.push_back(std::move(e));
        at += 46 + name_len + extra + comment;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> zip_read(std::span<const std::uint8_t> zip,
                                                  std::string_view name) {
    for (const ZipEntry& e : zip_entries(zip)) {
        if (e.name != name) {
            continue;
        }
        const std::size_t at = e.local_offset;
        if (at + 30 > zip.size() || le32(zip, at) != 0x04034b50U) {
            return std::nullopt;
        }
        const std::size_t data = at + 30 + le16(zip, at + 26) + le16(zip, at + 28);
        if (data + e.compressed_size > zip.size()) {
            return std::nullopt;
        }
        const auto raw = zip.subspan(data, e.compressed_size);
        std::optional<std::vector<std::uint8_t>> bytes;
        if (e.method == 0) {
            bytes = std::vector<std::uint8_t>(raw.begin(), raw.end());
        } else if (e.method == 8) {
            bytes = inflate_raw(raw, e.size);
        }
        if (!bytes || bytes->size() != e.size || crc32(*bytes) != e.crc) {
            return std::nullopt;
        }
        return bytes;
    }
    return std::nullopt;
}

} // namespace skydot::assets
