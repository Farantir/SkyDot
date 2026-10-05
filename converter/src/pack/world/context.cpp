// SPDX-License-Identifier: GPL-3.0-or-later
#include "context.hpp"

#include <utility>

namespace bethconv::pack::detail {

std::uint32_t CollectContext::global(const record::MergedRecord& merged, record::FormId local,
                                     bool& failed) const {
    if (local.is_null()) {
        return 0;
    }
    auto resolved = order_.resolve(merged.winner, local);
    if (!resolved) {
        failed = true;
        return 0;
    }
    return resolved->value;
}

std::vector<std::unique_ptr<wfb::ScriptT>> CollectContext::global_scripts(
    const record::MergedRecord& merged, const record::ScriptData& data, bool& failed) const {
    std::vector<std::unique_ptr<wfb::ScriptT>> out;
    out.reserve(data.scripts.size());
    for (const auto& script : data.scripts) {
        auto& s = out.emplace_back(std::make_unique<wfb::ScriptT>());
        s->name = script.name;
        s->status = static_cast<wfb::ScriptStatus>(script.status);
        for (const auto& p : script.properties) {
            auto& t = s->properties.emplace_back(std::make_unique<wfb::ScriptPropertyT>());
            t->name = p.name;
            t->type = static_cast<std::uint8_t>(p.type);
            t->status = p.status;
            for (const auto& o : p.objects) {
                t->objects.emplace_back(global(merged, o.form, failed), o.alias);
            }
            t->strings = p.strings;
            t->ints = p.integers;
            t->floats = p.floats;
        }
    }
    return out;
}

std::vector<std::uint32_t> CollectContext::global_all(const record::MergedRecord& merged,
                                                      const std::vector<record::FormId>& forms,
                                                      bool& failed) const {
    std::vector<std::uint32_t> out;
    out.reserve(forms.size());
    for (const record::FormId f : forms) {
        out.push_back(global(merged, f, failed));
    }
    return out;
}

} // namespace bethconv::pack::detail
