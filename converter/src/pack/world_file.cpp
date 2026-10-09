// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/world.hpp"

#include "bethconv/io/byte_view.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_reader.hpp"

#include "bethconv/pack/world_generated.h"
#include "skydot_formats/flags.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace bethconv::pack {
namespace {

/// The entry of a vector whose `(key)` is `id` (the schema marks them), or
/// null; also for a file without the vector.
template <typename T>
const T* lookup(const flatbuffers::Vector<flatbuffers::Offset<T>>* list, std::uint32_t id) {
    return list != nullptr ? list->LookupByKey(id) : nullptr;
}

/// `table` copied out as its object type, or nothing for null.
template <typename T>
std::optional<typename T::NativeTableType> unpack(const T* table) {
    if (table == nullptr) {
        return std::nullopt;
    }
    std::optional<typename T::NativeTableType> out(std::in_place);
    table->UnPackTo(&*out);
    return out;
}

/// Every table of `list` copied out as its object type.
template <typename T>
std::vector<typename T::NativeTableType> unpack_all(
    const flatbuffers::Vector<flatbuffers::Offset<T>>* list) {
    std::vector<typename T::NativeTableType> out;
    if (list != nullptr) {
        out.reserve(list->size());
        for (const auto* table : *list) {
            table->UnPackTo(&out.emplace_back());
        }
    }
    return out;
}

} // namespace

// ---- reading --------------------------------------------------------------

class WorldFile::Impl {
public:
    io::MappedFile mapping; ///< Empty when bytes are borrowed.
    std::span<const std::byte> bytes;
    const wfb::World* root{};
};

WorldFile::WorldFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WorldFile::WorldFile(WorldFile&&) noexcept = default;
WorldFile& WorldFile::operator=(WorldFile&&) noexcept = default;
WorldFile::~WorldFile() = default;

namespace {

io::ParseResult<const wfb::World*> verify_world(std::span<const std::byte> bytes,
                                                std::string_view origin) {
    io::SpanReader reader(bytes, origin);
    const std::span<const std::uint8_t> raw = io::as_u8(bytes);
    flatbuffers::Verifier verifier(raw.data(), raw.size());
    if (!wfb::VerifyWorldBuffer(verifier)) {
        return reader.fail(io::ErrorKind::corrupt, "not a valid world.fb");
    }
    const auto* root = wfb::GetWorld(raw.data());
    if (root->format_version() != k_world_format_version) {
        return reader.fail(io::ErrorKind::unsupported,
                           "world.fb format version " + std::to_string(root->format_version()) +
                               " is not one this build reads (it reads " +
                               std::to_string(k_world_format_version) + ")");
    }
    return root;
}

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

} // namespace

std::vector<float> terrain_heights(const wfb::TerrainT& terrain) {
    constexpr std::size_t k_grid = k_terrain_grid;
    std::vector<float> out(k_grid * k_grid, 0.0F);
    if (terrain.height_deltas.size() != out.size()) {
        return out;
    }
    float row = terrain.height_offset;
    for (std::size_t y = 0; y < k_grid; ++y) {
        row += static_cast<float>(terrain.height_deltas[y * k_grid]);
        float column = row;
        out[y * k_grid] = column * 8.0F;
        for (std::size_t x = 1; x < k_grid; ++x) {
            column += static_cast<float>(terrain.height_deltas[y * k_grid + x]);
            out[y * k_grid + x] = column * 8.0F;
        }
    }
    return out;
}

io::ParseResult<WorldFile> WorldFile::open(const std::filesystem::path& path) {
    auto mapping = io::MappedFile::open(path);
    if (!mapping) {
        return std::unexpected(std::move(mapping).error());
    }
    auto root = verify_world(mapping->bytes(), mapping->origin());
    if (!root) {
        return std::unexpected(std::move(root).error());
    }
    auto impl = std::make_unique<Impl>();
    impl->mapping = std::move(*mapping);
    impl->bytes = impl->mapping.bytes();
    impl->root = *root;
    return WorldFile(std::move(impl));
}

io::ParseResult<WorldFile> WorldFile::from_bytes(std::span<const std::byte> bytes,
                                                 std::string_view origin) {
    auto root = verify_world(bytes, origin);
    if (!root) {
        return std::unexpected(std::move(root).error());
    }
    auto impl = std::make_unique<Impl>();
    impl->bytes = bytes;
    impl->root = *root;
    return WorldFile(std::move(impl));
}

std::size_t WorldFile::cell_count() const noexcept {
    const auto* cells = impl_->root->cells();
    return cells == nullptr ? 0 : cells->size();
}

std::size_t WorldFile::base_count() const noexcept {
    const auto* bases = impl_->root->bases();
    return bases == nullptr ? 0 : bases->size();
}

std::optional<wfb::CellT> WorldFile::cell(std::uint32_t id) const {
    return unpack(lookup(impl_->root->cells(), id));
}

std::optional<wfb::CellT> WorldFile::cell_at(std::size_t index) const {
    const auto* cells = impl_->root->cells();
    if (cells == nullptr || index >= cells->size()) {
        return std::nullopt;
    }
    return unpack(cells->Get(static_cast<flatbuffers::uoffset_t>(index)));
}

std::optional<wfb::CellT> WorldFile::cell_by_editor_id(std::string_view editor_id) const {
    const auto* cells = impl_->root->cells();
    if (cells == nullptr) {
        return std::nullopt;
    }
    for (const auto* c : *cells) {
        const auto* name = c->editor_id();
        if (name != nullptr && iequals(name->string_view(), editor_id)) {
            return unpack(c);
        }
    }
    return std::nullopt;
}

std::optional<wfb::BaseT> WorldFile::base(std::uint32_t id) const {
    return unpack(lookup(impl_->root->bases(), id));
}

std::size_t WorldFile::quest_count() const noexcept {
    const auto* quests = impl_->root->quests();
    return quests == nullptr ? 0 : quests->size();
}

std::optional<wfb::QuestT> WorldFile::quest(std::uint32_t id) const {
    return unpack(lookup(impl_->root->quests(), id));
}

std::optional<wfb::GlobalT> WorldFile::global(std::uint32_t id) const {
    return unpack(lookup(impl_->root->globals(), id));
}

std::optional<wfb::NpcT> WorldFile::npc(std::uint32_t id) const {
    return unpack(lookup(impl_->root->npcs(), id));
}

std::size_t WorldFile::package_count() const noexcept {
    const auto* list = impl_->root->packages();
    return list != nullptr ? list->size() : 0;
}

std::optional<wfb::PackageT> WorldFile::package(std::uint32_t id) const {
    return unpack(lookup(impl_->root->packages(), id));
}

std::optional<wfb::RaceT> WorldFile::race(std::uint32_t id) const {
    return unpack(lookup(impl_->root->races(), id));
}

std::optional<wfb::ArmorAddonT> WorldFile::armor_addon(std::uint32_t id) const {
    return unpack(lookup(impl_->root->armor_addons(), id));
}

std::vector<wfb::PluginT> WorldFile::plugins() const { return unpack_all(impl_->root->plugins()); }

std::vector<wfb::ActorRef> WorldFile::actors() const {
    std::vector<wfb::ActorRef> out;
    if (const auto* actors = impl_->root->actors()) {
        out.reserve(actors->size());
        for (const auto* a : *actors) {
            out.push_back(*a);
        }
    }
    return out;
}

std::vector<wfb::WorldspaceT> WorldFile::worldspaces() const {
    return unpack_all(impl_->root->worlds());
}

std::optional<wfb::LandTextureT> WorldFile::land_texture(std::uint32_t id) const {
    return unpack(lookup(impl_->root->land_textures(), id));
}

std::optional<wfb::WaterT> WorldFile::water(std::uint32_t id) const {
    return unpack(lookup(impl_->root->waters(), id));
}

std::optional<wfb::ClimateT> WorldFile::climate(std::uint32_t id) const {
    return unpack(lookup(impl_->root->climates(), id));
}

std::optional<wfb::ImageSpaceT> WorldFile::image_space(std::uint32_t id) const {
    return unpack(lookup(impl_->root->image_spaces(), id));
}

std::optional<wfb::VolumetricLightingT> WorldFile::volumetric_lighting(std::uint32_t id) const {
    return unpack(lookup(impl_->root->volumetric_lightings(), id));
}

std::optional<wfb::WeatherT> WorldFile::weather(std::uint32_t id) const {
    return unpack(lookup(impl_->root->weathers(), id));
}

std::optional<wfb::PrecipitationT> WorldFile::precipitation(std::uint32_t id) const {
    return unpack(lookup(impl_->root->precipitations(), id));
}

std::vector<wfb::RegionT> WorldFile::regions() const { return unpack_all(impl_->root->regions()); }

std::optional<wfb::CellT> WorldFile::cell_at_grid(std::uint32_t world, std::int32_t x,
                                                 std::int32_t y) const {
    const auto* cells = impl_->root->cells();
    if (cells == nullptr) {
        return std::nullopt;
    }
    for (const auto* c : *cells) {
        if (c->world() == world && c->has_grid() && c->grid_x() == x && c->grid_y() == y &&
            !skydot::formats::has_flag(c->flags(), wfb::CellFlags::interior) &&
            !c->persistent()) {
            return unpack(c);
        }
    }
    return std::nullopt;
}

} // namespace bethconv::pack
