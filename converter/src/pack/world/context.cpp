// SPDX-License-Identifier: GPL-3.0-or-later
#include "context.hpp"

#include "bethconv/archive/vpath.hpp"

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

std::vector<record::Script> CollectContext::global_scripts(const record::MergedRecord& merged,
                                                           record::ScriptData data,
                                                           bool& failed) const {
    for (auto& script : data.scripts) {
        for (auto& property : script.properties) {
            for (auto& object : property.objects) {
                object.form = record::FormId{global(merged, object.form, failed)};
            }
        }
    }
    return std::move(data.scripts);
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

std::string model_vpath(std::string_view modl) {
    std::string path = archive::normalize_vpath(modl);
    if (path.empty() || path.starts_with("meshes/")) {
        return path;
    }
    return "meshes/" + path;
}

std::string texture_vpath(std::string_view path) {
    std::string out = archive::normalize_vpath(path);
    if (out.empty() || out.starts_with("textures/")) {
        return out;
    }
    return "textures/" + out;
}

} // namespace bethconv::pack::detail
