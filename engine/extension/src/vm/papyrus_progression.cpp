// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's progression forwarders for the GDScript rules (vm/progression_runtime.hpp): the
// player's XP, level, perk points and books, and the perks of actors.
#include "conditions/conditions.hpp"
#include "perks/entry_points.hpp"
#include "vm/papyrus.hpp"

using godot::Array;
using godot::Dictionary;

namespace skydot {
namespace {

constexpr std::int64_t k_first_skill = 6;

/// A skill's index (actor value - 6), or the count when `av` is no skill.
std::uint32_t skill_index(std::int64_t av) {
    const std::int64_t i = av - k_first_skill;
    return i >= 0 && i < vm::k_progression_skills ? static_cast<std::uint32_t>(i) : vm::k_progression_skills;
}

std::vector<std::uint32_t> ids_of(const godot::Variant& v) {
    std::vector<std::uint32_t> out;
    if (v.get_type() == godot::Variant::ARRAY) {
        for (const godot::Variant& item : static_cast<Array>(v)) {
            out.push_back(static_cast<std::uint32_t>(static_cast<std::int64_t>(item)));
        }
    }
    return out;
}

perks::EntryContext entry_context(std::int64_t owner, const Array& subjects, const Dictionary& extra) {
    perks::EntryContext ctx;
    ctx.owner = static_cast<std::uint32_t>(owner);
    for (int i = 0; i < 3 && i < subjects.size(); ++i) {
        ctx.subjects[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(static_cast<std::int64_t>(subjects[i]));
    }
    ctx.advance_skill = static_cast<std::int32_t>(static_cast<std::int64_t>(extra.get("advance_skill", -1)));
    ctx.advance_action = static_cast<std::int32_t>(static_cast<std::int64_t>(extra.get("advance_action", -1)));
    ctx.advance_object = static_cast<std::uint32_t>(static_cast<std::int64_t>(extra.get("advance_object", 0)));
    ctx.worn = ids_of(extra.get("worn", Array()));
    ctx.attack_keywords = ids_of(extra.get("attack_keywords", Array()));
    ctx.item_enchanted = static_cast<bool>(extra.get("item_enchanted", false));
    ctx.making_poison = static_cast<bool>(extra.get("making_poison", false));
    ctx.alchemy_effect = static_cast<std::uint32_t>(static_cast<std::int64_t>(extra.get("alchemy_effect", 0)));
    return ctx;
}

} // namespace

double SkydotPapyrus::perk_entry(std::int64_t entry_point, std::int64_t owner, const Array& subjects, double value,
                                 const Dictionary& extra) {
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (root == nullptr || entry_point < 0 || entry_point >= static_cast<std::int64_t>(perks::k_entry_point_count)) {
        return value;
    }
    const perks::EntryPoints points(*root, condition_world(),
                                    [this](std::uint32_t ref) { return progression_.perks_of(ref); });
    return points.apply(static_cast<perks::EntryPoint>(entry_point), entry_context(owner, subjects, extra), value);
}

godot::PackedInt64Array SkydotPapyrus::perk_entry_spells(std::int64_t entry_point, std::int64_t owner,
                                                         const Array& subjects, const Dictionary& extra) {
    godot::PackedInt64Array out;
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (root == nullptr || entry_point < 0 || entry_point >= static_cast<std::int64_t>(perks::k_entry_point_count)) {
        return out;
    }
    const perks::EntryPoints points(*root, condition_world(),
                                    [this](std::uint32_t ref) { return progression_.perks_of(ref); });
    for (const auto spell : points.select_spells(static_cast<perks::EntryPoint>(entry_point),
                                                 entry_context(owner, subjects, extra))) {
        out.push_back(spell);
    }
    return out;
}

godot::PackedStringArray SkydotPapyrus::perk_entry_texts(std::int64_t entry_point, std::int64_t owner,
                                                         const Array& subjects, const Dictionary& extra) {
    godot::PackedStringArray out;
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (root == nullptr || entry_point < 0 || entry_point >= static_cast<std::int64_t>(perks::k_entry_point_count)) {
        return out;
    }
    const perks::EntryPoints points(*root, condition_world(),
                                    [this](std::uint32_t ref) { return progression_.perks_of(ref); });
    for (const auto& text : points.select_text(static_cast<perks::EntryPoint>(entry_point),
                                               entry_context(owner, subjects, extra))) {
        out.push_back(godot::String::utf8(text.c_str()));
    }
    return out;
}

Dictionary SkydotPapyrus::get_player_progress() const {
    const vm::PlayerProgress& p = progression_.player();
    Dictionary out;
    Array skill_xp;
    Array legendary;
    for (std::uint32_t i = 0; i < vm::k_progression_skills; ++i) {
        skill_xp.push_back(p.skill_xp[i]);
        legendary.push_back(static_cast<std::int64_t>(p.legendary[i]));
    }
    out["skill_xp"] = skill_xp;
    out["xp"] = p.xp;
    out["level"] = static_cast<std::int64_t>(p.level);
    out["perk_points"] = static_cast<std::int64_t>(p.perk_points);
    out["trainings"] = static_cast<std::int64_t>(p.trainings);
    out["legendary"] = legendary;
    godot::PackedInt64Array books;
    for (const auto book : p.books_read) {
        books.push_back(static_cast<std::int64_t>(book));
    }
    out["books_read"] = books;
    return out;
}

void SkydotPapyrus::set_skill_xp(std::int64_t skill_av, double xp) {
    progression_.set_skill_xp(skill_index(skill_av), xp);
}
void SkydotPapyrus::set_player_xp(double xp) { progression_.set_xp(xp); }
void SkydotPapyrus::set_player_level(std::int64_t level) {
    progression_.set_level(static_cast<std::uint16_t>(std::clamp<std::int64_t>(level, 1, 0xFFFF)));
}
void SkydotPapyrus::set_perk_points(std::int64_t points) {
    progression_.set_perk_points(static_cast<std::int32_t>(std::clamp<std::int64_t>(points, -0x7FFFFFFF, 0x7FFFFFFF)));
}
void SkydotPapyrus::set_trainings(std::int64_t n) {
    progression_.set_trainings(static_cast<std::uint32_t>(std::clamp<std::int64_t>(n, 0, 0x7FFFFFFF)));
}
void SkydotPapyrus::set_legendary(std::int64_t skill_av, std::int64_t n) {
    progression_.set_legendary(skill_index(skill_av), static_cast<std::uint16_t>(std::clamp<std::int64_t>(n, 0, 0xFFFF)));
}
void SkydotPapyrus::mark_book_read(std::int64_t book) { progression_.mark_book_read(static_cast<std::uint32_t>(book)); }
bool SkydotPapyrus::is_book_read(std::int64_t book) const {
    return progression_.is_book_read(static_cast<std::uint32_t>(book));
}

void SkydotPapyrus::add_perk(std::int64_t ref, std::int64_t perk) {
    const auto r = static_cast<std::uint32_t>(ref);
    const auto p = static_cast<std::uint32_t>(perk);
    // Adding an owned perk again keeps its rank; a new one starts at 1.
    if (progression_.perk_rank(r, p) == 0) {
        progression_.add_perk(r, p, 1);
    }
}
void SkydotPapyrus::remove_perk(std::int64_t ref, std::int64_t perk) {
    progression_.remove_perk(static_cast<std::uint32_t>(ref), static_cast<std::uint32_t>(perk));
}
std::int64_t SkydotPapyrus::perk_rank(std::int64_t ref, std::int64_t perk) const {
    return progression_.perk_rank(static_cast<std::uint32_t>(ref), static_cast<std::uint32_t>(perk));
}
Array SkydotPapyrus::get_perks(std::int64_t ref) const {
    Array out;
    for (const auto& [perk, rank] : progression_.perks_of(static_cast<std::uint32_t>(ref))) {
        Dictionary d;
        d["perk"] = static_cast<std::int64_t>(perk);
        d["rank"] = static_cast<std::int64_t>(rank);
        out.push_back(d);
    }
    return out;
}

bool SkydotPapyrus::perk_conditions_met(std::int64_t perk, std::int64_t ref) {
    const auto* p = world_.is_valid() ? world_->data().perk_ptr(static_cast<std::uint32_t>(perk)) : nullptr;
    if (p == nullptr) {
        return true;
    }
    const conditions::ConditionContext ctx{.subject = static_cast<std::uint32_t>(ref), .target = 0, .quest = 0};
    return conditions::conditions_pass(p->conditions(), condition_world(), ctx);
}

} // namespace skydot
