// SPDX-License-Identifier: GPL-3.0-or-later
//
// Readers for the field shapes that recur across record types: zstring paths,
// localized strings, object bounds, FormID arrays, models, keyword lists and so
// on. A per-type definition is then mostly a list of tags.
//
// Sizes were measured with `bethconv records --field-sizes <TYPE>` over vanilla
// Skyrim.esm + DLC. Source: <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format>,
// checked against xEdit.
#pragma once

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/headers.hpp"
#include "bethconv/record/strings.hpp"
#include "bethconv/record/vmad.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bethconv::record {

struct Vec3 {
    float x{};
    float y{};
    float z{};

    friend constexpr bool operator==(const Vec3&, const Vec3&) noexcept = default;
};

/// OBND: the object's bounding box in game units, six int16. 12 bytes on all
/// 9,720 vanilla STATs and every other type that has it.
struct ObjectBounds {
    std::int16_t x1{};
    std::int16_t y1{};
    std::int16_t z1{};
    std::int16_t x2{};
    std::int16_t y2{};
    std::int16_t z2{};

    friend constexpr bool operator==(const ObjectBounds&, const ObjectBounds&) noexcept = default;
};

/// A string stored in the plugin or in a .STRINGS table.
///
/// In a localized plugin a string field is a 4-byte table index (every vanilla
/// FULL is 4 bytes). Three states once a StringSource is attached: inline text
/// (`is_id` false, `id` 0); a resolved index (`is_id` false, `id` kept for
/// provenance); an unresolved index (`is_id` true).
struct LString {
    std::string text;    ///< Empty when this is an unresolved index.
    std::uint32_t id{};  ///< The STRINGS index, when the field was localized.
    bool is_id{};        ///< The index was not resolved to any text.

    [[nodiscard]] bool empty() const noexcept { return !is_id && text.empty(); }
    /// True if the text came from a string table.
    [[nodiscard]] bool from_table() const noexcept { return !is_id && id != 0; }
    [[nodiscard]] std::string to_string() const;
};

/// Receives what a per-type parser did not understand. Conversion passes
/// nullptr; the census counts.
class FieldTally {
public:
    FieldTally() = default;
    FieldTally(const FieldTally&) = delete;
    FieldTally& operator=(const FieldTally&) = delete;
    virtual ~FieldTally() = default;

    /// A field this record type has no definition for.
    virtual void unhandled(FourCC record, FourCC field, std::uint32_t size) = 0;

    /// A defined field whose payload was longer than the definition read, i.e.
    /// the definition is too short.
    virtual void leftover(FourCC record, FourCC field, std::size_t bytes) = 0;

    /// A localized string index and whether the tables had it. No-op by
    /// default, since it only fires with a StringSource attached.
    virtual void localized_string(FourCC /*record*/, FourCC /*field*/, std::uint32_t /*id*/,
                                  bool /*resolved*/) {}
};

/// Context a per-type parse needs from outside the record itself.
struct FormContext {
    /// From the TES4 header: whether string fields are text or indices.
    bool localized{};

    /// The plugin's own string tables, or null to leave indices unresolved.
    /// Always per plugin: an index only means something in its own file, and
    /// another plugin's tables would silently give wrong names.
    const StringSource* strings{};

    /// Optional; receives unhandled and over-long fields.
    FieldTally* tally{};
};

// ---- field readers --------------------------------------------------------
// Each takes a reader confined to one field's payload.

/// EDID, MODL etc.: a NUL-terminated string filling the field.
[[nodiscard]] io::ParseResult<std::string> read_zstring(io::SpanReader& body);

/// FULL, DESC: text or a STRINGS index, depending on the plugin.
///
/// `kind` selects the table and depends on the field (FULL `plain`, DESC
/// `description`, INFO responses `dialogue`). `field` is only used to report
/// unresolved indices.
[[nodiscard]] io::ParseResult<LString> read_lstring(io::SpanReader& body, const FormContext& ctx,
                                                    FourCC record = FourCC{}, FourCC field = FourCC{},
                                                    StringKind kind = StringKind::plain);

[[nodiscard]] io::ParseResult<ObjectBounds> read_object_bounds(io::SpanReader& body);

[[nodiscard]] io::ParseResult<Vec3> read_vec3(io::SpanReader& body);

[[nodiscard]] io::ParseResult<FormId> read_formid(io::SpanReader& body);

/// KWDA, XCLR, LNAM in FLST: a field of consecutive FormIDs. A trailing partial
/// FormID is an error.
[[nodiscard]] io::ParseResult<std::vector<FormId>> read_formid_array(io::SpanReader& body);

/// Copy a field's payload. A copy, not a span, because compressed records live
/// in a temporary inflate buffer (7,506 of 17,568 vanilla CELLs).
[[nodiscard]] io::ParseResult<std::vector<std::byte>> read_verbatim(io::SpanReader& body);

/// Report bytes the caller left unread in a field it claims to understand.
void note_leftover(const FormContext& ctx, FourCC record, const FieldHeader& field,
                   const io::SpanReader& body);

void note_unhandled(const FormContext& ctx, FourCC record, const FieldHeader& field);

// ---- recurring sub-structures ---------------------------------------------
// Blocks shared by many record types, defined once so fixes apply everywhere.

/// MODL and its companions. MODT (texture hashes) and MODS (alternate textures)
/// are kept raw.
struct ModelData {
    std::string path;                          ///< MODL, a path under meshes/.
    std::vector<std::byte> texture_hashes;     ///< MODT
    std::vector<std::byte> alternate_textures; ///< MODS

    [[nodiscard]] bool empty() const noexcept { return path.empty(); }
};

/// DEST + DSTD/DMDL/DMDT/DMDS/DSTF: destruction stages.
///
/// DSTD opens a stage, the following model fields belong to it, and the
/// zero-length DSTF closes it. DEST is 8 bytes and DSTD 20 on all 793
/// destructible vanilla ACTIs.
struct Destruction {
    std::int32_t health{};      ///< DEST[0]
    std::uint8_t stage_count{}; ///< DEST[4]; the file's own count, kept for checking.
    std::uint8_t flags{};       ///< DEST[5]
    std::uint16_t unknown{};    ///< DEST[6..7]

    struct Stage {
        std::uint8_t health_percent{};        ///< DSTD[0]
        std::uint8_t index{};                 ///< DSTD[1]
        std::uint8_t damage_stage{};          ///< DSTD[2]
        std::uint8_t stage_flags{};           ///< DSTD[3]
        std::int32_t self_damage_per_second{};///< DSTD[4..7]
        FormId explosion;                     ///< DSTD[8..11]
        FormId debris;                        ///< DSTD[12..15]
        std::int32_t debris_count{};          ///< DSTD[16..19]
        ModelData model;                      ///< DMDL/DMDT/DMDS
    };
    std::vector<Stage> stages;

    [[nodiscard]] bool empty() const noexcept { return health == 0 && stages.empty(); }
};

/// KSIZ + KWDA. Count and array are kept separately so a mismatch can be
/// reported.
struct KeywordList {
    std::uint32_t declared_count{}; ///< KSIZ
    std::vector<FormId> keywords;   ///< KWDA, one or more, concatenated.

    [[nodiscard]] bool consistent() const noexcept {
        return declared_count == keywords.size();
    }
};

/// COCT + CNTO + COED: an inventory. COED belongs to the preceding CNTO and is
/// attached to it. CNTO is 8 bytes and COED 12 on all 13,038 vanilla CONT
/// entries.
struct ContainerItem {
    FormId item;
    std::int32_t count{};
    /// COED: owner, a rank-or-global word, item condition. The middle word's
    /// meaning is not recorded in the field, so it is kept as raw bits.
    struct Extra {
        FormId owner;
        std::uint32_t rank_or_global{};
        float condition{};
    };
    std::optional<Extra> extra;
};

/// EFID + EFIT + CTDA: one magic effect entry. Following CTDAs condition this
/// entry. EFIT is 12 bytes on all 2,673 vanilla SPEL entries, CTDA 32 on all
/// 1,253.
struct EffectItem {
    FormId effect;            ///< EFID
    float magnitude{};        ///< EFIT[0]
    std::uint32_t area{};     ///< EFIT[4..7]
    std::uint32_t duration{}; ///< EFIT[8..11]

    /// CTDA, 32 bytes each, kept raw. Parameter meaning depends on the function
    /// index; nothing needs it yet.
    std::vector<std::vector<std::byte>> conditions;
    /// CIS1/CIS2: string parameter of the preceding condition.
    std::vector<std::string> condition_strings;
};

// ---- shared field dispatch ------------------------------------------------

/// Move a ParseResult into `out`, recording only the first failure so the
/// diagnostic points at where things started going wrong.
template <typename T>
void take(std::optional<io::ParseError>& failure, io::ParseResult<T>&& result, T& dst) {
    if (!result) {
        if (!failure) {
            failure = std::move(result).error();
        }
        return;
    }
    dst = std::move(*result);
}

/// Skip `n` padding bytes, recording a failure like `take`.
inline void skip_bytes(std::optional<io::ParseError>& failure, io::SpanReader& body,
                       std::size_t n) {
    if (auto skipped = body.skip(n); !skipped && !failure) {
        failure = std::move(skipped).error();
    }
}

/// Common per-type parse: walk the fields, let `dispatch` claim known ones and
/// tally the rest.
///
/// `dispatch(field, body, failure)` returns whether it claimed the field.
/// Declined fields go to the tally and the record still parses.
template <typename F>
io::ParseResult<void> walk_fields(io::SpanReader& data, FourCC self, const FormContext& ctx,
                                  F&& dispatch) {
    std::optional<io::ParseError> failure;
    const auto walk = for_each_field(data, [&](const FieldHeader& field, io::SpanReader& body) {
        if (failure) {
            return; // Stop at the first bad field.
        }
        if (!dispatch(field, body, failure)) {
            note_unhandled(ctx, self, field);
            return;
        }
        if (!failure) {
            note_leftover(ctx, self, field, body);
        }
    });
    if (!walk) {
        return std::unexpected(walk.error());
    }
    if (failure) {
        return std::unexpected(std::move(*failure));
    }
    return {};
}

/// MODL/MODT/MODS. Returns false when the field is not one of them.
[[nodiscard]] bool read_model_field(ModelData& model, const FieldHeader& field,
                                    io::SpanReader& body,
                                    std::optional<io::ParseError>& failure);

/// The destruction tags. Returns false for any other field.
[[nodiscard]] bool read_destruction_field(Destruction& dest, const FieldHeader& field,
                                          io::SpanReader& body,
                                          std::optional<io::ParseError>& failure);

/// KSIZ/KWDA. Returns false when the field is neither.
[[nodiscard]] bool read_keyword_field(KeywordList& keywords, const FieldHeader& field,
                                      io::SpanReader& body,
                                      std::optional<io::ParseError>& failure);

/// COCT/CNTO/COED. Returns false when the field is none of them.
[[nodiscard]] bool read_container_field(std::vector<ContainerItem>& items,
                                        std::uint32_t& declared_count, const FieldHeader& field,
                                        io::SpanReader& body,
                                        std::optional<io::ParseError>& failure);

/// EFID/EFIT/CTDA/CIS1/CIS2. Returns false when the field is none of them.
[[nodiscard]] bool read_effect_field(std::vector<EffectItem>& effects, const FieldHeader& field,
                                     io::SpanReader& body,
                                     std::optional<io::ParseError>& failure);

/// Destinations for the tags most base objects share. A null member means the
/// type has no such field, so e.g. an unexpected KWDA is tallied instead of
/// read.
struct BaseObjectFields {
    std::string* editor_id{};             ///< EDID
    ObjectBounds* bounds{};               ///< OBND
    ModelData* model{};                   ///< MODL/MODT/MODS
    LString* name{};                      ///< FULL
    KeywordList* keywords{};              ///< KSIZ/KWDA
    Destruction* destruction{};           ///< DEST and its stage ladder
    ScriptData* scripts{};                ///< VMAD
};

/// Read a shared tag into its destination. Returns false if the field is not
/// one of them or its destination is null.
[[nodiscard]] bool read_base_object_field(const BaseObjectFields& into, FourCC self,
                                          const FieldHeader& field, io::SpanReader& body,
                                          const FormContext& ctx,
                                          std::optional<io::ParseError>& failure);

/// Number of `stride`-byte entries in a field; an error if the payload is not a
/// whole multiple.
[[nodiscard]] io::ParseResult<std::size_t> entry_count(const io::SpanReader& body,
                                                       std::size_t stride,
                                                       std::string_view what);

} // namespace bethconv::record
