// SPDX-License-Identifier: GPL-3.0-or-later
#include "magic/magic_save.hpp"

#include <string>

namespace skydot::magic {

std::vector<std::uint8_t> write_magc(const ActiveEffects& effects, const SpellBook& book, const ShoutBook& shouts) {
    save::ByteWriter w;
    w.u32(k_magc_version);
    const auto e = effects.save();
    const auto b = book.save();
    const auto h = shouts.save();
    w.text(std::string_view(reinterpret_cast<const char*>(e.data()), e.size()));
    w.text(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
    w.text(std::string_view(reinterpret_cast<const char*>(h.data()), h.size()));
    return w.out;
}

bool read_magc(save::ByteReader r, MagcParts& out) {
    std::uint32_t version = 0;
    std::string e, b, h;
    if (!r.u32(version) || version < k_magc_min_version || version > k_magc_version || !r.text(e) || !r.text(b) ||
        (version >= 2 && !r.text(h)) || !r.at_end()) {
        return false;
    }
    out.effects.assign(e.begin(), e.end());
    out.book.assign(b.begin(), b.end());
    out.shouts.assign(h.begin(), h.end());
    return true;
}

bool check_magc(const wfb::World& world, const MagcParts& parts) {
    if (!ActiveEffects::check(world, save::ByteReader(parts.effects))) {
        return false;
    }
    struct Silent final : MagicHost {
        double actor_value(std::uint32_t, std::uint32_t) const override { return 0.0; }
        void damage_value(std::uint32_t, std::uint32_t, double, std::uint32_t) override {}
        void restore_value(std::uint32_t, std::uint32_t, double) override {}
        void modify_temp(std::uint32_t, std::uint32_t, double) override {}
        bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>*, std::uint32_t,
                             std::uint32_t) override {
            return true;
        }
        bool is_dead(std::uint32_t) const override { return false; }
        void log(const std::string&) override {}
    } host;
    ActiveEffects scratch(world, host);
    SpellBook book(world, scratch);
    if (!SpellBook::load(save::ByteReader(parts.book), book)) {
        return false;
    }
    ShoutBook shouts(world);
    return parts.shouts.empty() || ShoutBook::load(save::ByteReader(parts.shouts), shouts);
}

} // namespace skydot::magic
