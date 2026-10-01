// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic plugin builder for tests, for valid and deliberately malformed
// files.
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

namespace bethconv::test {

/// Little-endian byte-stream builder.
class ByteWriter {
public:
    void u8(std::uint8_t v) { bytes_.push_back(static_cast<std::byte>(v)); }

    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFF));
        u8(static_cast<std::uint8_t>(v >> 8));
    }

    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v & 0xFFFF));
        u16(static_cast<std::uint16_t>(v >> 16));
    }

    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v & 0xFFFFFFFF));
        u32(static_cast<std::uint32_t>(v >> 32));
    }

    void f32(float v) {
        std::uint32_t bits{};
        std::memcpy(&bits, &v, sizeof(bits));
        u32(bits);
    }

    void tag(std::string_view t) {
        for (std::size_t i = 0; i < 4; ++i) {
            u8(i < t.size() ? static_cast<std::uint8_t>(t[i]) : 0x20);
        }
    }

    void raw(std::string_view s) {
        for (const char c : s) {
            u8(static_cast<std::uint8_t>(c));
        }
    }

    void zstring(std::string_view s) {
        raw(s);
        u8(0);
    }

    void raw(std::span<const std::byte> s) {
        bytes_.insert(bytes_.end(), s.begin(), s.end());
    }

    /// Overwrite a reserved u32 (for sizes patched afterwards).
    void patch_u32(std::size_t offset, std::uint32_t v) {
        for (std::size_t i = 0; i < 4; ++i) {
            bytes_[offset + i] = static_cast<std::byte>((v >> (i * 8)) & 0xFF);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::span<const std::byte> span() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

/// A field (subrecord): 4-byte tag, uint16 size, payload.
inline void write_field(ByteWriter& w, std::string_view type, std::span<const std::byte> data) {
    w.tag(type);
    w.u16(static_cast<std::uint16_t>(data.size()));
    w.raw(data);
}

inline void write_field(ByteWriter& w, std::string_view type, const ByteWriter& data) {
    write_field(w, type, data.span());
}

/// A record: 24-byte header then payload.
inline void write_record(ByteWriter& w, std::string_view type, std::uint32_t form_id,
                         std::span<const std::byte> payload, std::uint32_t flags = 0) {
    w.tag(type);
    w.u32(static_cast<std::uint32_t>(payload.size()));
    w.u32(flags);
    w.u32(form_id);
    w.u32(0);      // revision
    w.u16(44);     // version
    w.u16(0);      // unknown
    w.raw(payload);
}

/// zlib-compress a payload into the compressed-record layout: uint32
/// uncompressed size, then the stream.
inline std::vector<std::byte> compress_payload(std::span<const std::byte> payload) {
    uLongf bound = ::compressBound(static_cast<uLong>(payload.size()));
    std::vector<std::byte> deflated(bound);
    const int rc = ::compress(reinterpret_cast<Bytef*>(deflated.data()), &bound,
                              reinterpret_cast<const Bytef*>(payload.data()),
                              static_cast<uLong>(payload.size()));
    if (rc != Z_OK) {
        return {};
    }
    deflated.resize(bound);

    ByteWriter out;
    out.u32(static_cast<std::uint32_t>(payload.size()));
    out.raw(std::span<const std::byte>(deflated));
    return out.bytes();
}

/// A minimal but valid TES4 header record.
inline void write_tes4(ByteWriter& w, std::uint32_t flags = 0,
                       const std::vector<std::string>& masters = {}) {
    ByteWriter payload;

    ByteWriter hedr;
    hedr.f32(1.70f);
    hedr.u32(0); // record count
    hedr.u32(0x800); // next object id
    write_field(payload, "HEDR", hedr);

    ByteWriter cnam;
    cnam.zstring("bethconv-tests");
    write_field(payload, "CNAM", cnam);

    for (const auto& master : masters) {
        ByteWriter mast;
        mast.zstring(master);
        write_field(payload, "MAST", mast);
        ByteWriter data;
        data.u64(0);
        write_field(payload, "DATA", data);
    }

    write_record(w, "TES4", 0, payload.span(), flags);
}

/// A GRUP around serialized children. Its size includes the 24-byte header,
/// unlike a record's.
inline void write_group(ByteWriter& w, std::uint32_t label, std::int32_t group_type,
                        std::span<const std::byte> children) {
    w.tag("GRUP");
    w.u32(static_cast<std::uint32_t>(children.size() + 24));
    w.u32(label);
    w.u32(static_cast<std::uint32_t>(group_type));
    w.u16(0); // stamp
    w.u16(0);
    w.u16(44); // version
    w.u16(0);
    w.raw(children);
}

} // namespace bethconv::test
