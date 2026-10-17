// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/audio/xwma.hpp"

#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace bethconv::audio {

namespace {

using io::ErrorKind;
using io::ParseError;

[[nodiscard]] std::unexpected<ParseError> fail(std::string_view origin, std::size_t offset,
                                               ErrorKind kind, std::string detail) {
    return std::unexpected(ParseError{
        .origin = std::string(origin), .offset = offset, .kind = kind, .detail = std::move(detail)});
}

[[nodiscard]] std::string av_message(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof buffer);
    return buffer;
}

void quiet_libav() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_QUIET); });
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[at + i])) << (8 * i);
    }
    return value;
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
    }
}

// --- libav input over a memory span -----------------------------------------

struct MemoryReader {
    std::span<const std::byte> data;
    std::size_t position = 0;
};

int read_packet(void* opaque, std::uint8_t* buffer, int size) {
    auto* reader = static_cast<MemoryReader*>(opaque);
    const std::size_t left = reader->data.size() - reader->position;
    if (left == 0) {
        return AVERROR_EOF;
    }
    const auto count = std::min(left, static_cast<std::size_t>(size));
    std::memcpy(buffer, reader->data.data() + reader->position, count);
    reader->position += count;
    return static_cast<int>(count);
}

std::int64_t seek_packet(void* opaque, std::int64_t offset, int whence) {
    auto* reader = static_cast<MemoryReader*>(opaque);
    const auto size = static_cast<std::int64_t>(reader->data.size());
    if ((whence & AVSEEK_SIZE) != 0) {
        return size;
    }
    std::int64_t target = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: target = offset; break;
    case SEEK_CUR: target = static_cast<std::int64_t>(reader->position) + offset; break;
    case SEEK_END: target = size + offset; break;
    default: return AVERROR(EINVAL);
    }
    if (target < 0 || target > size) {
        return AVERROR(EINVAL);
    }
    reader->position = static_cast<std::size_t>(target);
    return target;
}

struct FormatDeleter {
    void operator()(AVFormatContext* context) const { avformat_close_input(&context); }
};
struct IoDeleter {
    void operator()(AVIOContext* io) const {
        if (io != nullptr) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};
struct CodecDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

// --- libvorbis output --------------------------------------------------------

class VorbisWriter {
public:
    VorbisWriter() = default;
    VorbisWriter(const VorbisWriter&) = delete;
    VorbisWriter& operator=(const VorbisWriter&) = delete;
    ~VorbisWriter() {
        if (stream_ready_) {
            ogg_stream_clear(&stream_);
        }
        if (block_ready_) {
            vorbis_block_clear(&block_);
        }
        if (dsp_ready_) {
            vorbis_dsp_clear(&dsp_);
        }
        vorbis_comment_clear(&comment_);
        vorbis_info_clear(&info_);
    }

    /// False if libvorbis refuses the channel count and rate.
    [[nodiscard]] bool start(int channels, long rate) {
        vorbis_info_init(&info_);
        vorbis_comment_init(&comment_);
        if (vorbis_encode_init_vbr(&info_, channels, rate, k_vorbis_quality) != 0) {
            return false;
        }
        channels_ = channels;
        if (vorbis_analysis_init(&dsp_, &info_) != 0) {
            return false;
        }
        dsp_ready_ = true;
        if (vorbis_block_init(&dsp_, &block_) != 0) {
            return false;
        }
        block_ready_ = true;
        ogg_stream_init(&stream_, static_cast<int>(k_ogg_serial));
        stream_ready_ = true;

        ogg_packet identification{};
        ogg_packet comments{};
        ogg_packet codebooks{};
        if (vorbis_analysis_headerout(&dsp_, &comment_, &identification, &comments, &codebooks) !=
            0) {
            return false;
        }
        ogg_stream_packetin(&stream_, &identification);
        ogg_stream_packetin(&stream_, &comments);
        ogg_stream_packetin(&stream_, &codebooks);
        // The headers end on a page boundary, as the Ogg Vorbis spec asks.
        ogg_page page;
        while (ogg_stream_flush(&stream_, &page) != 0) {
            append(page);
        }
        return true;
    }

    /// `planes[c]` holds `count` samples of channel c.
    void write(const float* const* planes, int count) {
        float** buffer = vorbis_analysis_buffer(&dsp_, count);
        for (int c = 0; c < channels_; ++c) {
            std::memcpy(buffer[c], planes[c], static_cast<std::size_t>(count) * sizeof(float));
        }
        vorbis_analysis_wrote(&dsp_, count);
        drain();
    }

    std::vector<std::byte> finish() {
        vorbis_analysis_wrote(&dsp_, 0);
        drain();
        ogg_page page;
        while (ogg_stream_flush(&stream_, &page) != 0) {
            append(page);
        }
        return std::move(out_);
    }

private:
    void drain() {
        while (vorbis_analysis_blockout(&dsp_, &block_) == 1) {
            vorbis_analysis(&block_, nullptr);
            vorbis_bitrate_addblock(&block_);
            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&dsp_, &packet) != 0) {
                ogg_stream_packetin(&stream_, &packet);
                ogg_page page;
                while (ogg_stream_pageout(&stream_, &page) != 0) {
                    append(page);
                }
            }
        }
    }

    void append(const ogg_page& page) {
        const auto* header = reinterpret_cast<const std::byte*>(page.header);
        const auto* body = reinterpret_cast<const std::byte*>(page.body);
        out_.insert(out_.end(), header, header + page.header_len);
        out_.insert(out_.end(), body, body + page.body_len);
    }

    vorbis_info info_{};
    vorbis_comment comment_{};
    vorbis_dsp_state dsp_{};
    vorbis_block block_{};
    ogg_stream_state stream_{};
    bool dsp_ready_ = false;
    bool block_ready_ = false;
    bool stream_ready_ = false;
    int channels_ = 0;
    std::vector<std::byte> out_;
};

} // namespace

io::ParseResult<std::vector<std::byte>> xwma_to_ogg(std::span<const std::byte> xwma,
                                                    std::string_view origin) {
    if (xwma.size() < 12 || std::memcmp(xwma.data(), "RIFF", 4) != 0 ||
        std::memcmp(xwma.data() + 8, "XWMA", 4) != 0) {
        return fail(origin, 0, ErrorKind::bad_magic, "not an XWMA RIFF");
    }
    quiet_libav();

    MemoryReader reader{.data = xwma};
    constexpr int k_io_buffer = 4096;
    auto* io_buffer = static_cast<std::uint8_t*>(av_malloc(k_io_buffer));
    if (io_buffer == nullptr) {
        return fail(origin, 0, ErrorKind::corrupt, "out of memory");
    }
    std::unique_ptr<AVIOContext, IoDeleter> io(
        avio_alloc_context(io_buffer, k_io_buffer, 0, &reader, read_packet, nullptr, seek_packet));
    if (!io) {
        av_free(io_buffer);
        return fail(origin, 0, ErrorKind::corrupt, "out of memory");
    }

    AVFormatContext* raw_format = avformat_alloc_context();
    if (raw_format == nullptr) {
        return fail(origin, 0, ErrorKind::corrupt, "out of memory");
    }
    raw_format->pb = io.get();
    raw_format->flags |= AVFMT_FLAG_CUSTOM_IO;
    // On failure avformat_open_input frees the context itself.
    if (const int code = avformat_open_input(&raw_format, nullptr, nullptr, nullptr); code < 0) {
        return fail(origin, 0, ErrorKind::corrupt, "libavformat: " + av_message(code));
    }
    const std::unique_ptr<AVFormatContext, FormatDeleter> format(raw_format);
    if (const int code = avformat_find_stream_info(format.get(), nullptr); code < 0) {
        return fail(origin, 0, ErrorKind::corrupt, "libavformat: " + av_message(code));
    }
    if (std::string_view(format->iformat->name) != "xwma") {
        return fail(origin, 0, ErrorKind::bad_magic,
                    std::string("not xwma, libavformat found ") + format->iformat->name);
    }

    const AVStream* stream = nullptr;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream = format->streams[i];
            break;
        }
    }
    if (stream == nullptr) {
        return fail(origin, 0, ErrorKind::corrupt, "no audio stream");
    }
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return fail(origin, 0, ErrorKind::unsupported, "no decoder for the stream's codec");
    }
    const std::unique_ptr<AVCodecContext, CodecDeleter> decoder(avcodec_alloc_context3(codec));
    if (!decoder || avcodec_parameters_to_context(decoder.get(), stream->codecpar) < 0) {
        return fail(origin, 0, ErrorKind::corrupt, "cannot set up the decoder");
    }
    if (const int code = avcodec_open2(decoder.get(), codec, nullptr); code < 0) {
        return fail(origin, 0, ErrorKind::unsupported, "libavcodec: " + av_message(code));
    }
    const int channels = decoder->ch_layout.nb_channels;
    const int rate = decoder->sample_rate;
    if (channels < 1 || channels > 8 || rate < 8000 || rate > 192000) {
        return fail(origin, 0, ErrorKind::bad_value,
                    "channels " + std::to_string(channels) + ", rate " + std::to_string(rate));
    }

    VorbisWriter writer;
    if (!writer.start(channels, rate)) {
        return fail(origin, 0, ErrorKind::unsupported, "libvorbis refused the format");
    }

    const std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    const std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!packet || !frame) {
        return fail(origin, 0, ErrorKind::corrupt, "out of memory");
    }

    std::vector<std::vector<float>> interleaved_split; // scratch for packed input
    std::string problem;
    const auto take_frames = [&]() -> bool {
        for (;;) {
            const int code = avcodec_receive_frame(decoder.get(), frame.get());
            if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) {
                return true;
            }
            if (code < 0) {
                problem = "decode: " + av_message(code);
                return false;
            }
            const int count = frame->nb_samples;
            if (frame->format == AV_SAMPLE_FMT_FLTP) {
                writer.write(reinterpret_cast<const float* const*>(frame->extended_data), count);
            } else if (frame->format == AV_SAMPLE_FMT_FLT) {
                interleaved_split.assign(static_cast<std::size_t>(channels),
                                         std::vector<float>(static_cast<std::size_t>(count)));
                const auto* in = reinterpret_cast<const float*>(frame->extended_data[0]);
                std::vector<const float*> planes(static_cast<std::size_t>(channels));
                for (int c = 0; c < channels; ++c) {
                    auto& plane = interleaved_split[static_cast<std::size_t>(c)];
                    for (int s = 0; s < count; ++s) {
                        plane[static_cast<std::size_t>(s)] = in[s * channels + c];
                    }
                    planes[static_cast<std::size_t>(c)] = plane.data();
                }
                writer.write(planes.data(), count);
            } else {
                problem = std::string("unsupported sample format ") +
                          std::to_string(frame->format);
                return false;
            }
            av_frame_unref(frame.get());
        }
    };

    for (;;) {
        const int code = av_read_frame(format.get(), packet.get());
        if (code == AVERROR_EOF) {
            break;
        }
        if (code < 0) {
            return fail(origin, reader.position, ErrorKind::corrupt,
                        "libavformat: " + av_message(code));
        }
        const bool mine = packet->stream_index == stream->index;
        const int sent = mine ? avcodec_send_packet(decoder.get(), packet.get()) : 0;
        av_packet_unref(packet.get());
        if (sent < 0) {
            return fail(origin, reader.position, ErrorKind::corrupt,
                        "libavcodec: " + av_message(sent));
        }
        if (mine && !take_frames()) {
            return fail(origin, reader.position, ErrorKind::corrupt, problem);
        }
    }
    avcodec_send_packet(decoder.get(), nullptr);
    if (!take_frames()) {
        return fail(origin, reader.position, ErrorKind::corrupt, problem);
    }
    return writer.finish();
}

io::ParseResult<FuzParts> parse_fuz(std::span<const std::byte> fuz, std::string_view origin) {
    if (fuz.size() < 12 || std::memcmp(fuz.data(), "FUZE", 4) != 0) {
        return fail(origin, 0, ErrorKind::bad_magic, "not a FUZE file");
    }
    if (const auto version = read_u32(fuz, 4); version != 1) {
        return fail(origin, 4, ErrorKind::unsupported, "FUZE version " + std::to_string(version));
    }
    const std::size_t lip_size = read_u32(fuz, 8);
    if (lip_size > fuz.size() - 12) {
        return fail(origin, 8, ErrorKind::out_of_range, "lip size past the end of the file");
    }
    return FuzParts{.lip = fuz.subspan(12, lip_size), .xwma = fuz.subspan(12 + lip_size)};
}

std::vector<std::byte> write_voice_container(std::span<const std::byte> ogg,
                                             std::span<const std::byte> lip) {
    std::vector<std::byte> out;
    out.reserve(k_voice_header_size + ogg.size() + lip.size());
    for (const char c : std::string_view("SDVC")) {
        out.push_back(static_cast<std::byte>(c));
    }
    put_u32(out, k_voice_version);
    put_u32(out, static_cast<std::uint32_t>(k_voice_header_size));
    put_u32(out, static_cast<std::uint32_t>(ogg.size()));
    put_u32(out, static_cast<std::uint32_t>(k_voice_header_size + ogg.size()));
    put_u32(out, static_cast<std::uint32_t>(lip.size()));
    out.insert(out.end(), ogg.begin(), ogg.end());
    out.insert(out.end(), lip.begin(), lip.end());
    return out;
}

} // namespace bethconv::audio
