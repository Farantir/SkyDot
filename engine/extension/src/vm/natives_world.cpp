// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/natives_world.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string_view>

namespace skydot::vm {

std::uint32_t nth_linked_ref(const WorldAccess& world, std::uint32_t ref, std::int32_t n) {
    if (n < 0) {
        return 0;
    }
    // A chain that loops back never ends; the steps are bounded by `n`.
    for (std::int32_t i = 0; i < n && ref != 0; ++i) {
        ref = world.linked_ref(ref, 0);
    }
    return ref;
}

std::string format_message(const std::string& text, const std::vector<double>& args) {
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }
        // %[width][.precision]conversion; flags are skipped.
        std::size_t end = i + 1;
        while (end < text.size() && std::string_view("-+ #0").find(text[end]) != std::string_view::npos) {
            ++end;
        }
        const auto number = [&text, &end](int fallback) {
            if (end >= text.size() || std::isdigit(static_cast<unsigned char>(text[end])) == 0) {
                return fallback;
            }
            int n = 0;
            for (; end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0; ++end) {
                n = std::min(n * 10 + (text[end] - '0'), 99);
            }
            return n;
        };
        const int width = number(0);
        int precision = 6;
        if (end < text.size() && text[end] == '.') {
            ++end;
            precision = number(0);
        }
        const char conversion = end < text.size() ? text[end] : '\0';
        if (conversion != 'f' && conversion != 'd' && conversion != 'i') {
            out += text[i];
            continue;
        }
        const double value = next < args.size() ? args[next] : 0.0;
        ++next;
        char buffer[160];
        if (conversion == 'f') {
            std::snprintf(buffer, sizeof buffer, "%*.*f", width, precision, value);
        } else {
            std::snprintf(buffer, sizeof buffer, "%*lld", width, static_cast<long long>(value));
        }
        out += buffer;
        i = end;
    }
    return out;
}

std::vector<std::uint32_t> form_list_contents(const Vm& vm, const WorldAccess& world, std::uint32_t list) {
    std::vector<std::uint32_t> forms;
    (void)world.form_list(list, forms);
    const auto& added = vm.form_lists().added(list);
    forms.insert(forms.end(), added.begin(), added.end());
    return forms;
}

void bind_world_natives(Vm& vm, const WorldAccess& world) {
    // ObjectReference.psc: the nth linked ref in the chain of this reference's
    // links without a keyword (DartTrap picks a port with it). 0 is not
    // documented; it gives the reference itself (assumed, not checked).
    vm.bind("ObjectReference", "GetNthLinkedRef", [&world](NativeCall& c) {
        const std::int32_t n = c.args.empty() ? 0 : c.args[0].i();
        const std::uint32_t target = nth_linked_ref(world, c.self.form(), n);
        return NativeResult{.value = target != 0 ? c.vm.object(target, "ObjectReference") : Value{}, .wait = -1.0};
    });
    // The cell is a form of its own (class Cell, the cell record's id).
    vm.bind("ObjectReference", "GetParentCell", [&world](NativeCall& c) {
        const std::uint32_t cell = world.parent_cell(c.self.form());
        return NativeResult{.value = cell != 0 ? c.vm.object(cell, "Cell") : Value{}, .wait = -1.0};
    });
    vm.bind("Cell", "IsInterior", [&world](NativeCall& c) {
        return NativeResult{.value = Value::boolean(world.cell_interior(c.self.form())), .wait = -1.0};
    });
    vm.bind("Cell", "IsAttached", [&world](NativeCall& c) {
        return NativeResult{.value = Value::boolean(world.cell_attached(c.self.form())), .wait = -1.0};
    });

    // ---- keywords
    vm.bind("Keyword", "GetKeyword", [&world](NativeCall& c) {
        const std::uint32_t keyword = c.args.empty() ? 0 : world.find_keyword(Vm::to_string(c.args[0]));
        return NativeResult{.value = keyword != 0 ? c.vm.object(keyword, "Keyword") : Value{}, .wait = -1.0};
    });
    vm.bind("Form", "HasKeyword", [&world](NativeCall& c) {
        const bool has = !c.args.empty() && c.args[0].kind() == Kind::object &&
                         world.has_keyword(c.self.form(), c.args[0].form());
        return NativeResult{.value = Value::boolean(has), .wait = -1.0};
    });

    // ---- form lists
    vm.bind("FormList", "GetSize", [&world](NativeCall& c) {
        return NativeResult{.value = Value::integer(static_cast<std::int32_t>(
                                form_list_contents(c.vm, world, c.self.form()).size())),
                            .wait = -1.0};
    });
    vm.bind("FormList", "GetAt", [&world](NativeCall& c) {
        const auto forms = form_list_contents(c.vm, world, c.self.form());
        const std::int32_t at = c.args.empty() ? -1 : c.args[0].i();
        if (at < 0 || static_cast<std::size_t>(at) >= forms.size()) {
            return NativeResult{};
        }
        return NativeResult{.value = c.vm.object(forms[static_cast<std::size_t>(at)], "Form"), .wait = -1.0};
    });
    // The index of the form, -1 if it is not in the list.
    vm.bind("FormList", "Find", [&world](NativeCall& c) {
        const auto forms = form_list_contents(c.vm, world, c.self.form());
        std::int32_t found = -1;
        if (!c.args.empty() && c.args[0].kind() == Kind::object) {
            const auto it = std::ranges::find(forms, c.args[0].form());
            found = it != forms.end() ? static_cast<std::int32_t>(it - forms.begin()) : -1;
        }
        return NativeResult{.value = Value::integer(found), .wait = -1.0};
    });
    vm.bind("FormList", "HasForm", [&world](NativeCall& c) {
        const auto forms = form_list_contents(c.vm, world, c.self.form());
        const bool has = !c.args.empty() && c.args[0].kind() == Kind::object &&
                         std::ranges::find(forms, c.args[0].form()) != forms.end();
        return NativeResult{.value = Value::boolean(has), .wait = -1.0};
    });
    // A form the list holds already is not added twice.
    vm.bind("FormList", "AddForm", [&world](NativeCall& c) {
        if (!c.args.empty() && c.args[0].kind() == Kind::object) {
            const auto forms = form_list_contents(c.vm, world, c.self.form());
            if (std::ranges::find(forms, c.args[0].form()) == forms.end()) {
                c.vm.form_lists().add(c.self.form(), c.args[0].form());
            }
        }
        return NativeResult{};
    });
    // Only forms scripts added can be taken out; the world's stay.
    vm.bind("FormList", "RemoveAddedForm", [](NativeCall& c) {
        if (!c.args.empty() && c.args[0].kind() == Kind::object) {
            c.vm.form_lists().remove(c.self.form(), c.args[0].form());
        }
        return NativeResult{};
    });
    vm.bind("FormList", "Revert", [](NativeCall& c) {
        c.vm.form_lists().revert(c.self.form());
        return NativeResult{};
    });

    // Levelled lists are resolved against the player's level for now.
    // Days since the game started, from the game clock (not the VM's run time).
    vm.bind("Utility", "GetCurrentGameTime", [&world](NativeCall&) {
        return NativeResult{.value = Value::floating(static_cast<float>(world.game_days()))};
    });
    vm.bind("ObjectReference", "CalculateEncounterLevel", [&world](NativeCall&) {
        return NativeResult{.value = Value::integer(world.player_level()), .wait = -1.0};
    });
}

} // namespace skydot::vm
