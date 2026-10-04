// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/lod_asset.hpp"

#include "bethconv/io/byte_view.hpp"
#include "bethconv/pack/lod_generated.h"

#include <utility>

namespace bethconv::pack {

namespace {

namespace lfb = bethconv::pack::lfb;

constexpr std::size_t k_settings_size = 16;
constexpr std::size_t k_tree_type_size = 32;
constexpr std::size_t k_tree_size = 32;

/// Reads in sequence and keeps the first failure.
class Cursor {
public:
    explicit Cursor(io::SpanReader& r) : r_{r} {}

    template <typename T>
    T get() {
        if (failure_) {
            return T{};
        }
        auto v = r_.get<T>();
        if (!v) {
            failure_ = std::move(v).error();
            return T{};
        }
        return *v;
    }

    /// `count` entries of `size` bytes must fit in what is left.
    bool fits(std::uint64_t count, std::size_t size, std::string_view what) {
        if (!failure_ && count * size > r_.remaining()) {
            failure_ = r_.fail(io::ErrorKind::truncated,
                               std::to_string(count) + " " + std::string(what) + " in " +
                                   std::to_string(r_.remaining()) + " bytes")
                           .error();
        }
        return !failure_;
    }

    [[nodiscard]] std::optional<io::ParseError>& failure() noexcept { return failure_; }

private:
    io::SpanReader& r_;
    std::optional<io::ParseError> failure_;
};

bool power_of_two_level(std::int32_t level) {
    return level >= 1 && level <= 256 && (level & (level - 1)) == 0;
}

} // namespace

io::ParseResult<LodData> read_lod_source(std::span<const std::byte> bytes, std::string_view extension,
                                         std::string_view origin) {
    io::SpanReader r(bytes, origin);
    Cursor in(r);
    LodData out;
    if (extension == ".lod") {
        if (bytes.size() != k_settings_size) {
            return r.fail(io::ErrorKind::bad_value,
                          "LOD settings are " + std::to_string(bytes.size()) + " bytes, not 16");
        }
        LodSettings s;
        s.south_west_x = in.get<std::int16_t>();
        s.south_west_y = in.get<std::int16_t>();
        s.stride = in.get<std::int32_t>();
        s.lowest_level = in.get<std::int32_t>();
        s.highest_level = in.get<std::int32_t>();
        if (!in.failure() && (s.stride <= 0 || !power_of_two_level(s.lowest_level) ||
                              !power_of_two_level(s.highest_level) ||
                              s.lowest_level > s.highest_level)) {
            return r.fail(io::ErrorKind::bad_value,
                          "LOD settings: stride " + std::to_string(s.stride) + ", levels " +
                              std::to_string(s.lowest_level) + " to " +
                              std::to_string(s.highest_level));
        }
        out.settings = s;
    } else if (extension == ".lst") {
        const auto count = in.get<std::uint32_t>();
        if (in.fits(count, k_tree_type_size, "tree types")) {
            out.tree_types.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                TreeType t;
                t.index = in.get<std::uint32_t>();
                t.width = in.get<float>();
                t.height = in.get<float>();
                t.u0 = in.get<float>();
                t.v0 = in.get<float>();
                t.u1 = in.get<float>();
                t.v1 = in.get<float>();
                t.unknown = in.get<std::uint32_t>();
                out.tree_types.push_back(t);
            }
        }
    } else if (extension == ".btt") {
        const auto blocks = in.get<std::uint32_t>();
        // A block is at least its 8-byte header.
        if (in.fits(blocks, 8, "tree blocks")) {
            for (std::uint32_t b = 0; b < blocks && !in.failure(); ++b) {
                const auto type = in.get<std::uint32_t>();
                const auto count = in.get<std::uint32_t>();
                if (!in.fits(count, k_tree_size, "trees")) {
                    break;
                }
                for (std::uint32_t i = 0; i < count; ++i) {
                    TreeInstance t;
                    t.type = type;
                    t.x = in.get<float>();
                    t.y = in.get<float>();
                    t.z = in.get<float>();
                    t.rotation = in.get<float>();
                    t.scale = in.get<float>();
                    t.ref = in.get<std::uint32_t>();
                    t.unknown1 = in.get<std::uint32_t>();
                    t.unknown2 = in.get<std::uint32_t>();
                    out.trees.push_back(t);
                }
            }
        }
    } else {
        return r.fail(io::ErrorKind::unsupported, "not a LOD data file: " + std::string(extension));
    }
    if (in.failure()) {
        return std::unexpected(std::move(*in.failure()));
    }
    if (extension == ".btt") {
        out.trailing_bytes = r.remaining();
    } else if (!r.at_end()) {
        return r.fail(io::ErrorKind::bad_value,
                      std::to_string(r.remaining()) + " bytes left over");
    }
    return out;
}

std::vector<std::byte> write_lod_asset(const LodData& data) {
    flatbuffers::FlatBufferBuilder b(1024);
    std::vector<lfb::TreeType> types;
    types.reserve(data.tree_types.size());
    for (const auto& t : data.tree_types) {
        types.emplace_back(t.index, t.width, t.height, t.u0, t.v0, t.u1, t.v1, t.unknown);
    }
    std::vector<lfb::TreeInstance> trees;
    trees.reserve(data.trees.size());
    for (const auto& t : data.trees) {
        trees.emplace_back(t.type, t.x, t.y, t.z, t.rotation, t.scale, t.ref, t.unknown1, t.unknown2);
    }
    const auto types_off = b.CreateVectorOfStructs(types);
    const auto trees_off = b.CreateVectorOfStructs(trees);
    lfb::LodBuilder lb(b);
    lb.add_format_version(k_lod_format_version);
    std::optional<lfb::LodSettings> settings;
    if (data.settings) {
        const auto& s = *data.settings;
        settings = lfb::LodSettings(s.south_west_x, s.south_west_y, s.stride, s.lowest_level,
                                    s.highest_level);
        lb.add_settings(&*settings);
    }
    lb.add_tree_types(types_off);
    lb.add_trees(trees_off);
    lfb::FinishLodBuffer(b, lb.Finish());
    const std::span<const std::uint8_t> raw(b.GetBufferPointer(), b.GetSize());
    const auto bytes = std::as_bytes(raw);
    return {bytes.begin(), bytes.end()};
}

io::ParseResult<LodData> read_lod_asset(std::span<const std::byte> bytes, std::string_view origin) {
    io::SpanReader reader(bytes, origin);
    const auto* raw = io::as_u8(bytes).data();
    flatbuffers::Verifier verifier(raw, bytes.size());
    if (bytes.empty() || !lfb::VerifyLodBuffer(verifier)) {
        return reader.fail(io::ErrorKind::corrupt, "not a valid LOD asset");
    }
    const auto* root = lfb::GetLod(raw);
    if (root->format_version() != k_lod_format_version) {
        return reader.fail(io::ErrorKind::unsupported,
                           "LOD asset format " + std::to_string(root->format_version()) +
                               " is not one this build reads (it reads " +
                               std::to_string(k_lod_format_version) + ")");
    }
    LodData out;
    if (const auto* s = root->settings()) {
        out.settings = LodSettings{.south_west_x = s->south_west_x(),
                                   .south_west_y = s->south_west_y(),
                                   .stride = s->stride(),
                                   .lowest_level = s->lowest_level(),
                                   .highest_level = s->highest_level()};
    }
    if (const auto* types = root->tree_types()) {
        for (const auto* t : *types) {
            out.tree_types.push_back(TreeType{.index = t->index(),
                                              .width = t->width(),
                                              .height = t->height(),
                                              .u0 = t->u0(),
                                              .v0 = t->v0(),
                                              .u1 = t->u1(),
                                              .v1 = t->v1(),
                                              .unknown = t->unknown()});
        }
    }
    if (const auto* trees = root->trees()) {
        for (const auto* t : *trees) {
            out.trees.push_back(TreeInstance{.type = t->type(),
                                             .x = t->x(),
                                             .y = t->y(),
                                             .z = t->z(),
                                             .rotation = t->rotation(),
                                             .scale = t->scale(),
                                             .ref = t->ref(),
                                             .unknown1 = t->unknown1(),
                                             .unknown2 = t->unknown2()});
        }
    }
    return out;
}

} // namespace bethconv::pack
