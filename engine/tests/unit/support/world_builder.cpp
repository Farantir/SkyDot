// SPDX-License-Identifier: GPL-3.0-or-later
#include "world_builder.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <stdexcept>

namespace skydot::testing {

namespace {

using Fbb = flatbuffers::FlatBufferBuilder;

template <typename T>
const std::vector<T>* some(const std::vector<T>& v) {
    return v.empty() ? nullptr : &v;
}

const char* some(const std::string& s) { return s.empty() ? nullptr : s.c_str(); }

std::vector<flatbuffers::Offset<wfb::Condition>> conditions(Fbb& fbb, const std::vector<ConditionSpec>& list) {
    std::vector<flatbuffers::Offset<wfb::Condition>> out;
    for (const auto& c : list) {
        out.push_back(wfb::CreateConditionDirect(fbb, c.type, c.function, c.value, c.value_global, c.param1, c.param2,
                                                 c.run_on, c.reference, c.param3, nullptr,
                                                 c.string2.empty() ? nullptr : c.string2.c_str()));
    }
    return out;
}

/// The tree in pre-order, each node followed by its subtrees.
void branches(Fbb& fbb, const BranchSpec& b, std::vector<flatbuffers::Offset<wfb::PackageBranch>>& out) {
    const auto cs = conditions(fbb, b.conditions);
    out.push_back(wfb::CreatePackageBranchDirect(fbb, b.type.c_str(), some(cs),
                                                 static_cast<std::uint32_t>(b.children.size()), b.flags,
                                                 some(b.procedure), false, some(b.inputs)));
    for (const auto& child : b.children) {
        branches(fbb, child, out);
    }
}

template <typename Spec>
std::vector<Spec> by_id(std::vector<Spec> list) {
    std::ranges::sort(list, {}, &Spec::id);
    return list;
}

flatbuffers::Offset<wfb::Package> package(Fbb& fbb, const PackageSpec& p) {
    const auto cs = conditions(fbb, p.conditions);
    std::vector<flatbuffers::Offset<wfb::PackageInput>> inputs;
    for (const auto& in : p.inputs) {
        inputs.push_back(wfb::CreatePackageInputDirect(fbb, in.key, in.type.c_str(), nullptr, in.number));
    }
    std::vector<flatbuffers::Offset<wfb::PackageBranch>> tree;
    if (p.tree) {
        branches(fbb, *p.tree, tree);
    }
    return wfb::CreatePackageDirect(fbb, p.id, some(p.editor_id), p.type, wfb::PackageFlags::NONE, 0, 0, 0,
                                    p.month, p.day_of_week, p.date, p.hour, p.minute, p.duration, some(cs),
                                    p.template_id, some(inputs), some(tree));
}

} // namespace

BuiltWorld::BuiltWorld(const WorldSpec& spec) {
    Fbb fbb;

    std::vector<flatbuffers::Offset<wfb::Cell>> cells;
    for (const auto& c : by_id(spec.cells)) {
        std::vector<wfb::Ref> refs;
        std::map<std::uint32_t, wfb::RefFlags> placed;
        for (const std::uint32_t id : c.refs) {
            placed[id] = wfb::RefFlags{};
        }
        for (const std::uint32_t id : c.disabled_refs) {
            placed[id] = wfb::RefFlags::initially_disabled;
        }
        for (const auto& [id, flags] : placed) {
            refs.emplace_back(id, std::uint32_t{0}, wfb::Vec3f(), wfb::Vec3f(), 1.0F, flags, std::uint32_t{0});
        }
        const auto refs_v = fbb.CreateVectorOfStructs(refs);
        wfb::CellBuilder cell(fbb);
        cell.add_id(c.id);
        cell.add_refs(refs_v);
        cells.push_back(cell.Finish());
    }
    std::vector<flatbuffers::Offset<wfb::Npc>> npcs;
    const auto entries_of = [](const std::vector<ItemEntry>& list) {
        std::vector<wfb::NpcItem> out;
        for (const auto& e : list) {
            out.emplace_back(e.form, e.count);
        }
        return out;
    };
    for (const auto& n : by_id(spec.npcs)) {
        const auto npc_items = entries_of(n.items);
        const auto items_v = npc_items.empty() ? 0 : fbb.CreateVectorOfStructs(npc_items);
        const auto face_v = n.face_model.empty() ? 0 : fbb.CreateString(n.face_model);
        const auto tone_v = n.skin_tone.empty() ? 0 : fbb.CreateVector(n.skin_tone);
        const auto packages_v = n.packages.empty() ? 0 : fbb.CreateVector(n.packages);
        const auto default_packages_v = n.default_packages.empty() ? 0 : fbb.CreateVector(n.default_packages);
        const auto spells_v = n.spells.empty() ? 0 : fbb.CreateVector(n.spells);
        wfb::NpcBuilder b(fbb);
        b.add_id(n.id);
        b.add_flags(n.flags);
        b.add_level(n.level);
        b.add_race(n.race);
        b.add_template_(n.template_id);
        b.add_template_flags(n.template_flags);
        b.add_skin(n.skin);
        b.add_default_outfit(n.default_outfit);
        b.add_height(n.height);
        b.add_weight(n.weight);
        b.add_items(items_v);
        b.add_face_model(face_v);
        b.add_skin_tone(tone_v);
        b.add_packages(packages_v);
        b.add_default_packages(default_packages_v);
        b.add_spells(spells_v);
        npcs.push_back(b.Finish());
    }
    std::vector<flatbuffers::Offset<wfb::Base>> bases;
    for (const auto& b : by_id(spec.bases)) {
        flatbuffers::Offset<wfb::ItemData> item = 0;
        if (b.item) {
            const auto& i = *b.item;
            std::vector<flatbuffers::Offset<wfb::EffectItem>> effects;
            for (const auto& e : i.effects) {
                effects.push_back(wfb::CreateEffectItem(fbb, e.effect, e.magnitude, 0U, e.duration));
            }
            flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<wfb::EffectItem>>> effects_v = 0;
            if (!effects.empty()) {
                effects_v = fbb.CreateVector(effects);
            }
            item = wfb::CreateItemData(fbb, i.category, i.weight, i.value, i.playable, i.equip_slot, i.weapon_type,
                                       i.damage, i.armor_rating, i.armor_type, i.biped_slots, i.food, effects_v,
                                       i.enchantment, i.enchantment_amount, 1.0F, 1.0F, -1, 0, 0.0F, 0.0F, 0U, 0, 0U, 0U,
                                       i.soul, i.soul_capacity, i.soul_gem_linked, i.holds_npc_souls);
        }
        flatbuffers::Offset<flatbuffers::Vector<std::uint32_t>> keywords = 0;
        if (!b.keywords.empty()) {
            keywords = fbb.CreateVector(b.keywords);
        }
        flatbuffers::Offset<wfb::Harvest> harvest = 0;
        if (b.harvest) {
            harvest = wfb::CreateHarvest(fbb, b.harvest->ingredient, b.harvest->sound, b.harvest->seasonal_yield);
        }
        const auto items = entries_of(b.items);
        flatbuffers::Offset<flatbuffers::Vector<const wfb::NpcItem*>> items_v = 0;
        if (!items.empty()) {
            items_v = fbb.CreateVectorOfStructs(items);
        }
        flatbuffers::Offset<flatbuffers::String> name = 0;
        if (!b.name.empty()) {
            name = fbb.CreateString(b.name);
        }
        wfb::BaseBuilder base(fbb);
        base.add_id(b.id);
        base.add_flags(b.flags);
        base.add_keywords(keywords);
        base.add_name(name);
        base.add_item(item);
        base.add_items(items_v);
        base.add_harvest(harvest);
        base.add_bench_type(b.bench_type);
        bases.push_back(base.Finish());
    }
    std::vector<flatbuffers::Offset<wfb::Race>> races;
    for (const auto& r : by_id(spec.races)) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> skeletons;
        for (const auto& s : r.skeletons) {
            skeletons.push_back(fbb.CreateString(s));
        }
        std::vector<flatbuffers::Offset<flatbuffers::String>> behaviours;
        for (const auto& b : r.behaviours) {
            behaviours.push_back(fbb.CreateString(b));
        }
        const auto spells_v = r.spells.empty() ? 0 : fbb.CreateVector(r.spells);
        const auto skeletons_v = skeletons.empty() ? 0 : fbb.CreateVector(skeletons);
        const auto behaviours_v = behaviours.empty() ? 0 : fbb.CreateVector(behaviours);
        const auto heights_v = r.heights.empty() ? 0 : fbb.CreateVector(r.heights);
        wfb::RaceBuilder b(fbb);
        b.add_id(r.id);
        b.add_skeletons(skeletons_v);
        b.add_behaviours(behaviours_v);
        b.add_skin(r.skin);
        b.add_heights(heights_v);
        b.add_armor_race(r.armor_race);
        b.add_spells(spells_v);
        races.push_back(b.Finish());
    }
    std::vector<flatbuffers::Offset<wfb::Armor>> armors;
    for (const auto& a : by_id(spec.armors)) {
        armors.push_back(wfb::CreateArmorDirect(fbb, a.id, nullptr, a.slots, 0, some(a.addons)));
    }
    std::vector<flatbuffers::Offset<wfb::ArmorAddon>> addons;
    for (const auto& a : by_id(spec.addons)) {
        addons.push_back(wfb::CreateArmorAddonDirect(fbb, a.id, nullptr, a.slots, a.race, some(a.additional_races),
                                                     some(a.male_model), some(a.female_model), 0, 0,
                                                     a.male_weight_slider, a.female_weight_slider));
    }
    std::vector<flatbuffers::Offset<wfb::Outfit>> outfits;
    for (const auto& o : by_id(spec.outfits)) {
        outfits.push_back(wfb::CreateOutfitDirect(fbb, o.id, some(o.items)));
    }
    std::vector<flatbuffers::Offset<wfb::LeveledList>> lists;
    for (const auto& l : by_id(spec.leveled_lists)) {
        std::vector<wfb::LeveledEntry> entries;
        for (const auto& e : l.entries) {
            entries.emplace_back(e.level, e.count, e.form);
        }
        lists.push_back(wfb::CreateLeveledListDirect(fbb, l.id, l.type, l.flags, l.chance_none, some(entries),
                                                     l.chance_global));
    }
    std::vector<flatbuffers::Offset<wfb::Recipe>> recipes;
    for (const auto& r : by_id(spec.recipes)) {
        const auto components = entries_of(r.components);
        flatbuffers::Offset<flatbuffers::Vector<const wfb::NpcItem*>> components_v = 0;
        if (!components.empty()) {
            components_v = fbb.CreateVectorOfStructs(components);
        }
        const auto list = conditions(fbb, r.conditions);
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>> list_v = 0;
        if (!list.empty()) {
            list_v = fbb.CreateVector(list);
        }
        recipes.push_back(
            wfb::CreateRecipe(fbb, r.id, 0, r.product, r.product_count, r.bench_keyword, components_v, list_v));
    }
    std::vector<flatbuffers::Offset<wfb::MagicEffect>> effects;
    for (const auto& e : by_id(spec.magic_effects)) {
        const auto mgef_conditions = conditions(fbb, e.conditions);
        const auto name = fbb.CreateString(e.name);
        const auto description = fbb.CreateString(e.description);
        const auto editor_id = fbb.CreateString(e.editor_id);
        const auto keywords = fbb.CreateVector(e.keywords);
        const auto cond_v = fbb.CreateVector(mgef_conditions);
        wfb::MagicEffectBuilder m(fbb);
        m.add_id(e.id);
        m.add_name(name);
        m.add_description(description);
        m.add_flags(e.flags);
        m.add_base_cost(e.base_cost);
        m.add_archetype(e.archetype);
        m.add_actor_value(e.actor_value);
        m.add_second_actor_value(e.second_actor_value);
        m.add_second_av_weight(e.second_av_weight);
        m.add_resist_value(e.resist_value);
        m.add_magic_skill(e.magic_skill);
        m.add_casting_type(e.casting_type);
        m.add_delivery(e.delivery);
        m.add_keywords(keywords);
        m.add_conditions(cond_v);
        m.add_projectile(e.projectile);
        m.add_explosion(e.explosion);
        m.add_casting_art(e.casting_art);
        m.add_hit_effect_art(e.hit_effect_art);
        m.add_casting_light(e.casting_light);
        m.add_associated_item(e.associated_item);
        m.add_editor_id(editor_id);
        effects.push_back(m.Finish());
    }
    std::vector<flatbuffers::Offset<wfb::Spell>> spells;
    for (const auto& sp : by_id(spec.spells)) {
        std::vector<flatbuffers::Offset<wfb::EffectItem>> items;
        for (const auto& e : sp.effects) {
            const auto cs = conditions(fbb, e.conditions);
            items.push_back(wfb::CreateEffectItem(fbb, e.effect, e.magnitude, e.area, e.duration,
                                                  cs.empty() ? 0 : fbb.CreateVector(cs)));
        }
        const auto name = fbb.CreateString(sp.name);
        const auto items_v = fbb.CreateVector(items);
        wfb::SpellBuilder b(fbb);
        b.add_id(sp.id);
        b.add_name(name);
        b.add_cost(sp.cost);
        b.add_flags(sp.flags);
        b.add_type(sp.type);
        b.add_casting_type(sp.casting_type);
        b.add_delivery(sp.delivery);
        b.add_charge_time(sp.charge_time);
        b.add_cast_duration(sp.cast_duration);
        b.add_range(sp.range);
        b.add_effects(items_v);
        spells.push_back(b.Finish());
    }
    std::vector<flatbuffers::Offset<wfb::Enchantment>> enchantments;
    for (const auto& en : by_id(spec.enchantments)) {
        std::vector<flatbuffers::Offset<wfb::EffectItem>> items;
        for (const auto& e : en.effects) {
            items.push_back(wfb::CreateEffectItem(fbb, e.effect, e.magnitude, e.area, e.duration));
        }
        const auto name = fbb.CreateString(en.name);
        const auto items_v = fbb.CreateVector(items);
        wfb::EnchantmentBuilder b(fbb);
        b.add_id(en.id);
        b.add_name(name);
        b.add_cost(en.cost);
        b.add_charge(en.charge);
        b.add_effects(items_v);
        b.add_flags(en.flags);
        b.add_casting_type(en.casting_type);
        b.add_enchantment_type(en.enchantment_type);
        b.add_base_enchantment(en.base_enchantment);
        b.add_worn_restrictions(en.worn_restrictions);
        enchantments.push_back(b.Finish());
    }
    std::vector<flatbuffers::Offset<wfb::FormList>> form_lists;
    for (const auto& l : by_id(spec.form_lists)) {
        const auto forms = fbb.CreateVector(l.forms);
        wfb::FormListBuilder b(fbb);
        b.add_id(l.id);
        b.add_forms(forms);
        form_lists.push_back(b.Finish());
    }
    std::vector<flatbuffers::Offset<wfb::Keyword>> keywords_table;
    for (const auto& k : by_id(spec.keywords)) {
        const auto edid = fbb.CreateString(k.editor_id);
        keywords_table.push_back(wfb::CreateKeyword(fbb, k.id, edid));
    }
    std::vector<flatbuffers::Offset<wfb::Package>> packages;
    for (const auto& p : by_id(spec.packages)) {
        packages.push_back(package(fbb, p));
    }

    std::vector<flatbuffers::Offset<wfb::Worldspace>> worlds;
    for (const auto& w : by_id(spec.worlds)) {
        std::vector<wfb::LargeRef> refs;
        std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::uint32_t>> per_square; // by (y, x)
        for (const auto& r : by_id(w.large_refs)) {
            refs.emplace_back(r.id, r.base, wfb::Vec3f(), wfb::Vec3f(), 1.0F, r.flags, r.enable_parent, r.x, r.y);
        }
        for (std::uint32_t index = 0; index < refs.size(); ++index) {
            const auto& r = refs[index];
            std::set<std::pair<std::int16_t, std::int16_t>> squares{{r.cell_y(), r.cell_x()}};
            for (const auto& spec_ref : w.large_refs) {
                if (spec_ref.id == r.id()) {
                    for (const auto& [x, y] : spec_ref.reaches) {
                        squares.emplace(y, x);
                    }
                }
            }
            for (const auto& square : squares) {
                per_square[square].push_back(index);
            }
        }
        std::vector<wfb::LargeRefCell> large_cells;
        std::vector<std::uint32_t> large_cell_refs;
        for (const auto& [square, list] : per_square) {
            large_cells.emplace_back(square.second, square.first, static_cast<std::uint32_t>(large_cell_refs.size()),
                                     static_cast<std::uint32_t>(list.size()));
            large_cell_refs.insert(large_cell_refs.end(), list.begin(), list.end());
        }
        worlds.push_back(wfb::CreateWorldspaceDirect(fbb, w.id, nullptr, 0, wfb::ParentFlags{}, 0, false, 0.0F, 0.0F, 0,
                                                     0, 0.0F, 0.0F, 0.0F, 0.0F, refs.empty() ? nullptr : &refs, some(large_cells),
                                                     some(large_cell_refs)));
    }

    const auto bases_v = fbb.CreateVector(bases);
    const auto cells_v = fbb.CreateVector(cells);
    const auto worlds_v = fbb.CreateVector(worlds);
    const auto npcs_v = fbb.CreateVector(npcs);
    const auto races_v = fbb.CreateVector(races);
    const auto armors_v = fbb.CreateVector(armors);
    const auto addons_v = fbb.CreateVector(addons);
    const auto outfits_v = fbb.CreateVector(outfits);
    const auto lists_v = fbb.CreateVector(lists);
    const auto packages_v = fbb.CreateVector(packages);
    const auto recipes_v = fbb.CreateVector(recipes);
    const auto effects_v = fbb.CreateVector(effects);
    const auto spells_v = fbb.CreateVector(spells);
    const auto enchantments_v = fbb.CreateVector(enchantments);
    const auto form_lists_v = fbb.CreateVector(form_lists);
    const auto keywords_v = fbb.CreateVector(keywords_table);
    wfb::WorldBuilder world(fbb);
    world.add_format_version(static_cast<std::uint32_t>(spec.format_version));
    world.add_worlds(worlds_v);
    world.add_cells(cells_v);
    world.add_bases(bases_v);
    world.add_npcs(npcs_v);
    world.add_races(races_v);
    world.add_armors(armors_v);
    world.add_armor_addons(addons_v);
    world.add_outfits(outfits_v);
    world.add_leveled_lists(lists_v);
    world.add_packages(packages_v);
    world.add_recipes(recipes_v);
    world.add_magic_effects(effects_v);
    world.add_spells(spells_v);
    world.add_enchantments(enchantments_v);
    world.add_form_lists(form_lists_v);
    world.add_keywords(keywords_v);
    wfb::FinishWorldBuffer(fbb, world.Finish());

    bytes_.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    flatbuffers::Verifier verifier(bytes_.data(), bytes_.size());
    if (!wfb::VerifyWorldBuffer(verifier)) {
        throw std::logic_error("test world.fb does not verify");
    }
    world_ = wfb::GetWorld(bytes_.data());
}

} // namespace skydot::testing
