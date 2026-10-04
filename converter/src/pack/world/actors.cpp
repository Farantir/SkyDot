// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/forms_actor.hpp"
#include "bethconv/record/forms_game.hpp"
#include "bethconv/record/forms_object.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>

namespace bethconv::pack::detail {
namespace {

using io::FourCC;

/// The precomputed FaceGen head: named by the plugin owning the form and
/// the form's id within it.
std::string face_model(const record::LoadOrder& order, const record::MergedRecord& merged) {
    const auto& entries = order.entries();
    if (merged.owner >= entries.size()) {
        return {};
    }
    const auto& owner = entries[merged.owner];
    std::array<char, 16> id{};
    std::snprintf(id.data(), id.size(), "%08x", merged.form.value & owner.object_mask());
    std::string plugin = owner.name;
    std::ranges::transform(plugin, plugin.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string hex(id.data());
    return "meshes/actors/character/facegendata/facegeom/" + plugin + "/" + hex + ".nif";
}

} // namespace

void ActorCollector::collect(const record::MergedRecord& merged, io::SpanReader& data,
                             const record::FormContext& form_ctx) {
    switch (merged.type.value) {
    case FourCC{"NPC_"}.value:
        on_npc(merged, data, form_ctx);
        break;
    case FourCC{"RACE"}.value:
        on_race(merged, data, form_ctx);
        break;
    case FourCC{"ARMO"}.value:
        on_armor(merged, data, form_ctx);
        break;
    case FourCC{"ARMA"}.value:
        on_armor_addon(merged, data, form_ctx);
        break;
    case FourCC{"OTFT"}.value:
        on_outfit(merged, data, form_ctx);
        break;
    case FourCC{"LVLI"}.value:
        on_leveled_item(merged, data, form_ctx);
        break;
    case FourCC{"LVLN"}.value:
        on_leveled_npc(merged, data, form_ctx);
        break;
    default:
        break;
    }
}

void ActorCollector::on_npc(const record::MergedRecord& merged, io::SpanReader& data,
                            const record::FormContext& form_ctx) {
    auto npc = record::parse_npc(data, form_ctx);
    if (!npc) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    WorldNpc out;
    out.id = merged.form.value;
    out.editor_id = npc->editor_id;
    out.name = npc->name.text;
    out.flags = npc->flags;
    out.level = npc->level;
    out.race = shared_.global(merged, npc->race, failed);
    out.template_form = shared_.global(merged, npc->npc_template, failed);
    out.template_flags = npc->template_flags;
    out.skin = shared_.global(merged, npc->worn_armor, failed);
    out.default_outfit = shared_.global(merged, npc->default_outfit, failed);
    out.sleeping_outfit = shared_.global(merged, npc->sleeping_outfit, failed);
    out.height = npc->height;
    out.weight = npc->weight;
    out.head_parts = shared_.global_all(merged, npc->head_parts, failed);
    out.packages = shared_.global_all(merged, npc->packages, failed);
    out.default_package_list = shared_.global(merged, npc->default_package_list, failed);
    for (const auto& f : npc->factions) {
        out.factions.emplace_back(shared_.global(merged, f.faction, failed), f.rank);
    }
    for (const auto& item : npc->items) {
        out.items.emplace_back(shared_.global(merged, item.item, failed), item.count);
    }
    out.face_model = face_model(shared_.order(), merged);
    out.skin_tone = {npc->skin_red, npc->skin_green, npc->skin_blue};
    if (failed) {
        ++shared_.stats().unresolved;
    }
    npcs_[out.id] = std::move(out);
}

void ActorCollector::on_race(const record::MergedRecord& merged, io::SpanReader& data,
                             const record::FormContext& form_ctx) {
    auto race = record::parse_race(data, form_ctx);
    if (!race) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    WorldRace out;
    out.id = merged.form.value;
    out.editor_id = race->editor_id;
    for (std::size_t sex = 0; sex < 2; ++sex) {
        const auto& s = race->sexes[sex];
        out.skeletons[sex] = s.skeleton.empty() ? std::string{} : model_vpath(s.skeleton);
        out.behaviours[sex] = s.behaviour.empty() ? std::string{} : model_vpath(s.behaviour);
        for (const auto& part : s.body_parts) {
            out.body_parts.push_back(WorldRace::BodyPart{
                .female = sex == 1,
                .index = part.index,
                .model = part.model.empty() ? std::string{} : model_vpath(part.model)});
        }
        out.head_parts[sex] = shared_.global_all(merged, s.head_parts, failed);
    }
    out.skin = shared_.global(merged, race->skin, failed);
    out.heights = race->height;
    out.weights = race->weight;
    out.flags = race->flags;
    out.armor_race = shared_.global(merged, race->armor_race, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    races_[out.id] = std::move(out);
}

void ActorCollector::on_armor(const record::MergedRecord& merged, io::SpanReader& data,
                              const record::FormContext& form_ctx) {
    auto armor = record::parse_armor(data, form_ctx);
    if (!armor) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    WorldArmor out{.id = merged.form.value,
                   .editor_id = armor->editor_id,
                   .slots = armor->body.slots,
                   .race = shared_.global(merged, armor->race, failed),
                   .addons = shared_.global_all(merged, armor->addons, failed)};
    if (failed) {
        ++shared_.stats().unresolved;
    }
    armors_[out.id] = std::move(out);
}

void ActorCollector::on_armor_addon(const record::MergedRecord& merged, io::SpanReader& data,
                                    const record::FormContext& form_ctx) {
    auto addon = record::parse_armor_addon(data, form_ctx);
    if (!addon) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    WorldArmorAddon out;
    out.id = merged.form.value;
    out.editor_id = addon->editor_id;
    out.slots = addon->body.slots;
    out.race = shared_.global(merged, addon->race, failed);
    out.additional_races = shared_.global_all(merged, addon->additional_races, failed);
    out.models = {addon->male_model.empty() ? std::string{} : model_vpath(addon->male_model.path),
                  addon->female_model.empty() ? std::string{} : model_vpath(addon->female_model.path)};
    out.priorities = {addon->male_priority, addon->female_priority};
    out.weight_sliders = {addon->male_weight_slider, addon->female_weight_slider};
    if (failed) {
        ++shared_.stats().unresolved;
    }
    armor_addons_[out.id] = std::move(out);
}

void ActorCollector::on_outfit(const record::MergedRecord& merged, io::SpanReader& data,
                               const record::FormContext& form_ctx) {
    auto outfit = record::parse_outfit(data, form_ctx);
    if (!outfit) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    outfits_[merged.form.value] =
        WorldOutfit{.id = merged.form.value,
                    .items = shared_.global_all(merged, outfit->items, failed)};
    if (failed) {
        ++shared_.stats().unresolved;
    }
}

template <typename Entries>
void ActorCollector::add_leveled(const record::MergedRecord& merged, std::uint8_t flags,
                                 std::uint8_t chance_none, const Entries& entries) {
    bool failed = false;
    WorldLeveledList out{.id = merged.form.value,
                         .type = merged.type.value,
                         .flags = flags,
                         .chance_none = chance_none,
                         .entries = {}};
    for (const auto& e : entries) {
        out.entries.push_back({.level = e.level,
                               .count = e.count,
                               .form = shared_.global(merged, e.reference, failed)});
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    leveled_lists_[out.id] = std::move(out);
}

void ActorCollector::on_leveled_item(const record::MergedRecord& merged, io::SpanReader& data,
                                     const record::FormContext& form_ctx) {
    auto list = record::parse_leveled_item(data, form_ctx);
    if (!list) {
        ++shared_.stats().parse_errors;
        return;
    }
    add_leveled(merged, list->flags, list->chance_none, list->entries);
}

void ActorCollector::on_leveled_npc(const record::MergedRecord& merged, io::SpanReader& data,
                                    const record::FormContext& form_ctx) {
    auto list = record::parse_leveled_npc(data, form_ctx);
    if (!list) {
        ++shared_.stats().parse_errors;
        return;
    }
    add_leveled(merged, list->flags, list->chance_none, list->entries);
}

std::vector<flatbuffers::Offset<wfb::Npc>> ActorCollector::write_npcs(
    flatbuffers::FlatBufferBuilder& builder,
    const FormLists& form_lists) {
    auto& stats = shared_.stats();
    // DPLT names an FLST of packages.
    const auto default_packages = [&](const WorldNpc& n) {
        const auto found = form_lists.find(n.default_package_list);
        return found != form_lists.end() ? found->second : std::vector<std::uint32_t>{};
    };
    std::vector<flatbuffers::Offset<wfb::Npc>> npcs;
    for (const auto& [id, n] : npcs_) {
        std::vector<wfb::NpcItem> items;
        for (const auto& [form, count] : n.items) {
            items.emplace_back(form, count);
        }
        std::vector<wfb::NpcFaction> factions;
        for (const auto& [faction, rank] : n.factions) {
            factions.emplace_back(faction, rank);
        }
        npcs.push_back(wfb::CreateNpc(builder, n.id, builder.CreateString(n.editor_id),
                                      builder.CreateString(n.name), n.flags, n.level, n.race,
                                      n.template_form, n.template_flags, n.skin, n.default_outfit,
                                      n.sleeping_outfit, n.height, n.weight,
                                      builder.CreateVector(n.head_parts),
                                      builder.CreateVectorOfStructs(items),
                                      builder.CreateString(n.face_model),
                                      builder.CreateVector(std::vector<float>(n.skin_tone.begin(), n.skin_tone.end())),
                                      builder.CreateVector(n.packages),
                                      builder.CreateVector(default_packages(n)),
                                      builder.CreateVectorOfStructs(factions)));
        ++stats.npcs;
    }
    return npcs;
}

std::vector<flatbuffers::Offset<wfb::Race>> ActorCollector::write_races(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    const auto strings = [&](const auto& list) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> offsets;
        for (const auto& text : list) {
            offsets.push_back(builder.CreateString(text));
        }
        return builder.CreateVector(offsets);
    };
    std::vector<flatbuffers::Offset<wfb::Race>> races;
    for (const auto& [id, r] : races_) {
        std::vector<flatbuffers::Offset<wfb::RaceBodyPart>> parts;
        for (const auto& p : r.body_parts) {
            parts.push_back(wfb::CreateRaceBodyPart(builder, p.female, p.index, builder.CreateString(p.model)));
        }
        const std::vector<float> heights(r.heights.begin(), r.heights.end());
        const std::vector<float> weights(r.weights.begin(), r.weights.end());
        races.push_back(wfb::CreateRace(builder, r.id, builder.CreateString(r.editor_id),
                                        strings(r.skeletons), strings(r.behaviours), r.skin,
                                        builder.CreateVector(heights), builder.CreateVector(weights),
                                        r.flags, builder.CreateVector(parts),
                                        builder.CreateVector(r.head_parts[0]),
                                        builder.CreateVector(r.head_parts[1]), r.armor_race));
        ++stats.races;
    }
    return races;
}

std::vector<flatbuffers::Offset<wfb::Armor>> ActorCollector::write_armors(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Armor>> armors;
    for (const auto& [id, a] : armors_) {
        armors.push_back(wfb::CreateArmor(builder, a.id, builder.CreateString(a.editor_id), a.slots,
                                          a.race, builder.CreateVector(a.addons)));
        ++stats.armors;
    }
    return armors;
}

std::vector<flatbuffers::Offset<wfb::ArmorAddon>> ActorCollector::write_armor_addons(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::ArmorAddon>> addons;
    for (const auto& [id, a] : armor_addons_) {
        addons.push_back(wfb::CreateArmorAddon(
            builder, a.id, builder.CreateString(a.editor_id), a.slots, a.race,
            builder.CreateVector(a.additional_races), builder.CreateString(a.models[0]),
            builder.CreateString(a.models[1]), a.priorities[0], a.priorities[1], a.weight_sliders[0],
            a.weight_sliders[1]));
        ++stats.armor_addons;
    }
    return addons;
}

std::vector<flatbuffers::Offset<wfb::Outfit>> ActorCollector::write_outfits(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Outfit>> outfits;
    for (const auto& [id, o] : outfits_) {
        outfits.push_back(wfb::CreateOutfit(builder, o.id, builder.CreateVector(o.items)));
        ++stats.outfits;
    }
    return outfits;
}

std::vector<flatbuffers::Offset<wfb::LeveledList>> ActorCollector::write_leveled_lists(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::LeveledList>> leveled;
    for (const auto& [id, l] : leveled_lists_) {
        std::vector<wfb::LeveledEntry> entries;
        for (const auto& e : l.entries) {
            entries.emplace_back(e.level, e.count, e.form);
        }
        leveled.push_back(wfb::CreateLeveledList(builder, l.id, l.type, l.flags, l.chance_none,
                                                 builder.CreateVectorOfStructs(entries)));
        ++stats.leveled_lists;
    }
    return leveled;
}

} // namespace bethconv::pack::detail
