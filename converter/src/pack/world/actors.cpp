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
#include <memory>
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
    wfb::NpcT out;
    out.id = merged.form.value;
    out.editor_id = npc->editor_id;
    out.name = npc->name.text;
    out.flags = static_cast<wfb::NpcFlags>(npc->flags);
    out.level = npc->level;
    out.race = shared_.global(merged, npc->race, failed);
    out.template_ = shared_.global(merged, npc->npc_template, failed);
    out.template_flags = static_cast<wfb::NpcTemplateFlags>(npc->template_flags);
    out.skin = shared_.global(merged, npc->worn_armor, failed);
    out.default_outfit = shared_.global(merged, npc->default_outfit, failed);
    out.sleeping_outfit = shared_.global(merged, npc->sleeping_outfit, failed);
    out.height = npc->height;
    out.weight = npc->weight;
    out.head_parts = shared_.global_all(merged, npc->head_parts, failed);
    out.packages = shared_.global_all(merged, npc->packages, failed);
    default_package_lists_[out.id] = shared_.global(merged, npc->default_package_list, failed);
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
    wfb::RaceT out;
    out.id = merged.form.value;
    out.editor_id = race->editor_id;
    out.skeletons.resize(2);
    out.behaviours.resize(2);
    for (std::size_t sex = 0; sex < 2; ++sex) {
        const auto& s = race->sexes[sex];
        out.skeletons[sex] = s.skeleton.empty() ? std::string{} : model_vpath(s.skeleton);
        out.behaviours[sex] = s.behaviour.empty() ? std::string{} : model_vpath(s.behaviour);
        for (const auto& part : s.body_parts) {
            auto& p = out.body_parts.emplace_back(std::make_unique<wfb::RaceBodyPartT>());
            p->female = sex == 1;
            p->index = part.index;
            p->model = part.model.empty() ? std::string{} : model_vpath(part.model);
        }
        (sex == 0 ? out.head_parts_male : out.head_parts_female) =
            shared_.global_all(merged, s.head_parts, failed);
    }
    out.skin = shared_.global(merged, race->skin, failed);
    out.heights.assign(race->height.begin(), race->height.end());
    out.weights.assign(race->weight.begin(), race->weight.end());
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
    wfb::ArmorT out;
    out.id = merged.form.value;
    out.editor_id = armor->editor_id;
    out.slots = armor->body.slots;
    out.race = shared_.global(merged, armor->race, failed);
    out.addons = shared_.global_all(merged, armor->addons, failed);
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
    wfb::ArmorAddonT out;
    out.id = merged.form.value;
    out.editor_id = addon->editor_id;
    out.slots = addon->body.slots;
    out.race = shared_.global(merged, addon->race, failed);
    out.additional_races = shared_.global_all(merged, addon->additional_races, failed);
    out.male_model = addon->male_model.empty() ? std::string{} : model_vpath(addon->male_model.path);
    out.female_model =
        addon->female_model.empty() ? std::string{} : model_vpath(addon->female_model.path);
    out.male_priority = addon->male_priority;
    out.female_priority = addon->female_priority;
    out.male_weight_slider = addon->male_weight_slider;
    out.female_weight_slider = addon->female_weight_slider;
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
    wfb::OutfitT out;
    out.id = merged.form.value;
    out.items = shared_.global_all(merged, outfit->items, failed);
    outfits_[out.id] = std::move(out);
    if (failed) {
        ++shared_.stats().unresolved;
    }
}

template <typename Entries>
void ActorCollector::add_leveled(const record::MergedRecord& merged, std::uint8_t flags,
                                 std::uint8_t chance_none, const Entries& entries) {
    bool failed = false;
    wfb::LeveledListT out;
    out.id = merged.form.value;
    out.type = merged.type.value;
    out.flags = static_cast<wfb::LeveledListFlags>(flags);
    out.chance_none = chance_none;
    for (const auto& e : entries) {
        out.entries.emplace_back(e.level, e.count, shared_.global(merged, e.reference, failed));
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
    flatbuffers::FlatBufferBuilder& builder, const FormLists& form_lists) {
    std::vector<flatbuffers::Offset<wfb::Npc>> npcs;
    npcs.reserve(npcs_.size());
    for (auto& [id, n] : npcs_) {
        // DPLT names an FLST of packages.
        if (const auto list = form_lists.find(default_package_lists_[id]);
            list != form_lists.end()) {
            n.default_packages = list->second;
        }
        npcs.push_back(wfb::CreateNpc(builder, &n));
    }
    shared_.stats().npcs += npcs_.size();
    return npcs;
}

std::vector<flatbuffers::Offset<wfb::Race>> ActorCollector::write_races(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::Race>> races;
    races.reserve(races_.size());
    for (const auto& [id, r] : races_) {
        races.push_back(wfb::CreateRace(builder, &r));
    }
    shared_.stats().races += races_.size();
    return races;
}

std::vector<flatbuffers::Offset<wfb::Armor>> ActorCollector::write_armors(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::Armor>> armors;
    armors.reserve(armors_.size());
    for (const auto& [id, a] : armors_) {
        armors.push_back(wfb::CreateArmor(builder, &a));
    }
    shared_.stats().armors += armors_.size();
    return armors;
}

std::vector<flatbuffers::Offset<wfb::ArmorAddon>> ActorCollector::write_armor_addons(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::ArmorAddon>> addons;
    addons.reserve(armor_addons_.size());
    for (const auto& [id, a] : armor_addons_) {
        addons.push_back(wfb::CreateArmorAddon(builder, &a));
    }
    shared_.stats().armor_addons += armor_addons_.size();
    return addons;
}

std::vector<flatbuffers::Offset<wfb::Outfit>> ActorCollector::write_outfits(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::Outfit>> outfits;
    outfits.reserve(outfits_.size());
    for (const auto& [id, o] : outfits_) {
        outfits.push_back(wfb::CreateOutfit(builder, &o));
    }
    shared_.stats().outfits += outfits_.size();
    return outfits;
}

std::vector<flatbuffers::Offset<wfb::LeveledList>> ActorCollector::write_leveled_lists(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::LeveledList>> leveled;
    leveled.reserve(leveled_lists_.size());
    for (const auto& [id, l] : leveled_lists_) {
        leveled.push_back(wfb::CreateLeveledList(builder, &l));
    }
    shared_.stats().leveled_lists += leveled_lists_.size();
    return leveled;
}

} // namespace bethconv::pack::detail
