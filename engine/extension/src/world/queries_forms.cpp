// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keywords (KYWD, and KWDA on bases), form lists (FLST) and messages (MESG)
// as world.fb carries them.
#include "world/queries.hpp"
#include "data/fb_search.hpp"
#include "data/text.hpp"

#include "skydot_formats/flags.hpp"
#include "skydot_formats/vpath.hpp"
#include "build/refs.hpp"
#include "world_generated.h"

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace wfb = bethconv::pack::wfb;

namespace skydot::queries {

std::int64_t find_keyword(const WorldData& data, const String& editor_id) {
    const auto* all = data.root() != nullptr ? data.root()->keywords() : nullptr;
    if (all == nullptr) {
        return 0;
    }
    const std::string wanted = formats::ascii_lower(std::string(editor_id.utf8().get_data()));
    for (const auto* k : *all) {
        if (k->editor_id() != nullptr && formats::ascii_lower(k->editor_id()->string_view()) == wanted) {
            return k->id();
        }
    }
    return 0;
}

godot::PackedInt64Array get_base_keywords(const WorldData& data, std::int64_t base) {
    godot::PackedInt64Array out;
    const auto* b = lookup(data.root() != nullptr ? data.root()->bases() : nullptr, static_cast<std::uint32_t>(base));
    if (b != nullptr && b->keywords() != nullptr) {
        for (const auto k : *b->keywords()) {
            out.push_back(k);
        }
    }
    return out;
}

Dictionary get_form_list(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* l = lookup(data.root() != nullptr ? data.root()->form_lists() : nullptr, static_cast<std::uint32_t>(id));
    if (l == nullptr) {
        return out;
    }
    out["editor_id"] = to_godot(l->editor_id());
    godot::PackedInt64Array forms;
    if (l->forms() != nullptr) {
        for (const auto f : *l->forms()) {
            forms.push_back(f);
        }
    }
    out["forms"] = forms;
    return out;
}

Dictionary get_message(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* m = lookup(data.root() != nullptr ? data.root()->messages() : nullptr, static_cast<std::uint32_t>(id));
    if (m == nullptr) {
        return out;
    }
    out["editor_id"] = to_godot(m->editor_id());
    out["title"] = to_godot(m->title());
    out["text"] = to_godot(m->text());
    out["box"] = formats::has_flag(m->flags(), wfb::MessageFlags::message_box);
    Array buttons;
    if (m->buttons() != nullptr) {
        for (const auto* b : *m->buttons()) {
            buttons.push_back(to_godot(b));
        }
    }
    out["buttons"] = buttons;
    return out;
}

} // namespace skydot::queries
