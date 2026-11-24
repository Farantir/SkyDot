// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's magic: the Godot side of the magic module (vm/magic_runtime.hpp): its creation,
// the effects' signals, the spell book, the save section and the natives' binding. The game layer's API is papyrus_casting.cpp,
// papyrus_shouts.cpp and papyrus_enchantment_effects.cpp.
#include "vm/papyrus_magic_state.hpp"

namespace skydot {

struct SkydotPapyrus::MagicHost final : vm::MagicRuntime::Host {
    explicit MagicHost(SkydotPapyrus& owner) : owner_(owner) {}
    vm::WorldAccess& world_access() override { return *owner_.world_host_; }
    vm::ActorStates& actor_states() override { return owner_.actors_; }
    void forget_ref(std::uint32_t ref) override { owner_.forget_ref(ref); }

    std::uint32_t dialogue_npc(std::uint32_t ref) const override { return owner_.dialogue_npc(ref); }
    std::uint32_t place_summon_ref(const vm::PlacedRef& placed) override { return owner_.place_ref(placed, false); }
    std::vector<std::uint32_t> triggered_actors() const override {
        std::vector<std::uint32_t> out;
        for (const auto& [actor, position] : owner_.triggers_.actors()) {
            out.push_back(actor);
        }
        return out;
    }
    void magic_trace(const std::string& line) override { owner_.trace_line(line); }
    void magic_notification(const std::string& text) override { owner_.show_message(text, false, false); }
    void effect_event(const magic::EffectView& v, bool started) override {
        owner_.emit_signal("magic_effect_changed", static_cast<std::int64_t>(v.target), static_cast<std::int64_t>(v.mgef),
                           static_cast<std::int64_t>(v.archetype), started, static_cast<std::int64_t>(v.id));
        owner_.emit_signal("magic_effect_event", effect_event_dictionary(v, started));
    }
    void attach_effect_scripts(std::uint32_t id, std::uint32_t mgef) override {
        const auto* root = owner_.world_root();
        const auto* record = root != nullptr ? lookup(root->magic_effects(), mgef) : nullptr;
        owner_.attach_scripts(id, script_list(record != nullptr ? record->scripts() : nullptr, false), 0);
    }

    vm::Vm& vm() override { return *owner_.vm_; }
    vm::Inventory& inventory() override { return owner_.vm_->inventory(); }
    vm::CreatedForms& created_forms() override { return owner_.created_; }
    std::unique_ptr<conditions::ConditionWorld> new_condition_world() override {
        return std::make_unique<PapyrusConditionWorld>(&owner_);
    }

    // The perks' entry points for spell cost, magnitude and duration.
    double perk_entry(std::uint8_t ep, std::uint32_t owner, std::array<std::uint32_t, 3> subjects,
                      double value) const override {
        if (ep >= perks::k_entry_point_count) {
            return value;
        }
        perks::EntryContext ctx;
        ctx.owner = owner;
        ctx.subjects = subjects;
        return owner_.apply_entry_point(static_cast<perks::EntryPoint>(ep), std::move(ctx), value);
    }

    // The abilities of an actor's base perks are part of its spell list.
    std::vector<std::uint32_t> perk_abilities(std::uint32_t ref) override {
        std::vector<std::uint32_t> out;
        const auto* root = owner_.world_root();
        if (root == nullptr) {
            return out;
        }
        for (const auto& [perk, rank] : owner_.progression_.base_perks(ref)) {
            const auto* p = lookup(root->perks(), perk);
            if (p == nullptr || p->effects() == nullptr || rank == 0) {
                continue;
            }
            for (const auto* e : *p->effects()) {
                if (e->kind() == bethconv::pack::wfb::PerkEffectKind::ability && e->spell() != 0 &&
                    static_cast<unsigned>(e->rank()) + 1U == rank) {
                    out.push_back(e->spell());
                }
            }
        }
        return out;
    }
    SkydotPapyrus& owner_;
};

void SkydotPapyrus::MagicHostDeleter::operator()(MagicHost* host) const { delete host; }

std::unique_ptr<SkydotPapyrus::MagicHost, SkydotPapyrus::MagicHostDeleter> SkydotPapyrus::new_magic_host() {
    return std::unique_ptr<MagicHost, MagicHostDeleter>(new MagicHost(*this));
}

std::unique_ptr<vm::MagicRuntime> SkydotPapyrus::new_magic(const bethconv::pack::wfb::World& root) {
    return std::make_unique<vm::MagicRuntime>(*magic_host_, root, item_, combat_);
}

vm::MagicRuntime* SkydotPapyrus::magic_state() {
    if (magic_) {
        return magic_.get();
    }
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (root == nullptr || !vm_) {
        return nullptr;
    }
    magic_ = new_magic(*root);
    return magic_.get();
}

namespace {

godot::Dictionary overrides_dictionary(const magic::Overrides& o) {
    Dictionary d;
    d["calm"] = o.calm;
    d["frenzy"] = o.frenzy;
    d["paralyzed"] = o.paralyzed;
    d["invisible"] = o.invisible;
    d["fear_from"] = static_cast<std::int64_t>(o.fear_from);
    d["commanded_by"] = static_cast<std::int64_t>(o.commanded_by);
    d["time_scale"] = o.time_scale;
    return d;
}

} // namespace

Dictionary SkydotPapyrus::effect_event_dictionary(const magic::EffectView& v, bool started) {
    Dictionary d;
    d["started"] = started;
    d["instance"] = static_cast<std::int64_t>(v.id);
    d["mgef"] = static_cast<std::int64_t>(v.mgef);
    d["archetype"] = static_cast<std::int64_t>(v.archetype);
    d["caster"] = static_cast<std::int64_t>(v.caster);
    d["target"] = static_cast<std::int64_t>(v.target);
    d["magnitude"] = v.magnitude;
    d["duration"] = v.duration;
    d["area"] = v.area;
    d["associated"] = static_cast<std::int64_t>(v.associated);
    d["summoned"] = static_cast<std::int64_t>(v.summoned);
    return d;
}

magic::Overrides SkydotPapyrus::magic_overrides_of(std::uint32_t ref) const {
    return magic_ ? magic_->effects.overrides(ref) : magic::Overrides{};
}

Dictionary SkydotPapyrus::magic_overrides(std::int64_t ref) const {
    return overrides_dictionary(magic_overrides_of(static_cast<std::uint32_t>(ref)));
}

std::int64_t SkydotPapyrus::break_invisibility(std::int64_t ref) {
    auto* m = magic_state();
    return m != nullptr ? m->break_invisibility(static_cast<std::uint32_t>(ref)) : 0;
}

bool SkydotPapyrus::dispel_bound_weapon(std::int64_t actor, std::int64_t weapon) {
    auto* m = magic_state();
    return m != nullptr && m->dispel_bound_weapon(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(weapon));
}

void SkydotPapyrus::flush_magic_events() {
    if (auto* m = magic_state()) {
        m->flush();
    }
}

void SkydotPapyrus::tick_magic(double seconds) {
    if (auto* m = magic_state()) {
        m->tick(seconds);
    }
}

magic::SpellBook* SkydotPapyrus::spell_book() {
    auto* m = magic_state();
    return m != nullptr ? &m->book : nullptr;
}

magic::ActorSoul SkydotPapyrus::soul_of(std::uint32_t actor) {
    auto* m = magic_state();
    const std::uint32_t level = m != nullptr ? m->actor_level(actor) : 0;
    const std::uint32_t npc = world_.is_valid() ? world_->data().keyword_by_editor_id("ActorTypeNPC") : 0;
    const bool is_npc = npc != 0 && PapyrusConditionWorld(this).has_keyword(actor, npc);
    return magic::actor_soul(std::max(level, 1U), is_npc);
}

void SkydotPapyrus::magic_actor_died(std::uint32_t ref) {
    if (!magic_) {
        return;
    }
    // Soul Trap cast by the player: the soul goes into a gem as the actor dies.
    if (magic_->soul_trapped_by_player(ref)) {
        const auto fill = fill_gem(ref);
        emit_signal("soul_trapped", static_cast<std::int64_t>(ref), fill["filled"], fill["gem"], fill["soul"]);
    }
    magic_->actor_died(ref);
}

void SkydotPapyrus::clear_magic() {
    if (magic_) {
        magic_->clear();
    }
}

std::vector<std::uint8_t> SkydotPapyrus::magic_section() const {
    return magic_ ? magic_->save() : std::vector<std::uint8_t>{};
}

bool SkydotPapyrus::magic_decode(save::ByteReader r, std::vector<std::uint8_t>& effects,
                                 std::vector<std::uint8_t>& book, std::vector<std::uint8_t>& shouts) {
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    return root != nullptr && vm::MagicRuntime::decode(*root, r, effects, book, shouts);
}

void SkydotPapyrus::magic_adopt(bool has, const std::vector<std::uint8_t>& effects,
                                const std::vector<std::uint8_t>& book, const std::vector<std::uint8_t>& shouts) {
    if (auto* m = magic_state()) {
        m->adopt(has, effects, book, shouts);
    }
}

void SkydotPapyrus::bind_magic_natives() {
    // The natives need the world; a VM without one has no spells to cast (setup binds again).
    if (auto* m = magic_state()) {
        m->bind_natives(*vm_);
    }
}

} // namespace skydot
