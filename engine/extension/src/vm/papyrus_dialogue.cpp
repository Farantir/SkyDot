// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's dialogue surface: selection (dialogue/dialogue.hpp) with the
// world, the quests and the condition functions behind it, and INFO fragments,
// whose TIF__ scripts attach to the INFO form as TopicInfo objects.
#include "vm/papyrus.hpp"

#include "data/actors.hpp"
#include "data/fb_search.hpp"
#include "vm/natives_common.hpp"
#include "build/refs.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;

namespace skydot {
namespace {

namespace wfb = bethconv::pack::wfb;

/// Seconds a line stays without a voice file to play.
constexpr double k_min_seconds = 2.0;
constexpr double k_seconds_per_character = 0.07;

double line_seconds(const std::string& text) {
    if (text.empty()) {
        return 0.0;
    }
    return std::max(k_min_seconds, k_seconds_per_character * static_cast<double>(text.size()));
}

} // namespace

/// Dice and names for the dialogue logic.
class DialogueHostImpl final : public dialogue::DialogueHost {
public:
    explicit DialogueHostImpl(const SkydotPapyrus& owner) : owner_(owner) {}
    double random() override {
        dice_ ^= dice_ >> 12;
        dice_ ^= dice_ << 25;
        dice_ ^= dice_ >> 27;
        return static_cast<double>((dice_ * 0x2545F4914F6CDD1Dull) >> 11) / static_cast<double>(1ull << 53);
    }
    std::string name_of(std::uint32_t ref) const override {
        const auto* root = owner_.world_.is_valid() ? owner_.world_->data().root() : nullptr;
        const std::uint32_t npc = ref != 0 ? owner_.dialogue_npc(ref) : 0;
        if (root == nullptr || npc == 0) {
            return {};
        }
        const auto* n = resolve_npc(*root, npc, wfb::NpcTemplateFlags::use_traits, ref);
        return n != nullptr && n->name() != nullptr ? n->name()->str() : std::string();
    }

private:
    const SkydotPapyrus& owner_;
    std::uint64_t dice_ = 0x2545F4914F6CDD1Dull;
};

std::uint32_t SkydotPapyrus::dialogue_npc(std::uint32_t ref) const {
    if (ref == k_player_ref) {
        return 0x7;
    }
    const auto* actor = world_.is_valid() ? world_->data().actor_ptr(ref) : nullptr;
    return actor != nullptr ? actor->base() : base_form_of(ref);
}

dialogue::Dialogue* SkydotPapyrus::dialogue_logic() {
    if (dialogue_ == nullptr) {
        const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
        if (root == nullptr) {
            return nullptr;
        }
        if (dialogue_host_ == nullptr) {
            dialogue_host_ = std::make_unique<DialogueHostImpl>(*this);
        }
        dialogue_ = std::make_unique<dialogue::Dialogue>(*root, dialogue_states_, condition_world(), *dialogue_host_);
    }
    return dialogue_.get();
}

bool SkydotPapyrus::dialogue_can_talk(std::int64_t ref) const {
    const auto id = static_cast<std::uint32_t>(ref);
    if (id == k_player_ref || !world_.is_valid() || world_->data().actor_ptr(id) == nullptr) {
        return false;
    }
    return !actors_.is_dead(id);
}

Dictionary SkydotPapyrus::dialogue_dictionary(const dialogue::Result& result) const {
    Dictionary out;
    Array lines;
    for (const auto& line : result.lines) {
        Dictionary l;
        l["text"] = String::utf8(line.text.c_str());
        l["seconds"] = line_seconds(line.text);
        l["info"] = static_cast<std::int64_t>(line.info);
        l["first"] = line.first;
        l["last"] = line.last;
        lines.push_back(l);
    }
    Array topics;
    for (const auto& topic : result.topics) {
        Dictionary t;
        t["id"] = static_cast<std::int64_t>(topic.topic);
        t["text"] = String::utf8(topic.text.c_str());
        t["said"] = topic.said;
        topics.push_back(t);
    }
    out["lines"] = lines;
    out["topics"] = topics;
    out["closed"] = result.closed;
    return out;
}

Dictionary SkydotPapyrus::dialogue_open(std::int64_t speaker) {
    auto* logic = dialogue_logic();
    return logic != nullptr ? dialogue_dictionary(logic->open(static_cast<std::uint32_t>(speaker))) : Dictionary();
}

Dictionary SkydotPapyrus::dialogue_choose(std::int64_t topic) {
    auto* logic = dialogue_logic();
    // The vendor services topic opens the barter menu, which does not exist yet.
    if (const auto* root = world_.is_valid() ? world_->data().root() : nullptr) {
        const auto* t = lookup(root->dialogue_topics(), static_cast<std::uint32_t>(topic));
        if (t != nullptr && t->editor_id() != nullptr && t->editor_id()->str() == "OfferServicesTopic") {
            godot::UtilityFunctions::print("barter: not implemented");
        }
    }
    return logic != nullptr ? dialogue_dictionary(logic->choose(static_cast<std::uint32_t>(topic))) : Dictionary();
}

void SkydotPapyrus::dialogue_close() {
    if (dialogue_ != nullptr) {
        dialogue_->close();
    }
}

bool SkydotPapyrus::dialogue_is_open() const { return dialogue_ != nullptr && dialogue_->is_open(); }

Dictionary SkydotPapyrus::dialogue_topics_debug(std::int64_t speaker) {
    auto* logic = dialogue_logic();
    return logic != nullptr ? dialogue_dictionary(logic->peek(static_cast<std::uint32_t>(speaker))) : Dictionary();
}

bool SkydotPapyrus::dialogue_fragment(std::int64_t info_id, bool end, std::int64_t speaker) {
    auto* logic = dialogue_logic();
    const auto form = static_cast<std::uint32_t>(info_id);
    const auto* info = logic != nullptr ? logic->info(form) : nullptr;
    if (info == nullptr) {
        return false;
    }
    const auto* function = end ? info->end_fragment() : info->begin_fragment();
    if (function == nullptr || function->size() == 0 || info->fragment_script() == nullptr) {
        return false;
    }
    const std::string script = info->fragment_script()->str();
    const std::uint32_t quest = logic->topic_of_info(form) != 0
                                    ? [&] {
                                          const auto* topic = lookup(world_->data().root()->dialogue_topics(),
                                                                     logic->topic_of_info(form));
                                          return topic != nullptr ? topic->quest() : 0U;
                                      }()
                                    : 0U;
    if (!info_quests_.contains(form)) {
        info_quests_[form] = quest;
        attach_scripts(form, script_list(info->scripts(), false), quest);
    }
    auto* instance = vm_->instance(form, script);
    if (instance == nullptr) {
        instance = vm_->attach(form, script);
    }
    if (instance == nullptr) {
        vm_->error("dialogue: no fragment script " + script);
        return false;
    }
    const vm::Value akSpeaker = to_value(Variant(speaker), "ObjectReference");
    if (vm_->call_method(instance, function->str(), {akSpeaker}) == 0) {
        vm_->error("dialogue: no fragment " + function->str() + " in " + script);
    }
    return true;
}

} // namespace skydot
