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
            entries.emplace_back(level, std::uint16_t{1}, form);
        }
        lists.push_back(wfb::CreateLeveledListDirect(fbb, l.id, l.type, l.flags, 0, some(entries)));
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

    const auto cells_v = fbb.CreateVector(cells);
    const auto worlds_v = fbb.CreateVector(worlds);
    const auto npcs_v = fbb.CreateVector(npcs);
    const auto races_v = fbb.CreateVector(races);
    const auto armors_v = fbb.CreateVector(armors);
    const auto addons_v = fbb.CreateVector(addons);
    const auto outfits_v = fbb.CreateVector(outfits);
    const auto lists_v = fbb.CreateVector(lists);
    const auto packages_v = fbb.CreateVector(packages);
    wfb::WorldBuilder world(fbb);
    world.add_format_version(static_cast<std::uint32_t>(spec.format_version));
    world.add_worlds(worlds_v);
    world.add_cells(cells_v);
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
