// SPDX-License-Identifier: GPL-3.0-or-later
#include "world_builder.hpp"

#include <algorithm>
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
                                                 c.run_on, c.reference));
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
    return wfb::CreatePackageDirect(fbb, p.id, some(p.editor_id), p.type, 0, 0, 0, 0, p.month, p.day_of_week, p.date,
                                    p.hour, p.minute, p.duration, some(cs), p.template_id, some(inputs), some(tree));
}

} // namespace

BuiltWorld::BuiltWorld(const WorldSpec& spec) {
    Fbb fbb;

    std::vector<flatbuffers::Offset<wfb::Npc>> npcs;
    for (const auto& n : by_id(spec.npcs)) {
        npcs.push_back(wfb::CreateNpcDirect(fbb, n.id, nullptr, nullptr, n.flags, n.level, n.race, n.template_id,
                                            n.template_flags, n.skin, n.default_outfit, 0, n.height, n.weight, nullptr,
                                            nullptr, some(n.face_model), some(n.skin_tone), some(n.packages),
                                            some(n.default_packages)));
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
        races.push_back(wfb::CreateRaceDirect(fbb, r.id, nullptr, some(skeletons), some(behaviours), r.skin,
                                              some(r.heights), nullptr, 0, nullptr, nullptr, nullptr, r.armor_race));
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
        for (const auto& [level, form] : l.entries) {
            entries.emplace_back(level, 1, form);
        }
        lists.push_back(wfb::CreateLeveledListDirect(fbb, l.id, l.type, l.flags, 0, some(entries)));
    }
    std::vector<flatbuffers::Offset<wfb::Package>> packages;
    for (const auto& p : by_id(spec.packages)) {
        packages.push_back(package(fbb, p));
    }

    const auto npcs_v = fbb.CreateVector(npcs);
    const auto races_v = fbb.CreateVector(races);
    const auto armors_v = fbb.CreateVector(armors);
    const auto addons_v = fbb.CreateVector(addons);
    const auto outfits_v = fbb.CreateVector(outfits);
    const auto lists_v = fbb.CreateVector(lists);
    const auto packages_v = fbb.CreateVector(packages);
    wfb::WorldBuilder world(fbb);
    world.add_format_version(10);
    world.add_npcs(npcs_v);
    world.add_races(races_v);
    world.add_armors(armors_v);
    world.add_armor_addons(addons_v);
    world.add_outfits(outfits_v);
    world.add_leveled_lists(lists_v);
    world.add_packages(packages_v);
    wfb::FinishWorldBuffer(fbb, world.Finish());

    bytes_.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    flatbuffers::Verifier verifier(bytes_.data(), bytes_.size());
    if (!wfb::VerifyWorldBuffer(verifier)) {
        throw std::logic_error("test world.fb does not verify");
    }
    world_ = wfb::GetWorld(bytes_.data());
}

} // namespace skydot::testing
