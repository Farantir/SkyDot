// SPDX-License-Identifier: GPL-3.0-or-later
//
// What cells place: LIGH, the other types that have a model or scripts, MATO,
// ADDN, GRAS, and the land textures with the texture sets they name. Private
// to pack/world/.
#pragma once

#include "context.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/merge.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <array>
#include <span>
#include <vector>

namespace bethconv::pack::detail {

struct TextureSetEntry {
    std::string diffuse;
    std::string normal;
    /// All of TX00..TX07 as normalized virtual paths, empty if absent.
    std::array<std::string, 8> textures;
};

/// One MODS entry whose TXST is not resolved yet (global FormID).
struct PendingAlternate {
    std::string shape;
    std::int32_t index{};
    std::uint32_t texture_set{};
};

/// An LTEX: the table, and the TXST it names, whose paths fill the table in when
/// every record has been read.
struct LandTextureEntry {
    wfb::LandTextureT table;
    std::uint32_t texture_set{};
};

/// An item base or a container waiting for `finish` to join its base (or
/// become one: items without a model are bases too).
struct ItemEntry {
    std::uint32_t type{};
    std::string editor_id;
    std::string name;
    std::uint32_t record_flags{};
    std::uint32_t flags{}; ///< CONT DATA flags, into Base.flags.
    bool has_item{};       ///< False for a CONT: only `items`.
    wfb::ItemDataT item;
    std::vector<wfb::NpcItem> items;
};

class BaseCollector {
public:
    explicit BaseCollector(CollectContext& shared) : shared_(shared) {}

    /// LIGH, MATO, ADDN, TXST, LTEX, GRAS, AMMO, PROJ, SNDR, SOUN, SOPM, KYWD, MESG or EXPL; nothing for another type.
    void collect(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);

    /// Any other type: kept as a base if it has a model or scripts.
    void collect_generic(const record::MergedRecord& merged, io::SpanReader& data,
                         const record::FormContext& form_ctx);

    /// ALCH, AMMO, ARMO, BOOK, INGR, KEYM, MISC, SLGM, SCRL, WEAP (the item
    /// data) and CONT (the start inventory): read from `data` alone, joined to
    /// the base in `finish`. Nothing for another type.
    void collect_item(const record::MergedRecord& merged, io::SpanReader& data,
                      const record::FormContext& form_ctx);

    /// True for the types `collect_item` reads.
    [[nodiscard]] static bool is_item_type(io::FourCC type) noexcept;

    /// The bases, land textures (with their texture set's paths), material
    /// objects, grasses and addon nodes, each in id order, into `world`.
    void finish(wfb::WorldT& world);

private:
    /// Reads a MODS field's bytes; the entries wait for their texture sets.
    void keep_alternates(const record::MergedRecord& merged, std::span<const std::byte> mods);
    void on_light(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx);
    void on_material_object(const record::MergedRecord& merged, io::SpanReader& data);
    void on_land_texture(const record::MergedRecord& merged, io::SpanReader& data,
                         const record::FormContext& form_ctx);
    void on_addon_node(const record::MergedRecord& merged, io::SpanReader& data);
    void on_grass(const record::MergedRecord& merged, io::SpanReader& data);
    void on_ammo(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);
    void on_sound_descriptor(const record::MergedRecord& merged, io::SpanReader& data,
                             const record::FormContext& form_ctx);
    void on_sound_marker(const record::MergedRecord& merged, io::SpanReader& data,
                         const record::FormContext& form_ctx);
    void on_explosion(const record::MergedRecord& merged, io::SpanReader& data);
    void on_keyword(const record::MergedRecord& merged, io::SpanReader& data);
    void on_message(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx);
    void on_sound_output_model(const record::MergedRecord& merged, io::SpanReader& data);
    void on_projectile(const record::MergedRecord& merged, io::SpanReader& data,
                       const record::FormContext& form_ctx);
    void on_texture_set(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx);

    CollectContext& shared_;
    std::map<std::uint32_t, wfb::BaseT> bases_;
    std::map<std::uint32_t, ItemEntry> item_entries_;
    std::map<std::uint32_t, wfb::MaterialObjectT> material_objects_;
    std::map<std::uint32_t, wfb::GrassT> grasses_;
    std::map<std::uint32_t, wfb::AddonNodeT> addons_;
    std::map<std::uint32_t, wfb::AmmoT> ammos_;
    std::map<std::uint32_t, wfb::ProjectileT> projectiles_;
    std::map<std::uint32_t, wfb::SoundDescriptorT> sound_descriptors_;
    std::map<std::uint32_t, wfb::SoundMarkerT> sound_markers_;
    std::map<std::uint32_t, wfb::SoundOutputModelT> sound_output_models_;
    std::map<std::uint32_t, wfb::ExplosionT> explosions_;
    std::map<std::uint32_t, wfb::KeywordT> keywords_;
    std::map<std::uint32_t, wfb::MessageT> messages_;
    std::map<std::uint32_t, LandTextureEntry> land_textures_;
    std::unordered_map<std::uint32_t, TextureSetEntry> texture_sets_;
    std::map<std::uint32_t, std::vector<PendingAlternate>> pending_alternates_;
};

} // namespace bethconv::pack::detail
