// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fixture is built here: a synthetic sine is encoded with libavcodec's
// wmav2 encoder and the packets are wrapped in an XWMA RIFF by hand (libavformat
// cannot mux xwma). No game data is involved.
#include "bethconv/audio/xwma.hpp"
#include "../../src/pack/asset_conversion.hpp"

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;

void put16(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFU));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFFU));
}
void put32(Bytes& out, std::uint32_t v) {
    put16(out, v & 0xFFFFU);
    put16(out, v >> 16);
}
void put_tag(Bytes& out, const char* tag) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>(tag[i]));
    }
}

/// An XWMA RIFF holding `seconds` of a 440 Hz sine.
Bytes make_xwma(int rate, int channels, double seconds) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_WMAV2);
    REQUIRE(codec != nullptr);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = rate;
    ctx->bit_rate = 64000;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&ctx->ch_layout, channels);
    REQUIRE(avcodec_open2(ctx, codec, nullptr) == 0);

    AVFrame* frame = av_frame_alloc();
    frame->nb_samples = ctx->frame_size;
    frame->format = ctx->sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    REQUIRE(av_frame_get_buffer(frame, 0) == 0);
    AVPacket* packet = av_packet_alloc();

    Bytes data;
    std::vector<std::uint32_t> dpds;
    std::uint32_t decoded_bytes = 0;
    std::size_t made = 0;
    const auto total = static_cast<std::size_t>(seconds * rate);
    const auto drain = [&] {
        while (avcodec_receive_packet(ctx, packet) == 0) {
            const auto* bytes = reinterpret_cast<const std::byte*>(packet->data);
            data.insert(data.end(), bytes, bytes + packet->size);
            decoded_bytes += static_cast<std::uint32_t>(ctx->frame_size * channels * 2);
            dpds.push_back(decoded_bytes);
            av_packet_unref(packet);
        }
    };
    while (made < total) {
        REQUIRE(av_frame_make_writable(frame) == 0);
        for (int c = 0; c < channels; ++c) {
            auto* plane = reinterpret_cast<float*>(frame->extended_data[c]);
            for (int s = 0; s < ctx->frame_size; ++s) {
                plane[s] = 0.5F * static_cast<float>(std::sin(
                                      2.0 * std::numbers::pi * 440.0 *
                                      static_cast<double>(made + static_cast<std::size_t>(s)) /
                                      rate));
            }
        }
        REQUIRE(avcodec_send_frame(ctx, frame) == 0);
        drain();
        made += static_cast<std::size_t>(ctx->frame_size);
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    REQUIRE(ctx->block_align > 0);

    Bytes fmt;
    put16(fmt, 0x0161);
    put16(fmt, static_cast<std::uint32_t>(channels));
    put32(fmt, static_cast<std::uint32_t>(rate));
    put32(fmt, static_cast<std::uint32_t>(ctx->bit_rate / 8));
    put16(fmt, static_cast<std::uint32_t>(ctx->block_align));
    put16(fmt, 16);
    put16(fmt, static_cast<std::uint32_t>(ctx->extradata_size));
    for (int i = 0; i < ctx->extradata_size; ++i) {
        fmt.push_back(static_cast<std::byte>(ctx->extradata[i]));
    }
    if ((fmt.size() & 1U) != 0) {
        fmt.push_back(std::byte{0});
    }
    Bytes dpds_bytes;
    for (const auto v : dpds) {
        put32(dpds_bytes, v);
    }

    Bytes riff;
    put_tag(riff, "RIFF");
    put32(riff, 0);
    put_tag(riff, "XWMA");
    put_tag(riff, "fmt ");
    put32(riff, static_cast<std::uint32_t>(fmt.size()));
    riff.insert(riff.end(), fmt.begin(), fmt.end());
    put_tag(riff, "dpds");
    put32(riff, static_cast<std::uint32_t>(dpds_bytes.size()));
    riff.insert(riff.end(), dpds_bytes.begin(), dpds_bytes.end());
    put_tag(riff, "data");
    put32(riff, static_cast<std::uint32_t>(data.size()));
    riff.insert(riff.end(), data.begin(), data.end());
    const auto size = static_cast<std::uint32_t>(riff.size() - 8);
    for (int i = 0; i < 4; ++i) {
        riff[4 + static_cast<std::size_t>(i)] = static_cast<std::byte>((size >> (8 * i)) & 0xFFU);
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return riff;
}

std::uint32_t le32(const Bytes& b, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<unsigned>(b[at + i])) << (8 * i);
    }
    return v;
}

/// The Ogg stream's sample count (last granule position) and the vorbis
/// identification header's channels and rate.
struct OggInfo {
    bool ok = false;
    unsigned channels = 0;
    std::uint32_t rate = 0;
    std::uint64_t samples = 0;
};

OggInfo inspect_ogg(const Bytes& ogg) {
    OggInfo info;
    if (ogg.size() < 58 || std::memcmp(ogg.data(), "OggS", 4) != 0) {
        return info;
    }
    // First page: 27-byte header + 1 segment-table byte, then the packet.
    const std::size_t body = 27 + std::to_integer<unsigned>(ogg[26]);
    if (std::to_integer<unsigned>(ogg[body]) != 1 ||
        std::memcmp(ogg.data() + body + 1, "vorbis", 6) != 0) {
        return info;
    }
    info.channels = std::to_integer<unsigned>(ogg[body + 11]);
    info.rate = le32(ogg, body + 12);
    // Last page's granule position (the final "OggS" in the file).
    for (std::size_t i = ogg.size() - 27; i > 0; --i) {
        if (std::memcmp(ogg.data() + i, "OggS", 4) == 0) {
            info.samples = le32(ogg, i + 6) | (static_cast<std::uint64_t>(le32(ogg, i + 10)) << 32);
            break;
        }
    }
    info.ok = true;
    return info;
}

} // namespace

TEST_CASE("an XWMA RIFF becomes Ogg Vorbis of the same format and length", "[audio][xwma]") {
    for (const int channels : {1, 2}) {
        const auto xwma = make_xwma(44100, channels, 1.0);
        const auto ogg = bethconv::audio::xwma_to_ogg(xwma, "sound/test.xwm");
        REQUIRE(ogg.has_value());
        const auto info = inspect_ogg(*ogg);
        REQUIRE(info.ok);
        CHECK(info.channels == static_cast<unsigned>(channels));
        CHECK(info.rate == 44100);
        const double seconds = static_cast<double>(info.samples) / 44100.0;
        CHECK(seconds > 1.0 * 0.95);
        CHECK(seconds < 1.0 * 1.05 + 2048.0 / 44100.0); // the encoder pads to a whole frame
    }
}

TEST_CASE("the Ogg output is deterministic", "[audio][xwma]") {
    const auto xwma = make_xwma(22050, 1, 0.3);
    const auto first = bethconv::audio::xwma_to_ogg(xwma, "a");
    const auto second = bethconv::audio::xwma_to_ogg(xwma, "a");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
}

TEST_CASE("a .fuz splits into lip and XWMA and round-trips through the voice container",
          "[audio][xwma]") {
    const auto xwma = make_xwma(44100, 1, 0.2);
    const Bytes lip{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    Bytes fuz;
    put_tag(fuz, "FUZE");
    put32(fuz, 1);
    put32(fuz, static_cast<std::uint32_t>(lip.size()));
    fuz.insert(fuz.end(), lip.begin(), lip.end());
    fuz.insert(fuz.end(), xwma.begin(), xwma.end());

    const auto parts = bethconv::audio::parse_fuz(fuz, "x.fuz");
    REQUIRE(parts.has_value());
    CHECK(Bytes(parts->lip.begin(), parts->lip.end()) == lip);
    CHECK(Bytes(parts->xwma.begin(), parts->xwma.end()) == xwma);

    const auto converted = bethconv::pack::convert_sound(fuz, "x.fuz", ".fuz");
    REQUIRE_FALSE(converted.failure.has_value());
    const auto& c = converted.bytes;
    REQUIRE(c.size() > bethconv::audio::k_voice_header_size);
    CHECK(std::memcmp(c.data(), "SDVC", 4) == 0);
    CHECK(le32(c, 4) == 1);
    const auto ogg_at = le32(c, 8);
    const auto ogg_size = le32(c, 12);
    const auto lip_at = le32(c, 16);
    const auto lip_size = le32(c, 20);
    CHECK(ogg_at == 24);
    CHECK(lip_at == ogg_at + ogg_size);
    CHECK(lip_at + lip_size == c.size());
    CHECK(Bytes(c.begin() + lip_at, c.end()) == lip);
    CHECK(inspect_ogg(Bytes(c.begin() + ogg_at, c.begin() + lip_at)).ok);
}

TEST_CASE("garbage is refused with a reason", "[audio][xwma]") {
    const Bytes junk(200, std::byte{0x41});
    CHECK_FALSE(bethconv::audio::xwma_to_ogg(junk, "j").has_value());
    CHECK_FALSE(bethconv::audio::xwma_to_ogg({}, "j").has_value());
    CHECK_FALSE(bethconv::audio::parse_fuz(junk, "j").has_value());

    Bytes lie;
    put_tag(lie, "FUZE");
    put32(lie, 1);
    put32(lie, 1000); // lip size past the end
    put32(lie, 0);
    CHECK_FALSE(bethconv::audio::parse_fuz(lie, "j").has_value());

    // A valid RIFF header with no chunks, and a truncated real stream.
    Bytes bare;
    put_tag(bare, "RIFF");
    put32(bare, 4);
    put_tag(bare, "XWMA");
    CHECK_FALSE(bethconv::audio::xwma_to_ogg(bare, "j").has_value());
    auto xwma = make_xwma(44100, 1, 0.2);
    xwma.resize(40);
    CHECK_FALSE(bethconv::audio::xwma_to_ogg(xwma, "j").has_value());
}
