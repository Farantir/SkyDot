// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiled Papyrus for the test pack, assembled by hand. The engine's VM tests
// run these, so each comment gives the Papyrus it stands for and the value a
// correct VM returns.
//
// Argument order per opcode follows the vanilla scripts' disassembly
// (`bethconv script --dump`): results first for arithmetic, casts and array
// reads; calls name, object, result, then the arguments.
#pragma once

#include "../../tests/support/pex_builder.hpp"

#include <string>
#include <utility>
#include <vector>

namespace testpack {

using namespace bethconv::testing;

inline constexpr std::uint8_t op_iadd = 0x01, op_fadd = 0x02, op_isub = 0x03, op_fmul = 0x06,
                              op_fdiv = 0x08, op_imod = 0x09, op_fneg = 0x0C, op_assign = 0x0D,
                              op_cast = 0x0E, op_cmp_lt = 0x10, op_cmp_ge = 0x13, op_jmp = 0x14,
                              op_jmpf = 0x16,
                              op_callmethod = 0x17, op_callstatic = 0x19, op_return = 0x1A,
                              op_strcat = 0x1B, op_array_create = 0x1E, op_array_length = 0x1F,
                              op_array_getelement = 0x20, op_array_setelement = 0x21,
                              op_array_findelement = 0x22;

inline PexInstructionSpec ins(std::uint8_t op, std::vector<PexArg> args,
                              std::vector<PexArg> varargs = {}) {
    return {.op = op, .args = std::move(args), .varargs = std::move(varargs)};
}

inline PexFunctionSpec native(std::string name, std::string return_type,
                              std::vector<std::pair<std::string, std::string>> params,
                              bool global = false) {
    PexFunctionSpec f;
    f.name = std::move(name);
    f.return_type = std::move(return_type);
    f.flags = static_cast<std::uint8_t>(0x2 | (global ? 0x1 : 0x0));
    f.params = std::move(params);
    return f;
}

inline PexFunctionSpec function(std::string name, std::string return_type,
                                std::vector<std::pair<std::string, std::string>> params,
                                std::vector<std::pair<std::string, std::string>> locals,
                                std::vector<PexInstructionSpec> code, bool global = false) {
    PexFunctionSpec f;
    f.name = std::move(name);
    f.return_type = std::move(return_type);
    f.flags = global ? 0x1 : 0x0;
    f.params = std::move(params);
    f.locals = std::move(locals);
    f.code = std::move(code);
    return f;
}

inline std::vector<std::byte> one_state(std::string name, std::string parent,
                                        std::vector<PexFunctionSpec> functions,
                                        std::vector<PexVariableSpec> variables = {},
                                        std::vector<PexPropertySpec> properties = {}) {
    auto spec = empty_script(std::move(name), std::move(parent));
    spec.objects[0].variables = std::move(variables);
    spec.objects[0].properties = std::move(properties);
    spec.objects[0].states.push_back({.name = "", .functions = std::move(functions)});
    return build_script(spec);
}

/// Virtual path and bytes of every test script.
inline std::vector<std::pair<std::string, std::vector<std::byte>>> scripts() {
    std::vector<std::pair<std::string, std::vector<std::byte>>> out;

    // The native classes the others use, with the same signatures as the
    // game's.
    out.emplace_back("scripts/form.pex",
                     one_state("Form", "",
                               {native("GetFormID", "Int", {}),
                                native("RegisterForSingleUpdate", "None", {{"afInterval", "Float"}}),
                                native("RegisterForUpdate", "None", {{"afInterval", "Float"}}),
                                native("UnregisterForUpdate", "None", {}),
                                function("OnUpdate", "None", {}, {}, {})}));
    out.emplace_back(
        "scripts/objectreference.pex",
        one_state("ObjectReference", "Form",
                  {native("Enable", "None", {{"abFadeIn", "Bool"}}),
                   native("Disable", "None", {{"abFadeOut", "Bool"}}),
                   native("IsDisabled", "Bool", {}),
                   native("GetLinkedRef", "ObjectReference", {{"apKeyword", "Keyword"}}),
                   native("PlayAnimation", "Bool", {{"asAnimation", "String"}}),
                   native("PlayAnimationAndWait", "Bool",
                          {{"asAnimation", "String"}, {"asEventName", "String"}}),
                   native("BlockActivation", "None", {{"abBlocked", "Bool"}}),
                   native("IsActivationBlocked", "Bool", {}),
                   native("Lock", "None", {{"abLock", "Bool"}, {"abAsOwner", "Bool"}}),
                   native("IsLocked", "Bool", {}),
                   native("GetOpenState", "Int", {}),
                   native("SetOpen", "None", {{"abOpen", "Bool"}}),
                   native("GetTriggerObjectCount", "Int", {}),
                   function("OnActivate", "None", {{"akActionRef", "ObjectReference"}}, {}, {}),
                   function("OnTriggerEnter", "None", {{"akActionRef", "ObjectReference"}}, {}, {}),
                   function("OnTriggerLeave", "None", {{"akActionRef", "ObjectReference"}}, {}, {})}));
    out.emplace_back("scripts/debug.pex",
                     one_state("Debug", "",
                               {native("Trace", "None",
                                       {{"asTextToPrint", "String"}, {"aiSeverity", "Int"}},
                                       true)}));
    out.emplace_back("scripts/utility.pex",
                     one_state("Utility", "",
                               {native("Wait", "None", {{"afSeconds", "Float"}}, true),
                                native("RandomInt", "Int", {{"aiMin", "Int"}, {"aiMax", "Int"}}, true)}));

    // Scriptname TestpackLeverScript extends ObjectReference
    // ObjectReference Property Target Auto
    // Int Pulls
    // Event OnActivate(ObjectReference akActionRef)
    //     Pulls += 1
    //     Debug.Trace("lever pulled " + Pulls)
    //     if Target.IsDisabled()
    //         Target.Enable()
    //     else
    //         Target.Disable()
    //     endif
    //     PlayAnimation("Open")
    // EndEvent
    out.emplace_back(
        "scripts/testpackleverscript.pex",
        one_state(
            "TestpackLeverScript", "ObjectReference",
            {function("OnActivate", "None", {{"akActionRef", "ObjectReference"}},
                      {{"::temp0", "Int"},
                       {"::temp1", "String"},
                       {"::temp2", "String"},
                       {"::temp3", "Bool"},
                       {"::temp4", "Bool"},
                       {"::NoneVar", "None"}},
                      {
                          ins(op_iadd, {ident("::temp0"), ident("Pulls"), integer(1)}),
                          ins(op_assign, {ident("Pulls"), ident("::temp0")}),
                          ins(op_cast, {ident("::temp1"), ident("Pulls")}),
                          ins(op_strcat, {ident("::temp2"), str("lever pulled "), ident("::temp1")}),
                          ins(op_callstatic, {ident("Debug"), ident("Trace"), ident("::NoneVar")},
                              {ident("::temp2"), integer(0)}),
                          ins(op_callmethod,
                              {ident("IsDisabled"), ident("::Target_var"), ident("::temp3")}),
                          ins(op_jmpf, {ident("::temp3"), integer(3)}),
                          ins(op_callmethod,
                              {ident("Enable"), ident("::Target_var"), ident("::NoneVar")},
                              {boolean(false)}),
                          ins(op_jmp, {integer(2)}),
                          ins(op_callmethod,
                              {ident("Disable"), ident("::Target_var"), ident("::NoneVar")},
                              {boolean(false)}),
                          ins(op_callmethod, {ident("PlayAnimation"), ident("self"), ident("::temp4")},
                              {str("Open")}),
                      })},
            {{.name = "::Target_var", .type = "ObjectReference", .initial = none()},
             {.name = "Pulls", .type = "Int", .initial = integer(0)}},
            {{.name = "Target", .type = "ObjectReference", .auto_var = "::Target_var"}}));

    // Scriptname TestpackVmScript: global functions covering the opcodes.
    out.emplace_back(
        "scripts/testpackvmscript.pex",
        one_state(
            "TestpackVmScript", "",
            {
                // Int Function Fib(Int n) global
                //     if n < 2
                //         return n
                //     endif
                //     return Fib(n - 1) + Fib(n - 2)
                // Fib(10) == 55
                function("Fib", "Int", {{"n", "Int"}},
                         {{"::temp0", "Bool"}, {"::temp1", "Int"}, {"::temp2", "Int"},
                          {"::temp3", "Int"}, {"::temp4", "Int"}},
                         {
                             ins(op_cmp_lt, {ident("::temp0"), ident("n"), integer(2)}),
                             ins(op_jmpf, {ident("::temp0"), integer(2)}),
                             ins(op_return, {ident("n")}),
                             ins(op_isub, {ident("::temp1"), ident("n"), integer(1)}),
                             ins(op_callstatic,
                                 {ident("TestpackVmScript"), ident("Fib"), ident("::temp2")},
                                 {ident("::temp1")}),
                             ins(op_isub, {ident("::temp1"), ident("n"), integer(2)}),
                             ins(op_callstatic,
                                 {ident("TestpackVmScript"), ident("Fib"), ident("::temp3")},
                                 {ident("::temp1")}),
                             ins(op_iadd, {ident("::temp4"), ident("::temp2"), ident("::temp3")}),
                             ins(op_return, {ident("::temp4")}),
                         },
                         true),
                // Float Function Mix(Int a, Float b) global
                //     return (a as Float) * b / 2.0 + -b + (a % 3) as Float
                // Mix(5, 3.0) == 6.5
                function("Mix", "Float", {{"a", "Int"}, {"b", "Float"}},
                         {{"::f0", "Float"}, {"::f1", "Float"}, {"::f2", "Float"}, {"::i0", "Int"}},
                         {
                             ins(op_cast, {ident("::f0"), ident("a")}),
                             ins(op_fmul, {ident("::f0"), ident("::f0"), ident("b")}),
                             ins(op_fdiv, {ident("::f0"), ident("::f0"), floating(2.0F)}),
                             ins(op_fneg, {ident("::f1"), ident("b")}),
                             ins(op_fadd, {ident("::f0"), ident("::f0"), ident("::f1")}),
                             ins(op_imod, {ident("::i0"), ident("a"), integer(3)}),
                             ins(op_cast, {ident("::f2"), ident("::i0")}),
                             ins(op_fadd, {ident("::f0"), ident("::f0"), ident("::f2")}),
                             ins(op_return, {ident("::f0")}),
                         },
                         true),
                // String Function Join(Int count) global
                //     String[] parts = new String[3]
                //     parts[0] = "a"; parts[1] = "b"; parts[2] = "c"
                //     String out = ""
                //     Int i = 0
                //     while i < parts.Length && i < count
                //         out += parts[i]
                //         i += 1
                //     endwhile
                //     return out + parts.Find("b")
                // Join(2) == "ab1", Join(5) == "abc1"
                function("Join", "String", {{"count", "Int"}},
                         {{"parts", "String[]"}, {"out", "String"}, {"i", "Int"}, {"::b", "Bool"},
                          {"::len", "Int"}, {"::s", "String"}, {"::idx", "Int"}, {"::t", "String"}},
                         {
                             ins(op_array_create, {ident("parts"), integer(3)}),
                             ins(op_array_setelement, {ident("parts"), integer(0), str("a")}),
                             ins(op_array_setelement, {ident("parts"), integer(1), str("b")}),
                             ins(op_array_setelement, {ident("parts"), integer(2), str("c")}),
                             ins(op_assign, {ident("out"), str("")}),
                             ins(op_assign, {ident("i"), integer(0)}),
                             ins(op_array_length, {ident("::len"), ident("parts")}),
                             ins(op_cmp_lt, {ident("::b"), ident("i"), ident("::len")}),
                             ins(op_jmpf, {ident("::b"), integer(7)}),
                             ins(op_cmp_lt, {ident("::b"), ident("i"), ident("count")}),
                             ins(op_jmpf, {ident("::b"), integer(5)}),
                             ins(op_array_getelement, {ident("::s"), ident("parts"), ident("i")}),
                             ins(op_strcat, {ident("out"), ident("out"), ident("::s")}),
                             ins(op_iadd, {ident("i"), ident("i"), integer(1)}),
                             ins(op_jmp, {integer(-8)}),
                             ins(op_array_findelement,
                                 {ident("parts"), ident("::idx"), str("b"), integer(0)}),
                             ins(op_cast, {ident("::t"), ident("::idx")}),
                             ins(op_strcat, {ident("out"), ident("out"), ident("::t")}),
                             ins(op_return, {ident("out")}),
                         },
                         true),
                // Int Function Waits() global
                //     Utility.Wait(0.5)
                //     return 42
                function("Waits", "Int", {}, {{"::NoneVar", "None"}},
                         {
                             ins(op_callstatic, {ident("Utility"), ident("Wait"), ident("::NoneVar")},
                                 {floating(0.5F)}),
                             ins(op_return, {integer(42)}),
                         },
                         true),
            }));

    // Scriptname TestpackTriggerScript extends ObjectReference
    // Int Entered
    // Event OnTriggerEnter(ObjectReference akActionRef)
    //     Entered += 1
    //     Debug.Trace("inside " + GetTriggerObjectCount())
    // EndEvent
    // Event OnTriggerLeave(ObjectReference akActionRef)
    //     Debug.Trace("left")
    // EndEvent
    out.emplace_back(
        "scripts/testpacktriggerscript.pex",
        one_state(
            "TestpackTriggerScript", "ObjectReference",
            {function("OnTriggerEnter", "None", {{"akActionRef", "ObjectReference"}},
                      {{"::temp0", "Int"}, {"::temp1", "Int"}, {"::temp2", "String"},
                       {"::NoneVar", "None"}},
                      {
                          ins(op_iadd, {ident("::temp0"), ident("Entered"), integer(1)}),
                          ins(op_assign, {ident("Entered"), ident("::temp0")}),
                          ins(op_callmethod,
                              {ident("GetTriggerObjectCount"), ident("self"), ident("::temp1")}),
                          ins(op_cast, {ident("::temp2"), ident("::temp1")}),
                          ins(op_strcat, {ident("::temp2"), str("inside "), ident("::temp2")}),
                          ins(op_callstatic, {ident("Debug"), ident("Trace"), ident("::NoneVar")},
                              {ident("::temp2"), integer(0)}),
                      }),
             function("OnTriggerLeave", "None", {{"akActionRef", "ObjectReference"}},
                      {{"::NoneVar", "None"}},
                      {ins(op_callstatic, {ident("Debug"), ident("Trace"), ident("::NoneVar")},
                           {str("left"), integer(0)})})},
            {{.name = "Entered", .type = "Int", .initial = integer(0)}}));

    // Scriptname TestpackTimerScript extends ObjectReference
    // Int Ticks
    // Function Start()
    //     RegisterForUpdate(0.5)
    // EndFunction
    // Event OnUpdate()                 ; stops itself after three
    //     Ticks += 1
    //     if Ticks >= 3
    //         UnregisterForUpdate()
    //     endif
    // EndEvent
    // Int Function Roll()              ; 1 to 6
    //     return Utility.RandomInt(1, 6)
    // Bool Function Animate()          ; waits for the "Done" event
    //     return PlayAnimationAndWait("Open", "Done")
    // Function Guard()
    //     BlockActivation(true)
    //     Lock(true, false)
    // EndFunction
    out.emplace_back(
        "scripts/testpacktimerscript.pex",
        one_state(
            "TestpackTimerScript", "ObjectReference",
            {
                function("Start", "None", {}, {{"::NoneVar", "None"}},
                         {ins(op_callmethod, {ident("RegisterForUpdate"), ident("self"), ident("::NoneVar")},
                              {floating(0.5F)})}),
                function("OnUpdate", "None", {}, {{"::temp0", "Int"}, {"::b", "Bool"}, {"::NoneVar", "None"}},
                         {
                             ins(op_iadd, {ident("::temp0"), ident("Ticks"), integer(1)}),
                             ins(op_assign, {ident("Ticks"), ident("::temp0")}),
                             ins(op_cmp_ge, {ident("::b"), ident("Ticks"), integer(3)}),
                             ins(op_jmpf, {ident("::b"), integer(2)}),
                             ins(op_callmethod,
                                 {ident("UnregisterForUpdate"), ident("self"), ident("::NoneVar")}),
                         }),
                function("Roll", "Int", {}, {{"::temp0", "Int"}},
                         {ins(op_callstatic, {ident("Utility"), ident("RandomInt"), ident("::temp0")},
                              {integer(1), integer(6)}),
                          ins(op_return, {ident("::temp0")})}),
                function("Animate", "Bool", {}, {{"::temp0", "Bool"}},
                         {ins(op_callmethod, {ident("PlayAnimationAndWait"), ident("self"), ident("::temp0")},
                              {str("Open"), str("Done")}),
                          ins(op_return, {ident("::temp0")})}),
                function("Guard", "None", {}, {{"::NoneVar", "None"}},
                         {ins(op_callmethod, {ident("BlockActivation"), ident("self"), ident("::NoneVar")},
                              {boolean(true)}),
                          ins(op_callmethod, {ident("Lock"), ident("self"), ident("::NoneVar")},
                              {boolean(true), boolean(false)})}),
            },
            {{.name = "Ticks", .type = "Int", .initial = integer(0)}}));

    // Quests: the native classes, then a quest script, its stage fragments
    // and an alias script.
    out.emplace_back(
        "scripts/quest.pex",
        one_state("Quest", "Form",
                  {native("Start", "Bool", {}), native("Stop", "None", {}),
                   native("SetStage", "Bool", {{"aiStage", "Int"}}),
                   native("GetStage", "Int", {}),
                   native("GetStageDone", "Bool", {{"aiStage", "Int"}}),
                   native("IsRunning", "Bool", {}), native("IsCompleted", "Bool", {}),
                   native("GetAlias", "Alias", {{"aiAliasID", "Int"}}),
                   native("SetObjectiveDisplayed", "None",
                          {{"aiObjective", "Int"}, {"abDisplayed", "Bool"}, {"abForce", "Bool"}}),
                   native("SetObjectiveCompleted", "None",
                          {{"aiObjective", "Int"}, {"abCompleted", "Bool"}}),
                   native("IsObjectiveDisplayed", "Bool", {{"aiObjective", "Int"}}),
                   native("IsObjectiveCompleted", "Bool", {{"aiObjective", "Int"}})}));
    out.emplace_back("scripts/alias.pex",
                     one_state("Alias", "", {native("GetOwningQuest", "Quest", {})}));
    out.emplace_back(
        "scripts/referencealias.pex",
        one_state("ReferenceAlias", "Alias",
                  {native("GetReference", "ObjectReference", {}),
                   native("ForceRefTo", "None", {{"akNewRef", "ObjectReference"}}),
                   native("Clear", "None", {}),
                   function("OnActivate", "None", {{"akActionRef", "ObjectReference"}}, {}, {})}));
    out.emplace_back("scripts/globalvariable.pex",
                     one_state("GlobalVariable", "Form",
                               {native("GetValue", "Float", {}),
                                native("SetValue", "None", {{"afNewValue", "Float"}})}));

    // Scriptname TestpackQuestScript extends Quest
    // GlobalVariable Property Counter Auto
    // ReferenceAlias Property Lever Auto      ; alias 0 of this quest
    // Event OnInit()
    //     Debug.Trace("quest init")
    // EndEvent
    // ObjectReference Function LeverRef()    ; the lever's reference once started
    //     return Lever.GetReference()
    // EndFunction
    out.emplace_back(
        "scripts/testpackquestscript.pex",
        one_state(
            "TestpackQuestScript", "Quest",
            {function("OnInit", "None", {}, {{"::NoneVar", "None"}},
                      {ins(op_callstatic, {ident("Debug"), ident("Trace"), ident("::NoneVar")},
                           {str("quest init"), integer(0)})}),
             function("LeverRef", "ObjectReference", {}, {{"::temp0", "ObjectReference"}},
                      {ins(op_callmethod,
                           {ident("GetReference"), ident("::Lever_var"), ident("::temp0")}),
                       ins(op_return, {ident("::temp0")})})},
            {{.name = "::Counter_var", .type = "GlobalVariable", .initial = none()},
             {.name = "::Lever_var", .type = "ReferenceAlias", .initial = none()}},
            {{.name = "Counter", .type = "GlobalVariable", .auto_var = "::Counter_var"},
             {.name = "Lever", .type = "ReferenceAlias", .auto_var = "::Lever_var"}}));

    // Scriptname QF_TestpackQuest_00000150 extends Quest
    // GlobalVariable Property Counter Auto
    // Function Fragment_0()      ; stage 10
    //     SetObjectiveDisplayed(10, true, false)
    // EndFunction
    // Function Fragment_1()      ; stage 20
    //     Counter.SetValue(5.0)
    //     SetObjectiveCompleted(10, true)
    // EndFunction
    out.emplace_back(
        "scripts/qf_testpackquest_00000150.pex",
        one_state(
            "QF_TestpackQuest_00000150", "Quest",
            {function("Fragment_0", "None", {}, {{"::NoneVar", "None"}},
                      {ins(op_callmethod,
                           {ident("SetObjectiveDisplayed"), ident("self"), ident("::NoneVar")},
                           {integer(10), boolean(true), boolean(false)})}),
             function("Fragment_1", "None", {}, {{"::NoneVar", "None"}},
                      {ins(op_callmethod,
                           {ident("SetValue"), ident("::Counter_var"), ident("::NoneVar")},
                           {floating(5.0F)}),
                       ins(op_callmethod,
                           {ident("SetObjectiveCompleted"), ident("self"), ident("::NoneVar")},
                           {integer(10), boolean(true)})})},
            {{.name = "::Counter_var", .type = "GlobalVariable", .initial = none()}},
            {{.name = "Counter", .type = "GlobalVariable", .auto_var = "::Counter_var"}}));

    // Scriptname TestpackAliasScript extends ReferenceAlias
    // Int Activations
    // Event OnActivate(ObjectReference akActionRef)
    //     Activations += 1
    //     GetOwningQuest().SetStage(20)
    // EndEvent
    out.emplace_back(
        "scripts/testpackaliasscript.pex",
        one_state(
            "TestpackAliasScript", "ReferenceAlias",
            {function("OnActivate", "None", {{"akActionRef", "ObjectReference"}},
                      {{"::temp0", "Int"}, {"::quest", "Quest"}, {"::b", "Bool"}},
                      {ins(op_iadd, {ident("::temp0"), ident("Activations"), integer(1)}),
                       ins(op_assign, {ident("Activations"), ident("::temp0")}),
                       ins(op_callmethod,
                           {ident("GetOwningQuest"), ident("self"), ident("::quest")}),
                       ins(op_callmethod, {ident("SetStage"), ident("::quest"), ident("::b")},
                           {integer(20)})})},
            {{.name = "Activations", .type = "Int", .initial = integer(0)}}));

    // Scriptname TestpackStateScript
    // Int Function Ping()           -> 0
    // Function GoBusy()             -> GotoState("Busy")
    // Auto State Idle: Ping() -> 1
    // State Busy: Ping() -> 2; Event OnBeginState() Debug.Trace("busy")
    // plus GotoState and GetState as the compiler generates them.
    {
        auto spec = empty_script("TestpackStateScript", "");
        auto& o = spec.objects[0];
        o.auto_state = "Idle";
        o.states.push_back(
            {.name = "",
             .functions = {
                 function("Ping", "Int", {}, {}, {ins(op_return, {integer(0)})}),
                 function("GoBusy", "None", {}, {{"::NoneVar", "None"}},
                          {ins(op_callmethod, {ident("GotoState"), ident("self"), ident("::NoneVar")},
                               {str("Busy")})}),
                 function("GotoState", "None", {{"newState", "String"}}, {{"::NoneVar", "None"}},
                          {ins(op_callmethod, {ident("onEndState"), ident("self"), ident("::NoneVar")}),
                           ins(op_assign, {ident("::State"), ident("newState")}),
                           ins(op_callmethod,
                               {ident("onBeginState"), ident("self"), ident("::NoneVar")})}),
                 function("GetState", "String", {}, {}, {ins(op_return, {ident("::State")})}),
             }});
        o.states.push_back({.name = "Idle",
                            .functions = {function("Ping", "Int", {}, {}, {ins(op_return, {integer(1)})})}});
        o.states.push_back(
            {.name = "Busy",
             .functions = {function("Ping", "Int", {}, {}, {ins(op_return, {integer(2)})}),
                           function("OnBeginState", "None", {}, {{"::NoneVar", "None"}},
                                    {ins(op_callstatic,
                                         {ident("Debug"), ident("Trace"), ident("::NoneVar")},
                                         {str("busy"), integer(0)})})}});
        out.emplace_back("scripts/testpackstatescript.pex", build_script(spec));
    }
    return out;
}

} // namespace testpack
