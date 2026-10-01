// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wraps zlib's and LZ4's pointer-and-length APIs so callers only see spans.
#include "bethconv/io/deflate.hpp"

#include <lz4frame.h>
#include <zlib.h>

#include <algorithm>
#include <memory>

namespace bethconv::io {

ParseResult<std::vector<std::byte>> inflate_exact(std::span<const std::byte> compressed,
                                                  std::size_t expected_size,
                                                  std::string_view origin,
                                                  std::size_t origin_offset) {
    const auto make_error = [&](ErrorKind kind, std::string detail) {
        return std::unexpected(ParseError{.origin = std::string(origin),
                                          .offset = origin_offset,
                                          .kind = kind,
                                          .detail = std::move(detail)});
    };

    if (expected_size > k_max_inflate_size) {
        return make_error(ErrorKind::too_large,
                          "claims " + std::to_string(expected_size) +
                              " bytes uncompressed (limit " +
                              std::to_string(k_max_inflate_size) + ")");
    }
    if (expected_size == 0) {
        return std::vector<std::byte>{};
    }
    if (compressed.empty()) {
        return make_error(ErrorKind::truncated, "empty compressed payload");
    }

    std::vector<std::byte> out(expected_size);

    z_stream stream{};
    if (::inflateInit(&stream) != Z_OK) {
        return make_error(ErrorKind::corrupt, "inflateInit failed");
    }
    stream.next_in = const_cast<Bytef*>(static_cast<const Bytef*>(
        static_cast<const void*>(compressed.data())));
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = static_cast<Bytef*>(static_cast<void*>(out.data()));
    stream.avail_out = static_cast<uInt>(out.size());

    const int rc = ::inflate(&stream, Z_FINISH);
    const auto produced = static_cast<std::size_t>(stream.total_out);
    ::inflateEnd(&stream);

    if (rc != Z_STREAM_END) {
        return make_error(ErrorKind::corrupt,
                          "inflate returned " + std::to_string(rc) + " after " +
                              std::to_string(produced) + " of " +
                              std::to_string(expected_size) + " bytes");
    }
    if (produced != expected_size) {
        // Clean end of stream, but not the declared size: reject.
        return make_error(ErrorKind::corrupt,
                          "inflated to " + std::to_string(produced) +
                              " bytes, header promised " +
                              std::to_string(expected_size));
    }
    return out;
}

ParseResult<std::vector<std::byte>> lz4_decompress_exact(std::span<const std::byte> compressed,
                                                         std::size_t expected_size,
                                                         std::string_view origin,
                                                         std::size_t origin_offset) {
    const auto make_error = [&](ErrorKind kind, std::string detail) {
        return std::unexpected(ParseError{.origin = std::string(origin),
                                          .offset = origin_offset,
                                          .kind = kind,
                                          .detail = std::move(detail)});
    };

    if (expected_size > k_max_inflate_size) {
        return make_error(ErrorKind::too_large,
                          "claims " + std::to_string(expected_size) +
                              " bytes uncompressed (limit " +
                              std::to_string(k_max_inflate_size) + ")");
    }
    if (expected_size == 0) {
        return std::vector<std::byte>{};
    }
    if (compressed.empty()) {
        return make_error(ErrorKind::truncated, "empty compressed payload");
    }

    ::LZ4F_dctx* raw_ctx = nullptr;
    if (::LZ4F_isError(::LZ4F_createDecompressionContext(&raw_ctx, LZ4F_VERSION))) {
        return make_error(ErrorKind::corrupt, "LZ4F_createDecompressionContext failed");
    }
    const std::unique_ptr<::LZ4F_dctx, decltype(&::LZ4F_freeDecompressionContext)> ctx{
        raw_ctx, ::LZ4F_freeDecompressionContext};

    // One byte of slack so output larger than declared is detected instead of
    // truncated. (zlib gets this by requiring Z_STREAM_END.)
    std::vector<std::byte> out(expected_size + 1);
    std::size_t in_used = 0;
    std::size_t out_used = 0;
    std::size_t hint = 1;

    // Two independent stop conditions; either alone prevents the hang.
    while (hint != 0) {
        std::size_t in_left = compressed.size() - in_used;
        std::size_t out_left = out.size() - out_used;
        // (1) Input or output exhausted. rsm-bsa lacks this and calls
        // LZ4F_decompress(0, 0) forever.
        if (in_left == 0 || out_left == 0) {
            break;
        }
        hint = ::LZ4F_decompress(ctx.get(), static_cast<void*>(out.data() + out_used), &out_left,
                                 static_cast<const void*>(compressed.data() + in_used), &in_left,
                                 nullptr);
        if (::LZ4F_isError(hint)) {
            return make_error(ErrorKind::corrupt,
                              std::string("LZ4F_decompress: ") + ::LZ4F_getErrorName(hint));
        }
        // (2) The call consumed and produced nothing. After the call,
        // in_left/out_left hold the amounts used.
        if (in_left == 0 && out_left == 0) {
            return make_error(ErrorKind::truncated,
                              "LZ4 frame stalled after " + std::to_string(out_used) + " of " +
                                  std::to_string(expected_size) + " bytes with " +
                                  std::to_string(compressed.size() - in_used) +
                                  " input bytes left");
        }
        in_used += in_left;
        out_used += out_left;
    }

    if (out_used != expected_size) {
        // Output size differs from the declared size (`>` means the slack byte
        // was used): reject.
        return make_error(ErrorKind::corrupt,
                          "decompressed to " + (out_used > expected_size
                                                    ? std::string("more than ")
                                                    : std::string("")) +
                              std::to_string(std::min(out_used, expected_size)) +
                              " bytes, the archive promised " + std::to_string(expected_size));
    }
    out.resize(expected_size);
    return out;
}

} // namespace bethconv::io
