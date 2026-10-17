// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/pack/world.hpp"
#include "skydot_formats/vpath.hpp"

#include <CLI/CLI.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace bethconv::cli {
namespace {

struct FormArgs {
    std::filesystem::path pack;
    std::string gmst;
    std::vector<std::uint32_t> items;
    std::vector<std::uint32_t> races;
    std::vector<std::uint32_t> impact_data_sets;
    std::vector<std::uint32_t> impacts;
};

namespace wfb = bethconv::pack::wfb;

void print_setting(const wfb::GameSettingT& s) {
    switch (s.kind) {
    case wfb::GameSettingKind::floating:
        std::printf("%s = %g\n", s.editor_id.c_str(), static_cast<double>(s.float_value));
        break;
    case wfb::GameSettingKind::text:
        std::printf("%s = \"%s\"\n", s.editor_id.c_str(), s.text_value.c_str());
        break;
    default:
        std::printf("%s = %d\n", s.editor_id.c_str(), s.int_value);
        break;
    }
}

int cmd_form(const FormArgs& args) {
    auto world = bethconv::pack::WorldFile::open(args.pack / "world.fb");
    if (!world) {
        std::fprintf(stderr, "error: %s\n", world.error().to_string().c_str());
        return 1;
    }
    if (!args.gmst.empty()) {
        const std::string wanted = skydot::formats::ascii_lower(args.gmst);
        for (const auto& s : world->game_settings()) {
            if (wanted == "*" || skydot::formats::ascii_lower(s.editor_id).contains(wanted)) {
                print_setting(s);
            }
        }
    }
    for (const auto id : args.items) {
        const auto base = world->base(id);
        if (!base || !base->item) {
            std::printf("item 0x%X: none\n", id);
            continue;
        }
        const auto& b = *base;
        const auto& i = *b.item;
        std::printf("item 0x%X %s: type %u damage %g speed %g reach %g skill %d crit %d x%g "
                    "stagger %g ipds 0x%X fp '%s' equip 0x%X unequip 0x%X\n",
                    id, b.editor_id.c_str(), static_cast<unsigned>(i.weapon_type),
                    static_cast<double>(i.damage), static_cast<double>(i.speed),
                    static_cast<double>(i.reach), i.weapon_skill, i.crit_damage,
                    static_cast<double>(i.crit_mult), static_cast<double>(i.stagger),
                    i.impact_data_set, i.first_person_model.c_str(), i.equip_sound,
                    i.unequip_sound);
    }
    for (const auto id : args.races) {
        const auto race = world->race(id);
        if (!race) {
            std::printf("race 0x%X: none\n", id);
            continue;
        }
        std::printf("race 0x%X %s: unarmed %g reach %g material 0x%X ipds 0x%X, %zu attacks\n", id,
                    race->editor_id.c_str(), static_cast<double>(race->unarmed_damage),
                    static_cast<double>(race->unarmed_reach), race->impact_material,
                    race->impact_data_set, race->attacks.size());
        for (const auto& a : race->attacks) {
            std::printf("  %s: mult %g chance %g spell 0x%X flags 0x%X angle %g strike %g "
                        "stagger %g type 0x%X knockdown %g recovery %g stamina %g\n",
                        a->event.c_str(), static_cast<double>(a->damage_mult),
                        static_cast<double>(a->chance), a->spell, a->flags,
                        static_cast<double>(a->attack_angle), static_cast<double>(a->strike_angle),
                        static_cast<double>(a->stagger), a->attack_type,
                        static_cast<double>(a->knockdown), static_cast<double>(a->recovery_time),
                        static_cast<double>(a->stamina_mult));
        }
    }
    for (const auto id : args.impact_data_sets) {
        const auto set = world->impact_data_set(id);
        if (!set) {
            std::printf("ipds 0x%X: none\n", id);
            continue;
        }
        std::printf("ipds 0x%X %s: %zu entries\n", id, set->editor_id.c_str(), set->entries.size());
        for (const auto& e : set->entries) {
            std::printf("  material 0x%X -> impact 0x%X\n", e.material(), e.impact());
        }
    }
    for (const auto id : args.impacts) {
        const auto impact = world->impact(id);
        if (!impact) {
            std::printf("ipct 0x%X: none\n", id);
            continue;
        }
        std::printf("ipct 0x%X %s: model '%s' duration %g orientation %u flags 0x%X sound1 0x%X "
                    "sound2 0x%X\n",
                    id, impact->editor_id.c_str(), impact->model.c_str(),
                    static_cast<double>(impact->duration), impact->orientation,
                    static_cast<unsigned>(impact->flags), impact->sound1, impact->sound2);
    }
    return 0;
}

} // namespace

void register_form(CLI::App& app) {
    auto args = std::make_shared<FormArgs>();
    auto* form = app.add_subcommand("form", "Print GMSTs, items, races and impacts of a pack's world.fb");
    form->add_option("pack", args->pack, "The pack directory")
        ->required()
        ->check(CLI::ExistingDirectory);
    form->add_option("--gmst", args->gmst, "GMSTs whose editor id contains this (* for all)");
    form->add_option("--item", args->items, "An item base (WEAP, ...) by FormID");
    form->add_option("--race", args->races, "A race by FormID, with its attacks");
    form->add_option("--ipds", args->impact_data_sets, "An impact data set by FormID");
    form->add_option("--ipct", args->impacts, "An impact by FormID");
    form->callback([args] { set_exit_status(cmd_form(*args)); });
}

} // namespace bethconv::cli
