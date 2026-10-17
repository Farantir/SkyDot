// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialogue/dialogue.hpp"

#include "data/fb_search.hpp"
#include "dialogue/voice.hpp"
#include "skydot_formats/flags.hpp"

#include <algorithm>
#include <cctype>

namespace skydot::dialogue {
namespace {

/// The subtypes spoken when dialogue opens: Skyrim's own data has "Hello"
/// (HELO) for it; "GREE" is the older name and none are in Skyrim.esm.
constexpr std::uint32_t k_gree = 0x45455247; // "GREE"
constexpr std::uint32_t k_helo = 0x4F4C4548; // "HELO"

bool has(const wfb::DialogueInfo& info, wfb::DialogueInfoFlags flag) {
    return formats::has_flag(info.flags(), flag);
}

bool equal_nocase(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

// ---- indexes -----------------------------------------------------------------

void Dialogue::build_indexes() const {
    if (indexed_) {
        return;
    }
    indexed_ = true;
    if (world_.dialogue_topics() != nullptr) {
        for (const auto* topic : *world_.dialogue_topics()) {
            if (topic->subtype_code() == k_gree || topic->subtype_code() == k_helo) {
                greetings_.push_back(topic);
            }
            if (topic->infos() != nullptr) {
                for (const auto* info : *topic->infos()) {
                    infos_.emplace(info->id(), std::pair{info, topic->id()});
                }
            }
        }
    }
    if (world_.dialogue_branches() != nullptr) {
        for (const auto* branch : *world_.dialogue_branches()) {
            if (!formats::has_flag(branch->flags(), wfb::DialogueBranchFlags::top_level) || branch->category() != 0) {
                continue;
            }
            if (const auto* topic = lookup(world_.dialogue_topics(), branch->start_topic())) {
                starts_.push_back({branch, topic, quest_priority(topic->quest())});
            }
        }
    }
}

const wfb::DialogueInfo* Dialogue::info(std::uint32_t id) const {
    build_indexes();
    const auto it = infos_.find(id);
    return it != infos_.end() ? it->second.first : nullptr;
}

std::uint32_t Dialogue::topic_of_info(std::uint32_t id) const {
    build_indexes();
    const auto it = infos_.find(id);
    return it != infos_.end() ? it->second.second : 0;
}

std::int32_t Dialogue::quest_priority(std::uint32_t quest) const {
    const auto* q = lookup(world_.quests(), quest);
    return q != nullptr ? q->priority() : 0;
}

const wfb::DialogueBranch* Dialogue::branch_of(const wfb::DialogueTopic& topic) const {
    return lookup(world_.dialogue_branches(), topic.branch());
}

// ---- availability -------------------------------------------------------------

conditions::ConditionContext Dialogue::context(const wfb::DialogueTopic& topic) const {
    return {.subject = speaker_,
            .target = k_player,
            .quest = topic.quest(),
            .unknown = conditions::UnknownPolicy::fails};
}

bool Dialogue::topic_open(const wfb::DialogueTopic& topic) const {
    if (!conditions_.quest_running(topic.quest())) {
        return false;
    }
    const auto* quest = lookup(world_.quests(), topic.quest());
    return quest == nullptr ||
           conditions::conditions_pass(quest->dialogue_conditions(), conditions_, context(topic));
}

bool Dialogue::passes(const wfb::DialogueInfo& info, const wfb::DialogueTopic& topic) const {
    if (has(info, wfb::DialogueInfoFlags::say_once) && states_.said_once(info.id())) {
        return false;
    }
    return conditions::conditions_pass(info.conditions(), conditions_, context(topic));
}

const wfb::DialogueInfo* Dialogue::pick(const wfb::DialogueTopic& topic, bool dice) {
    const auto* list = topic.infos();
    if (list == nullptr || !topic_open(topic)) {
        return nullptr;
    }
    const auto& infos = *list;
    for (flatbuffers::uoffset_t i = 0; i < infos.size(); ++i) {
        const auto* info = infos.Get(i);
        if (!has(*info, wfb::DialogueInfoFlags::random)) {
            if (passes(*info, topic)) {
                return info;
            }
            continue;
        }
        // A run of Random INFOs: one of those that pass.
        std::vector<const wfb::DialogueInfo*> open;
        flatbuffers::uoffset_t j = i;
        for (; j < infos.size() && has(*infos.Get(j), wfb::DialogueInfoFlags::random); ++j) {
            if (passes(*infos.Get(j), topic)) {
                open.push_back(infos.Get(j));
            }
        }
        if (!open.empty()) {
            const auto index = dice ? std::min(open.size() - 1, static_cast<std::size_t>(
                                                                    host_.random() * static_cast<double>(open.size())))
                                    : 0;
            return open[index];
        }
        i = j - 1;
    }
    return nullptr;
}

// ---- lists ----------------------------------------------------------------------

bool Dialogue::entry_of(const wfb::DialogueTopic& topic, TopicEntry& out) {
    const auto* info = pick(topic, false);
    if (info == nullptr) {
        return false;
    }
    const std::string prompt = info->prompt() != nullptr ? info->prompt()->str() : std::string();
    out.topic = topic.id();
    out.text = !prompt.empty() ? prompt : (topic.text() != nullptr ? topic.text()->str() : std::string());
    out.said = states_.topic_said(topic.id());
    return !out.text.empty();
}

std::vector<TopicEntry> Dialogue::top_level() {
    build_indexes();
    std::vector<const Start*> order;
    for (const auto& start : starts_) {
        if (!blocking_only_ ||
            formats::has_flag(start.branch->flags(), wfb::DialogueBranchFlags::blocking)) {
            order.push_back(&start);
        }
    }
    std::ranges::stable_sort(order, [](const Start* a, const Start* b) {
        if (a->quest_priority != b->quest_priority) {
            return a->quest_priority > b->quest_priority;
        }
        if (a->topic->priority() != b->topic->priority()) {
            return a->topic->priority() > b->topic->priority();
        }
        return a->topic->id() < b->topic->id();
    });
    std::vector<TopicEntry> out;
    for (const Start* start : order) {
        TopicEntry entry;
        if (entry_of(*start->topic, entry) &&
            std::ranges::none_of(out, [&](const TopicEntry& e) { return e.topic == entry.topic; })) {
            out.push_back(std::move(entry));
        }
    }
    return out;
}

std::vector<TopicEntry> Dialogue::list_for(const std::vector<std::uint32_t>& topics) {
    std::vector<TopicEntry> out;
    for (const auto id : topics) {
        const auto* topic = lookup(world_.dialogue_topics(), id);
        TopicEntry entry;
        if (topic != nullptr && entry_of(*topic, entry) &&
            std::ranges::none_of(out, [&](const TopicEntry& e) { return e.topic == entry.topic; })) {
            out.push_back(std::move(entry));
        }
    }
    return out;
}

// ---- speaking ---------------------------------------------------------------------

std::string Dialogue::substitute(const std::string& text, const wfb::DialogueTopic& topic) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto open = text.find('<', at);
        const auto close = open == std::string::npos ? open : text.find('>', open);
        if (close == std::string::npos) {
            out.append(text, at, std::string::npos);
            break;
        }
        out.append(text, at, open - at);
        const std::string tag = text.substr(open + 1, close - open - 1);
        std::string replacement;
        for (const std::string prefix : {"Alias=", "Alias.ShortName="}) {
            if (tag.starts_with(prefix)) {
                const std::string alias_name = tag.substr(prefix.size());
                const auto* quest = lookup(world_.quests(), topic.quest());
                if (quest != nullptr && quest->aliases() != nullptr) {
                    for (const auto* alias : *quest->aliases()) {
                        if (alias->name() != nullptr && equal_nocase(alias->name()->str(), alias_name)) {
                            replacement = host_.name_of(conditions_.alias_fill(topic.quest(), alias->id()));
                            break;
                        }
                    }
                }
                break;
            }
        }
        if (replacement.empty()) {
            ++unresolved_tags_;
            out.append(text, open, close - open + 1);
        } else {
            out += replacement;
        }
        at = close + 1;
    }
    return out;
}

void Dialogue::speak(const wfb::DialogueTopic& topic, const wfb::DialogueInfo& info, Result& out, bool commit,
                     int depth) {
    // A shared INFO lends its responses.
    const wfb::DialogueInfo* source = &info;
    if (info.shared_info() != 0) {
        if (const auto* shared = this->info(info.shared_info())) {
            source = shared;
        }
    }
    std::vector<const wfb::DialogueResponse*> responses;
    if (source->responses() != nullptr) {
        responses.assign(source->responses()->begin(), source->responses()->end());
    }
    std::ranges::stable_sort(responses, {}, [](const wfb::DialogueResponse* r) { return r->number(); });
    const std::size_t first = out.lines.size();
    const std::string voice_type = speaker_voice_type();
    for (const auto* response : responses) {
        out.lines.push_back({.text = substitute(response->text() != nullptr ? response->text()->str() : "", topic),
                             .voice = voice_of(*source, response->number(), voice_type),
                             .info = info.id()});
    }
    if (out.lines.size() == first) {
        out.lines.push_back({.text = {}, .voice = {}, .info = info.id()});
    }
    out.lines[first].first = true;
    out.lines.back().last = true;

    if (commit) {
        states_.mark_info(info.id(), has(info, wfb::DialogueInfoFlags::say_once));
        states_.mark_topic(topic.id());
    }
    links_.clear();
    exclusive_ = false;
    if (has(info, wfb::DialogueInfoFlags::goodbye)) {
        out.closed = true;
        return;
    }
    if (info.links() != nullptr) {
        links_.assign(info.links()->begin(), info.links()->end());
    }
    const auto* branch = branch_of(topic);
    exclusive_ = branch != nullptr && formats::has_flag(branch->flags(), wfb::DialogueBranchFlags::exclusive);
    if (has(info, wfb::DialogueInfoFlags::invisible_continue) && depth + 1 < k_max_chain) {
        for (const auto id : links_) {
            const auto* next = lookup(world_.dialogue_topics(), id);
            const auto* next_info = next != nullptr ? pick(*next, dice_) : nullptr;
            if (next_info != nullptr) {
                speak(*next, *next_info, out, commit, depth + 1);
                return;
            }
        }
    }
}

std::string Dialogue::speaker_voice_type() const {
    std::uint32_t voice = conditions_.voice_type_of(speaker_);
    if (voice == 0) {
        const auto [male, female] = conditions_.race_voice_types(conditions_.race_of(speaker_));
        voice = conditions_.is_female(speaker_) ? female : male;
    }
    const auto* type = voice != 0 ? lookup(world_.voice_types(), voice) : nullptr;
    return type != nullptr && type->editor_id() != nullptr ? type->editor_id()->str() : std::string();
}

std::string Dialogue::voice_of(const wfb::DialogueInfo& source, unsigned number, const std::string& voice_type) const {
    std::string plugin;
    std::uint32_t local = 0;
    const auto* topic = lookup(world_.dialogue_topics(), topic_of_info(source.id()));
    if (voice_type.empty() || topic == nullptr || !plugin_of_form(world_, source.id(), plugin, local)) {
        return {};
    }
    const auto* quest = lookup(world_.quests(), topic->quest());
    const std::string quest_edid = quest != nullptr && quest->editor_id() != nullptr ? quest->editor_id()->str() : "";
    const std::string topic_edid = topic->editor_id() != nullptr ? topic->editor_id()->str() : "";
    return voice_path(plugin, voice_type, voice_stem(quest_edid, topic_edid, local, number));
}

void Dialogue::finish(Result& out) {
    if (out.closed) {
        return;
    }
    if (!links_.empty()) {
        out.topics = list_for(links_);
        if (!exclusive_ || out.topics.empty()) {
            for (auto& entry : top_level()) {
                if (std::ranges::none_of(out.topics, [&](const TopicEntry& e) { return e.topic == entry.topic; })) {
                    out.topics.push_back(std::move(entry));
                }
            }
        }
        return;
    }
    out.topics = top_level();
}

Result Dialogue::begin(std::uint32_t speaker, bool commit) {
    build_indexes();
    speaker_ = speaker;
    blocking_only_ = false;
    dice_ = commit;
    links_.clear();
    exclusive_ = false;
    Result out;
    if (commit) {
        states_.set_speaker(speaker);
        states_.mark_talked(speaker);
    }
    const auto order = [](const Start* a, const Start* b) {
        if (a->quest_priority != b->quest_priority) {
            return a->quest_priority > b->quest_priority;
        }
        if (a->topic->priority() != b->topic->priority()) {
            return a->topic->priority() > b->topic->priority();
        }
        return a->topic->id() < b->topic->id();
    };
    std::vector<const Start*> blocking;
    for (const auto& start : starts_) {
        if (formats::has_flag(start.branch->flags(), wfb::DialogueBranchFlags::blocking)) {
            blocking.push_back(&start);
        }
    }
    std::ranges::stable_sort(blocking, order);
    for (const Start* start : blocking) {
        if (const auto* chosen = pick(*start->topic, dice_)) {
            blocking_only_ = true;
            speak(*start->topic, *chosen, out, commit, 0);
            finish(out);
            return out;
        }
    }
    std::vector<const wfb::DialogueTopic*> greetings = greetings_;
    std::ranges::stable_sort(greetings, [this](const wfb::DialogueTopic* a, const wfb::DialogueTopic* b) {
        const auto pa = quest_priority(a->quest());
        const auto pb = quest_priority(b->quest());
        return pa != pb ? pa > pb : a->id() < b->id();
    });
    for (const auto* topic : greetings) {
        if (const auto* chosen = pick(*topic, dice_)) {
            speak(*topic, *chosen, out, commit, 0);
            break;
        }
    }
    finish(out);
    return out;
}

Result Dialogue::open(std::uint32_t speaker) {
    Result out = begin(speaker, true);
    if (out.closed) {
        close();
    }
    return out;
}

Result Dialogue::peek(std::uint32_t speaker) {
    const auto saved_speaker = speaker_;
    const auto saved_blocking = blocking_only_;
    const auto saved_links = links_;
    const bool saved_exclusive = exclusive_;
    const auto saved_dice = dice_;
    Result out = begin(speaker, false);
    speaker_ = saved_speaker;
    blocking_only_ = saved_blocking;
    links_ = saved_links;
    exclusive_ = saved_exclusive;
    dice_ = saved_dice;
    return out;
}

Result Dialogue::choose(std::uint32_t topic_id) {
    Result out;
    if (speaker_ == 0) {
        return out;
    }
    dice_ = true;
    const auto* topic = lookup(world_.dialogue_topics(), topic_id);
    const auto* chosen = topic != nullptr ? pick(*topic, true) : nullptr;
    if (chosen == nullptr) {
        finish(out);
        return out;
    }
    speak(*topic, *chosen, out, true, 0);
    finish(out);
    if (out.closed) {
        close();
    }
    return out;
}

void Dialogue::close() {
    speaker_ = 0;
    blocking_only_ = false;
    links_.clear();
    exclusive_ = false;
    states_.set_speaker(0);
}

} // namespace skydot::dialogue
