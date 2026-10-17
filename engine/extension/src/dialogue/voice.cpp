// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialogue/voice.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace skydot::dialogue {
namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

constexpr std::size_t k_name_limit = 25;
constexpr std::size_t k_quest_limit = 10;

} // namespace

std::string voice_stem(const std::string& quest_edid, const std::string& topic_edid, std::uint32_t local_id,
                       unsigned number) {
    std::string q = lower(quest_edid);
    std::string t = lower(topic_edid);
    if (q.size() + t.size() > k_name_limit) {
        q.resize(std::min(q.size(), k_quest_limit));
        t.resize(std::min(t.size(), k_name_limit - q.size()));
    }
    char id[16];
    std::snprintf(id, sizeof id, "%08x", local_id);
    return q + "_" + t + "_" + id + "_" + std::to_string(number);
}

bool plugin_of_form(const bethconv::pack::wfb::World& world, std::uint32_t form, std::string& plugin_file,
                    std::uint32_t& local_id) {
    if (world.plugins() == nullptr) {
        return false;
    }
    for (const auto* plugin : *world.plugins()) {
        const bool light = plugin->light();
        const std::uint32_t mask = light ? 0xFFFFF000U : 0xFF000000U;
        if ((form & mask) == plugin->prefix() && plugin->name() != nullptr) {
            plugin_file = plugin->name()->str();
            local_id = form & (light ? 0xFFFU : 0xFFFFFFU);
            return true;
        }
    }
    return false;
}

std::string voice_path(const std::string& plugin_file, const std::string& voice_type_edid, const std::string& stem) {
    return "sound/voice/" + lower(plugin_file) + "/" + lower(voice_type_edid) + "/" + stem + ".fuz";
}

} // namespace skydot::dialogue
