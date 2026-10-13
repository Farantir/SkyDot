// SPDX-License-Identifier: GPL-3.0-or-later
//
// `Vm::census`: which natives a set of scripts call, found by reading their
// code instead of running it. The parents of the named classes are read too,
// each class once.
#include "vm/vm.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace skydot::vm {

namespace {

/// The declared type of argument `arg` of a function, as `Vm::type_of` reads
/// it for a running frame; empty if it has none.
std::string_view declared_type(const Function& fn, std::uint32_t arg) {
    const Slot& slot = fn.slots[arg];
    switch (slot.type) {
    case Slot::Type::reg: return fn.reg_types[slot.index];
    case Slot::Type::var: return fn.owner->variables()[slot.index].type;
    case Slot::Type::self: return fn.owner->key();
    case Slot::Type::name:
        for (const ScriptClass* c = fn.owner->parent(); c != nullptr; c = c->parent()) {
            for (const Variable& v : c->variables()) {
                if (v.name == fn.owner->lower(slot.index)) {
                    return v.type;
                }
            }
        }
        return {};
    default: return {};
    }
}

} // namespace

std::vector<Vm::CensusEntry> Vm::census(const std::vector<std::string>& classes) {
    std::map<const Function*, std::size_t> sites;
    const auto scan = [&](const Function& fn) {
        for (std::size_t pc = 0; pc < fn.ops.size(); ++pc) {
            const Op op = fn.ops[pc];
            if (op != Op::callmethod && op != Op::callparent && op != Op::callstatic) {
                continue;
            }
            const std::uint32_t a = fn.offsets[pc];
            const auto name_at = [&](std::uint32_t arg) {
                const auto type = fn.args[arg].type();
                return type == 1 || type == 2 ? fn.owner->lower_name(fn.args[arg].data()) : Name("");
            };
            const ScriptClass* start = nullptr;
            if (op == Op::callstatic) {
                start = class_named(name_at(a).text);
            } else if (op == Op::callparent) {
                start = fn.owner->parent();
            } else {
                const std::string_view type = declared_type(fn, a + 1);
                start = type.empty() || type.ends_with("[]") || type_kind(type) != TypeKind::object
                            ? nullptr
                            : class_named(type);
            }
            if (start == nullptr) {
                continue;
            }
            const Name name = op == Op::callstatic ? name_at(a + 1) : name_at(a);
            const Function* target = find_in_chain(start, "", name);
            if (target != nullptr && target->native) {
                ++sites[target];
            }
        }
    };
    std::set<const ScriptClass*> seen;
    for (const std::string& name : classes) {
        for (const ScriptClass* cls = class_named(name); cls != nullptr && seen.insert(cls).second;
             cls = cls->parent()) {
            for (const auto& fn : cls->functions()) {
                if (!fn->native) {
                    scan(*fn);
                }
            }
            // Hand-written property accessors are not scanned.
        }
    }
    std::vector<CensusEntry> out;
    for (const auto& [fn, count] : sites) {
        out.push_back({.cls = fn->owner->key(),
                       .fn = fn->name,
                       .sites = count,
                       .bound = is_bound(fn),
                       .ignored = is_ignored(fn)});
    }
    std::ranges::sort(out, [](const CensusEntry& x, const CensusEntry& y) {
        return std::tie(x.cls, x.fn) < std::tie(y.cls, y.fn);
    });
    return out;
}

} // namespace skydot::vm
