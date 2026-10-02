// SPDX-License-Identifier: GPL-3.0-or-later
//
// CTDA conditions, decoded, and the table of condition functions.
//
// A CTDA is 32 bytes on every vanilla record that has one. Its parameters are
// raw 32-bit words whose meaning depends on the function, so the table records
// what kind each parameter is; FormID parameters can then be made global.
//
// Source: xEdit's wbDefinitionsTES5.pas (wbConditions, wbConditionFunctions,
// the parameter deciders) and wbDefinitionsCommon.pas (condition type flags).
#pragma once

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::record {

/// One CTDA with its CIS1/CIS2 strings.
struct Condition {
    /// Byte 0: the comparison in the top three bits (0 ==, 1 !=, 2 >, 3 >=,
    /// 4 <, 5 <=), then flags: 0x01 OR with the next, 0x02 use aliases, 0x04
    /// the comparison value is a GLOB, 0x08 use package data, 0x10 swap
    /// subject and target.
    std::uint8_t type{};
    float value{};          ///< Comparison value, unless `value_global`.
    FormId value_global;    ///< The comparison GLOB, with flag 0x04.
    std::uint16_t function{};
    std::uint32_t param1{}; ///< Raw; see `ConditionParam`.
    std::uint32_t param2{};
    /// 0 subject, 1 target, 2 reference, 3 combat target, 4 linked reference,
    /// 5 quest alias, 6 package data, 7 event data.
    std::uint32_t run_on{};
    FormId reference;       ///< When `run_on` is 2.
    std::int32_t param3{-1};///< An alias or event member, depending on `run_on`.
    std::string string1;    ///< CIS1
    std::string string2;    ///< CIS2

    static constexpr std::size_t k_size = 32;
    static constexpr std::uint8_t k_or = 0x01;
    static constexpr std::uint8_t k_use_aliases = 0x02;
    static constexpr std::uint8_t k_use_global = 0x04;
    static constexpr std::uint8_t k_use_package_data = 0x08;
    static constexpr std::uint8_t k_swap = 0x10;
    static constexpr std::uint32_t k_run_on_reference = 2;

    [[nodiscard]] std::uint8_t comparison() const noexcept {
        return static_cast<std::uint8_t>(type >> 5);
    }
};

/// What a condition parameter holds, from xEdit's parameter types. Enums,
/// actor values and the like are all `integer`.
enum class ConditionParam : std::uint8_t {
    none,
    integer,
    floating,
    string,       ///< Index of a CIS string; the text is in CIS1/CIS2.
    alias,        ///< A quest alias id.
    event,
    package_data, ///< A package data input key.
    quest_stage,
    reference,    ///< A placed reference (ptReference, ptActor).
    package,      ///< A PACK.
    form,         ///< Any other FormID.
};

struct ConditionFunction {
    std::uint16_t index;
    std::string_view name;
    ConditionParam param1;
    ConditionParam param2;
};

/// The function with this index, or null for one xEdit does not know.
[[nodiscard]] const ConditionFunction* condition_function(std::uint16_t index) noexcept;

/// What parameter `n` (1 or 2) of `c` holds, after the alias and package-data
/// flags, which turn a reference or package parameter into an alias or a data
/// input key (xEdit's wbConditionParam1Decider). `none` for an unknown
/// function.
[[nodiscard]] ConditionParam condition_param(const Condition& c, int n) noexcept;

/// Whether parameter `n` of `c` is a FormID.
[[nodiscard]] bool condition_param_is_form(const Condition& c, int n) noexcept;

/// CTDA, CIS1, CIS2 and CITC into `out`. Returns false when the field is none
/// of them. CIS strings attach to the last condition; one before any CTDA is
/// a failure. `declared_count` receives CITC.
[[nodiscard]] bool read_ctda_field(std::vector<Condition>& out, std::uint32_t& declared_count,
                                   const FieldHeader& field, io::SpanReader& body,
                                   std::optional<io::ParseError>& failure);

} // namespace bethconv::record
