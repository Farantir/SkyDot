// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dialogue selection (dialogue/dialogue.cpp) over a world.fb built in memory,
// with the conditions answered by a fake world: blocking branches, greetings
// by quest priority, Random runs, Say Once, links with Invisible Continue and
// Goodbye, quest dialogue conditions, shared INFOs, alias text, and the DLGS
// section.
#include "support/fake_condition_world.hpp"

#include "dialogue/dialogue.hpp"
#include "dialogue/dialogue_states.hpp"
#include "conditions/functions.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

using namespace skydot::testing;
namespace dialogue = skydot::dialogue;
namespace wfb = bethconv::pack::wfb;

namespace {

constexpr std::uint32_t k_gree = 0x45455247;
constexpr std::uint32_t k_idle = 0x454C4449;
constexpr std::uint32_t k_helo = 0x4F4C4548;
using InfoFlags = wfb::DialogueInfoFlags;
using BranchFlags = wfb::DialogueBranchFlags;

struct InfoSpec {
    std::uint32_t id;
    std::vector<std::string> lines = {};
    InfoFlags flags = {};
    std::vector<std::uint32_t> links = {};
    std::string prompt = {};
    std::uint32_t shared = 0;
    /// GetIsID conditions on these NPC_s, ORed.
    std::vector<std::uint32_t> speakers = {};
    /// GetGlobalValue(global) == 1.
    std::uint32_t global = 0;
};
struct TopicSpec {
    std::uint32_t id;
    std::string text = {};
    std::uint32_t quest = 0x100;
    std::uint32_t branch = 0;
    float priority = 0.0F;
    std::uint32_t subtype = 0;
    std::vector<InfoSpec> infos = {};
};
struct BranchSpec {
    std::uint32_t id;
    std::uint32_t start;
    BranchFlags flags = BranchFlags::top_level;
};
struct QuestSpec {
    std::uint32_t id;
    std::uint8_t priority = 0;
    std::uint32_t gate_global = 0; ///< a dialogue condition: GetGlobalValue == 1
    std::vector<std::pair<std::uint32_t, std::string>> aliases = {};
};

using CondOffsets = std::vector<flatbuffers::Offset<wfb::Condition>>;

class Built {
public:
    Built(std::vector<QuestSpec> quests, std::vector<BranchSpec> branches, std::vector<TopicSpec> topics) {
        flatbuffers::FlatBufferBuilder fbb;
        const auto cond = [&](std::uint16_t function, std::uint32_t p1, bool or_next) {
            return wfb::CreateConditionDirect(fbb, static_cast<std::uint8_t>(or_next ? 1 : 0), function, 1.0F, 0, p1);
        };
        std::vector<flatbuffers::Offset<wfb::Quest>> qs;
        for (const auto& q : quests) {
            std::vector<flatbuffers::Offset<wfb::QuestAlias>> aliases;
            for (const auto& [id, name] : q.aliases) {
                aliases.push_back(wfb::CreateQuestAliasDirect(fbb, id, name.c_str()));
            }
            CondOffsets gate;
            if (q.gate_global != 0) {
                gate.push_back(cond(skydot::conditions::fn::get_global_value, q.gate_global, false));
            }
            qs.push_back(wfb::CreateQuestDirect(fbb, q.id, nullptr, nullptr, {}, q.priority, 0, 0, nullptr, nullptr,
                                                nullptr, nullptr, nullptr, &aliases, &gate));
        }
        std::vector<flatbuffers::Offset<wfb::DialogueBranch>> bs;
        for (const auto& b : branches) {
            bs.push_back(wfb::CreateDialogueBranchDirect(fbb, b.id, nullptr, 0x100, b.start, b.flags, 0));
        }
        std::vector<flatbuffers::Offset<wfb::DialogueTopic>> ts;
        for (const auto& t : topics) {
            std::vector<flatbuffers::Offset<wfb::DialogueInfo>> infos;
            for (const auto& i : t.infos) {
                CondOffsets conditions;
                for (std::size_t k = 0; k < i.speakers.size(); ++k) {
                    conditions.push_back(cond(skydot::conditions::fn::get_is_id, i.speakers[k], k + 1 < i.speakers.size()));
                }
                if (i.global != 0) {
                    conditions.push_back(cond(skydot::conditions::fn::get_global_value, i.global, false));
                }
                std::vector<flatbuffers::Offset<wfb::DialogueResponse>> responses;
                std::uint8_t number = 1;
                for (const auto& line : i.lines) {
                    responses.push_back(wfb::CreateDialogueResponseDirect(fbb, 0, 0, number++, 0, 0, line.c_str()));
                }
                infos.push_back(wfb::CreateDialogueInfoDirect(
                    fbb, i.id, nullptr, i.flags, 0, 0, i.prompt.empty() ? nullptr : i.prompt.c_str(), 0, i.shared,
                    &i.links, 0, 0, &conditions, &responses));
            }
            ts.push_back(wfb::CreateDialogueTopicDirect(
                fbb, t.id, nullptr, t.text.empty() ? nullptr : t.text.c_str(), t.quest, t.branch, t.priority, {}, 0, 0,
                t.subtype, &infos));
        }
        const auto q_v = fbb.CreateVector(qs);
        const auto b_v = fbb.CreateVector(bs);
        const auto t_v = fbb.CreateVector(ts);
        wfb::WorldBuilder world(fbb);
        world.add_quests(q_v);
        world.add_dialogue_branches(b_v);
        world.add_dialogue_topics(t_v);
        wfb::FinishWorldBuffer(fbb, world.Finish());
        bytes_.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
        root_ = wfb::GetWorld(bytes_.data());
    }
    const wfb::World& operator*() const { return *root_; }

private:
    std::vector<std::uint8_t> bytes_;
    const wfb::World* root_ = nullptr;
};

class FakeHost final : public dialogue::DialogueHost {
public:
    double roll = 0.0;
    std::map<std::uint32_t, std::string> names;
    double random() override { return roll; }
    std::string name_of(std::uint32_t ref) const override {
        const auto it = names.find(ref);
        return it != names.end() ? it->second : std::string();
    }
};

std::vector<std::string> texts(const dialogue::Result& r) {
    std::vector<std::string> out;
    for (const auto& l : r.lines) {
        out.push_back(l.text);
    }
    return out;
}
std::vector<std::string> topic_texts(const dialogue::Result& r) {
    std::vector<std::string> out;
    for (const auto& t : r.topics) {
        out.push_back(t.text);
    }
    return out;
}

/// A world of one running quest 0x100, the speaker 0x20 (NPC_ 0x900).
struct Rig {
    FakeConditionWorld conditions;
    dialogue::DialogueStates states;
    FakeHost host;
    Rig() {
        conditions.quests[0x100].running = true;
        conditions.quests[0x200].running = true;
        conditions.npcs[0x20] = 0x900;
    }
};

} // namespace

TEST_CASE("a blocking branch speaks first and hides the other topics", "[dialogue]") {
    Built world({{.id = 0x100}},
                {{.id = 1, .start = 10, .flags = BranchFlags::top_level | BranchFlags::blocking},
                 {.id = 2, .start = 11}},
                {{.id = 10, .text = "Stop!", .branch = 1, .infos = {{.id = 100, .lines = {"Halt."}}}},
                 {.id = 11, .text = "Hello", .branch = 2, .infos = {{.id = 101, .lines = {"Hm."}}}}});
    Rig rig;
    dialogue::Dialogue d(*world, rig.states, rig.conditions, rig.host);
    const auto r = d.open(0x20);
    CHECK(texts(r) == std::vector<std::string>{"Halt."});
    CHECK(topic_texts(r) == std::vector<std::string>{"Stop!"});
    CHECK(rig.states.talked_to_player(0x20));
    CHECK(rig.states.speaker() == 0x20);
    d.close();
    CHECK(rig.states.speaker() == 0);
    CHECK(rig.states.talked_to_player(0x20)); // stays
}

TEST_CASE("the greeting comes from the running quest of highest priority", "[dialogue]") {
    Built world({{.id = 0x100, .priority = 10}, {.id = 0x200, .priority = 50}},
                {{.id = 1, .start = 10}},
                {{.id = 10, .text = "Ask", .branch = 1, .infos = {{.id = 100, .lines = {"Yes?"}}}},
                 {.id = 20, .quest = 0x100, .subtype = k_gree, .infos = {{.id = 200, .lines = {"Low."}}}},
                 {.id = 21, .quest = 0x200, .subtype = k_gree, .infos = {{.id = 201, .lines = {"High."}}}},
                 {.id = 22, .quest = 0x300, .subtype = k_gree, .infos = {{.id = 202, .lines = {"Stopped."}}}},
                 {.id = 23, .quest = 0x200, .subtype = k_idle, .infos = {{.id = 203, .lines = {"Not a greeting."}}}},
                 {.id = 24, .quest = 0x400, .subtype = k_helo, .infos = {{.id = 204, .lines = {"Hello is a greeting."}}}}});
    Rig rig;
    dialogue::Dialogue d(*world, rig.states, rig.conditions, rig.host);
    auto r = d.open(0x20);
    CHECK(texts(r) == std::vector<std::string>{"High."});
    CHECK(topic_texts(r) == std::vector<std::string>{"Ask"});
    // Quest 0x200 stops: the other greeting speaks.
    d.close();
    rig.conditions.quests[0x200].running = false;
    CHECK(texts(d.open(0x20)) == std::vector<std::string>{"Low."});
    d.close();
    rig.conditions.quests[0x100].running = false;
    r = d.open(0x20);
    CHECK(r.lines.empty());
    CHECK(r.topics.empty()); // the topic's quest does not run either
    // Hello is a greeting too.
    d.close();
    rig.conditions.quests[0x400].running = true;
    CHECK(texts(d.open(0x20)) == std::vector<std::string>{"Hello is a greeting."});
}

TEST_CASE("an INFO is chosen by its conditions, a Random run by the dice", "[dialogue]") {
    const InfoFlags random = InfoFlags::random;
    Built world({{.id = 0x100}}, {{.id = 1, .start = 10}},
                {{.id = 10, .text = "Talk", .branch = 1,
                  .infos = {{.id = 100, .lines = {"Only for 0x901."}, .speakers = {0x901}},
                            {.id = 101, .lines = {"A"}, .flags = random},
                            {.id = 102, .lines = {"B"}, .flags = random, .speakers = {0x901}},
                            {.id = 103, .lines = {"C"}, .flags = random},
                            {.id = 104, .lines = {"Fallback"}}}}});
    Rig rig;
    dialogue::Dialogue d(*world, rig.states, rig.conditions, rig.host);
    d.open(0x20);
    rig.host.roll = 0.0;
    CHECK(texts(d.choose(10)) == std::vector<std::string>{"A"});
    rig.host.roll = 0.99; // B does not pass for 0x900: A or C
    CHECK(texts(d.choose(10)) == std::vector<std::string>{"C"});
    rig.host.roll = 0.4;
    CHECK(texts(d.choose(10)) == std::vector<std::string>{"A"});
    // For NPC 0x901 the first INFO passes and ends the search.
    rig.conditions.npcs[0x20] = 0x901;
    CHECK(texts(d.choose(10)) == std::vector<std::string>{"Only for 0x901."});
}

TEST_CASE("a Say Once INFO is said once", "[dialogue]") {
    Built world({{.id = 0x100}}, {{.id = 1, .start = 10}},
                {{.id = 10, .text = "Rumours", .branch = 1,
                  .infos = {{.id = 100, .lines = {"First time."}, .flags = InfoFlags::say_once},
                            {.id = 101, .lines = {"Again."}}}}});
    Rig rig;
    dialogue::Dialogue d(*world, rig.states, rig.conditions, rig.host);
    auto r = d.open(0x20);
    REQUIRE(r.topics.size() == 1);
    CHECK_FALSE(r.topics[0].said);
    CHECK(texts(d.choose(10)) == std::vector<std::string>{"First time."});
    r = d.choose(10);
    CHECK(texts(r) == std::vector<std::string>{"Again."});
    REQUIRE(r.topics.size() == 1);
    CHECK(r.topics[0].said);
    CHECK(rig.states.said_once(100));
    CHECK_FALSE(rig.states.said_once(101));
    CHECK(rig.states.info_said(101));
}

TEST_CASE("links list their topics, Invisible Continue speaks the next, Goodbye closes", "[dialogue]") {
    Built world({{.id = 0x100}},
                {{.id = 1, .start = 10}, {.id = 2, .start = 12, .flags = BranchFlags::top_level | BranchFlags::exclusive}},
                {{.id = 10, .text = "Start", .branch = 1,
                  .infos = {{.id = 100, .lines = {"Pick."}, .links = {11, 12}}}},
                 {.id = 11, .text = "Chain", .branch = 1,
                  .infos = {{.id = 101, .lines = {"One.", "Two."}, .flags = InfoFlags::invisible_continue, .links = {13}}}},
                 {.id = 12, .text = "Exclusive", .branch = 2,
                  .infos = {{.id = 102, .lines = {"Excl."}, .links = {14}}}},
                 {.id = 13, .text = "End", .branch = 2,
                  .infos = {{.id = 103, .lines = {"Bye."}, .flags = InfoFlags::goodbye}}},
                 {.id = 14, .text = "Only me", .branch = 2, .infos = {{.id = 104, .lines = {"Mine."}}}}});
    Rig rig;
    dialogue::Dialogue d(*world, rig.states, rig.conditions, rig.host);
    auto r = d.open(0x20);
    CHECK(topic_texts(r) == std::vector<std::string>{"Start", "Exclusive"});
    // Links first, then the top-level list.
    r = d.choose(10);
    CHECK(topic_texts(r) == std::vector<std::string>{"Chain", "Exclusive", "Start"});
    // Chain: its lines, then at once the link's, which says goodbye.
    r = d.choose(11);
    CHECK(texts(r) == std::vector<std::string>{"One.", "Two.", "Bye."});
    CHECK(r.closed);
    CHECK(r.topics.empty());
    CHECK(rig.states.speaker() == 0);
    CHECK_FALSE(d.is_open());
    // The first and last lines of each INFO carry the fragment marks.
    CHECK(r.lines[0].first);
    CHECK_FALSE(r.lines[0].last);
    CHECK(r.lines[1].last);
    CHECK(r.lines[2].first);
    CHECK(r.lines[2].last);
    CHECK(r.lines[2].info == 103);
    // An exclusive branch lists only its links.
    d.open(0x20);
    r = d.choose(12);
    CHECK(topic_texts(r) == std::vector<std::string>{"Only me"});
}

TEST_CASE("a quest's dialogue conditions and a topic's quest gate its topics", "[dialogue]") {
    Built world({{.id = 0x100, .gate_global = 0x500}}, {{.id = 1, .start = 10}},
                {{.id = 10, .text = "Gated", .branch = 1, .infos = {{.id = 100, .lines = {"Open."}}}}});
    Rig rig;
    dialogue::Dialogue d(*world, rig.states, rig.conditions, rig.host);
    CHECK(d.open(0x20).topics.empty());
    d.close();
    rig.conditions.globals[0x500] = 1.0;
    CHECK(topic_texts(d.open(0x20)) == std::vector<std::string>{"Gated"});
    d.close();
    rig.conditions.quests[0x100].running = false;
    CHECK(d.open(0x20).topics.empty());
}

TEST_CASE("a shared INFO lends its responses, a prompt replaces the topic's text", "[dialogue]") {
    Built world({{.id = 0x100}}, {{.id = 1, .start = 10}, {.id = 2, .start = 11}},
                {{.id = 10, .text = "Topic text", .branch = 1,
                  .infos = {{.id = 100, .prompt = "Prompt text", .shared = 201}}},
                 {.id = 11, .text = "Other", .branch = 2, .infos = {{.id = 101, .lines = {"Own."}}}},
                 {.id = 12, .quest = 0x200, .infos = {{.id = 201, .lines = {"Shared line."}}}}});
    Rig rig;
    dialogue::Dialogue d(*world, rig.states, rig.conditions, rig.host);
    const auto r = d.open(0x20);
    CHECK(topic_texts(r) == std::vector<std::string>{"Prompt text", "Other"});
    CHECK(texts(d.choose(10)) == std::vector<std::string>{"Shared line."});
}

TEST_CASE("alias tags take the fill's name, other tags stay and are counted", "[dialogue]") {
    Built world({{.id = 0x100, .aliases = {{3, "Guard"}}}}, {{.id = 1, .start = 10}},
                {{.id = 10, .text = "Who?", .branch = 1,
                  .infos = {{.id = 100, .lines = {"<Alias=Guard> and <Alias.ShortName=guard> saw <Global=X>.",
                                                  "<Alias=Nobody>"}}}}});
    Rig rig;
    rig.conditions.aliases[{0x100, 3}] = 0x30;
    rig.host.names[0x30] = "Hroki";
    dialogue::Dialogue d(*world, rig.states, rig.conditions, rig.host);
    d.open(0x20);
    const auto r = d.choose(10);
    CHECK(texts(r) == std::vector<std::string>{"Hroki and Hroki saw <Global=X>.", "<Alias=Nobody>"});
    CHECK(d.unresolved_tags() == 2);
}

TEST_CASE("peek says what opening would, changing nothing", "[dialogue]") {
    Built world({{.id = 0x100}}, {{.id = 1, .start = 10}},
                {{.id = 10, .text = "Ask", .branch = 1, .infos = {{.id = 100, .lines = {"Hm."}}}},
                 {.id = 20, .subtype = k_gree,
                  .infos = {{.id = 200, .lines = {"Hello."}, .flags = InfoFlags::say_once}}}});
    Rig rig;
    dialogue::Dialogue d(*world, rig.states, rig.conditions, rig.host);
    const auto peeked = d.peek(0x20);
    CHECK(texts(peeked) == std::vector<std::string>{"Hello."});
    CHECK(topic_texts(peeked) == std::vector<std::string>{"Ask"});
    CHECK_FALSE(rig.states.info_said(200));
    CHECK_FALSE(rig.states.talked_to_player(0x20));
    CHECK_FALSE(d.is_open());
    CHECK(texts(d.open(0x20)) == std::vector<std::string>{"Hello."});
}

TEST_CASE("the DLGS section round-trips", "[dialogue][save]") {
    dialogue::DialogueStates a;
    a.mark_info(7, true);
    a.mark_info(8, false);
    a.mark_topic(3);
    a.mark_talked(0x20);
    a.set_speaker(0x20);
    const auto bytes = a.save();
    dialogue::DialogueStates b;
    REQUIRE(dialogue::DialogueStates::load(skydot::save::ByteReader(bytes.data(), bytes.size()), b));
    CHECK(b.info_said(7));
    CHECK(b.info_said(8));
    CHECK(b.said_once(7));
    CHECK_FALSE(b.said_once(8));
    CHECK(b.topic_said(3));
    CHECK(b.talked_to_player(0x20));
    CHECK(b.speaker() == 0); // not saved
    // Cut short or with bytes left over: refused.
    CHECK_FALSE(dialogue::DialogueStates::load(skydot::save::ByteReader(bytes.data(), bytes.size() - 1), b));
    auto longer = bytes;
    longer.push_back(0);
    CHECK_FALSE(dialogue::DialogueStates::load(skydot::save::ByteReader(longer.data(), longer.size()), b));
    // An empty state saves and loads.
    dialogue::DialogueStates empty;
    const auto none = empty.save();
    CHECK(dialogue::DialogueStates::load(skydot::save::ByteReader(none.data(), none.size()), b));
    CHECK_FALSE(b.info_said(7));
}
