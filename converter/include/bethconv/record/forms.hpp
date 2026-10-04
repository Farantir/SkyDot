// SPDX-License-Identifier: GPL-3.0-or-later
//
// Record definitions for placing a city: STAT, DOOR and LIGH (base objects),
// CELL and WRLD (where they sit) and REFR (the placement).
//
// Rules for all definitions:
//
//   1. Field sizes are measured with `bethconv records --field-sizes <TYPE>`
//      over vanilla Skyrim.esm and DLC; counts are noted where the wiki is
//      wrong (CELL DATA is 2 bytes 17,237 times and 1 byte 331 times).
//   2. Unknown fields go to the FieldTally; the record still parses.
//   3. FormIDs stay plugin-local here. Remapping happens once, in the merge.
//
// Source: <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format>, checked
// against xEdit.
#pragma once

#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/plugin.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bethconv::record {

/// STAT: a static object. 9,720 in Skyrim.esm.
struct Static {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;

    /// DNAM: max terrain-conform angle in degrees and a MATO FormID.
    ///
    /// 8 bytes in all 9,720 Skyrim.esm STATs, but 12 in later files (all of
    /// Update.esm, `_ResourcePack.esl` and the CC masters; 711 of 12,626 in SE).
    /// The extra 4 bytes are undocumented and kept raw.
    float max_angle{};
    FormId material;
    std::vector<std::byte> dnam_extra;

    /// MNAM, 1,040 bytes on all 824 that have it: four fixed 260-char
    /// (MAX_PATH) LOD model paths, present even when empty.
    std::array<std::string, 4> lod_models;

    static constexpr std::size_t k_lod_path_size = 260;
    static constexpr std::size_t k_lod_count = 4;
};

/// DOOR.
struct Door {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;

    /// FNAM, 1 byte on all 244.
    std::uint8_t flags{};

    FormId open_sound;   ///< SNAM
    FormId close_sound;  ///< ANAM
    FormId loop_sound;   ///< BNAM
    ScriptData scripts;  ///< VMAD, on 5 vanilla doors.

    /// Source: UESP, DOOR record.
    enum class Flag : std::uint8_t {
        oblivion_gate = 0x01,
        automatic = 0x02,
        hidden = 0x04,
        minimal_use = 0x08,
        sliding = 0x10,
        do_not_open_in_combat = 0x20,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint8_t>(f)) != 0;
    }
};

/// LIGH. Values are kept as stored; mapping to Godot lights happens in the
/// engine.
struct Light {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;

    /// DATA, 48 bytes on all 435. Field order per UESP.
    std::int32_t time{};
    std::uint32_t radius{};
    std::uint32_t colour{};   ///< RGBA, one byte each.
    std::uint32_t light_flags{};
    float falloff_exponent{};
    float fov{};
    float near_clip{};
    float flicker_period{};
    float flicker_intensity_amplitude{};
    float flicker_movement_amplitude{};
    std::uint32_t value{};
    float weight{};

    float fade{}; ///< FNAM, 4 bytes on all 435.

    /// Source: UESP, LIGH record, DATA flags.
    ScriptData scripts; ///< VMAD, on 2 vanilla lights.

    enum class Flag : std::uint32_t {
        dynamic = 0x0001,
        can_be_carried = 0x0002,
        negative = 0x0004,
        flicker = 0x0008,
        off_by_default = 0x0020,
        flicker_slow = 0x0040,
        pulse = 0x0080,
        pulse_slow = 0x0100,
        spot_light = 0x0400,
        spot_shadow = 0x0800,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (light_flags & static_cast<std::uint32_t>(f)) != 0;
    }
};

/// CELL: an interior, or one 4,096-unit square of a worldspace.
struct Cell {
    std::string editor_id;
    LString name;

    /// DATA: a uint16, but 1 byte in 331 of 17,568 vanilla cells (the high
    /// byte omitted when zero). Both widths are accepted.
    std::uint16_t flags{};

    /// XCLC, 12 bytes on all 16,978 exterior cells: grid X, grid Y, flags.
    /// Absent on interiors, which is how they are told apart.
    struct Grid {
        std::int32_t x{};
        std::int32_t y{};
        std::uint32_t flags{};
    };
    std::optional<Grid> grid;

    float water_height{};        ///< XCLW, 4 bytes on all 17,568.
    FormId lighting_template;    ///< LTMP, 4 bytes on all 17,568.
    FormId location;             ///< XLCN
    FormId water;                ///< XCWT
    FormId image_space;          ///< XCIM
    FormId music;                ///< XCMO
    FormId acoustic_space;       ///< XCAS
    FormId encounter_zone;       ///< XEZN
    FormId owner;                ///< XOWN
    FormId sky_lighting;         ///< LNAM/XNAM pair; XNAM is a 1-byte water noise index.
    std::vector<FormId> regions; ///< XCLR, an array of REGN FormIDs.

    /// XCLL, 92 bytes 589 times and 64 bytes once (the older, shorter lighting
    /// struct). Kept raw; the length tells the layouts apart.
    std::vector<std::byte> lighting;

    /// Source: UESP, CELL record, DATA flags.
    enum class Flag : std::uint16_t {
        interior = 0x0001,
        has_water = 0x0002,
        cant_travel_from_here = 0x0004, ///< Interiors only.
        no_lod_water = 0x0008,
        public_area = 0x0020,
        hand_changed = 0x0040,
        show_sky = 0x0080,
        use_sky_lighting = 0x0100,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint16_t>(f)) != 0;
    }
    [[nodiscard]] bool is_interior() const noexcept { return has(Flag::interior); }
};

/// WRLD: a worldspace. 37 in Skyrim.esm, including Tamriel.
struct Worldspace {
    std::string editor_id;
    LString name;

    std::uint8_t flags{};         ///< DATA, 1 byte on all 37.
    FormId parent;                ///< WNAM
    std::uint16_t parent_flags{}; ///< PNAM, 2 bytes: which parent properties are used.
    FormId climate;               ///< CNAM
    FormId water;                 ///< NAM2
    FormId lod_water_type;        ///< NAM3
    float lod_water_height{};     ///< NAM4
    FormId location;              ///< XLCN
    FormId lighting_template;     ///< LTMP
    FormId encounter_zone;        ///< XEZN
    FormId music;                 ///< ZNAM

    /// DNAM, 8 bytes on all 30 that have it: default land and water height.
    std::optional<float> default_land_height;
    std::optional<float> default_water_height;

    /// WCTR, 4 bytes on all 19 that have it: two int16 cell coordinates (not
    /// floats, despite the wiki).
    std::optional<std::pair<std::int16_t, std::int16_t>> centre_cell;

    /// NAM0 / NAM9, 8 bytes each on all 37: min and max corner in game units.
    float min_x{};
    float min_y{};
    float max_x{};
    float max_y{};

    /// OFST (offset table) and RNAM (per-cell reference index) are regenerated
    /// by the game and unused here; they are tallied, not stored.

    /// Source: UESP, WRLD record, DATA flags.
    enum class Flag : std::uint8_t {
        small_world = 0x01,
        cant_fast_travel = 0x02,
        no_lod_water = 0x08,
        no_landscape = 0x10,
        no_sky = 0x20,
        fixed_dimensions = 0x40,
        no_grass = 0x80,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint8_t>(f)) != 0;
    }
};

/// REFR: one placed instance of a base object. 693,333 in Skyrim.esm.
struct Reference {
    std::string editor_id;

    /// NAME, 4 bytes on all 693,333: the placed base record.
    FormId base;

    /// DATA, 24 bytes on all 693,333: position and rotation (radians, X/Y/Z),
    /// three floats each.
    Vec3 position;
    Vec3 rotation;

    /// XSCL, 4 bytes on 199,070. Absent means 1.0.
    float scale{1.0F};

    /// XESP, 8 bytes on 20,852: the enable parent and whether its state is
    /// inverted.
    struct EnableParent {
        FormId parent;
        std::uint32_t flags{};

        /// Source: UESP, REFR record, XESP.
        [[nodiscard]] bool set_enable_state_opposite() const noexcept {
            return (flags & 0x01u) != 0;
        }
        [[nodiscard]] bool pop_in() const noexcept { return (flags & 0x02u) != 0; }
    };
    std::optional<EnableParent> enable_parent;

    /// XTEL, 32 bytes on all 1,722: destination door and arrival position.
    struct Teleport {
        FormId destination_door;
        Vec3 position;
        Vec3 rotation;
        std::uint32_t flags{};
    };
    std::optional<Teleport> teleport;

    /// XPRM, 32 bytes on all 13,668: primitive volume (bounds, RGB color, an
    /// unknown float, type). Used for triggers, occlusion planes and room
    /// markers.
    struct Primitive {
        Vec3 bounds;
        float red{};
        float green{};
        float blue{};
        float unknown{};
        std::uint32_t type{};
    };
    std::optional<Primitive> primitive;

    FormId owner;            ///< XOWN
    FormId emittance;        ///< XEMI, the light source whose colour this takes.
    FormId light_ref;        ///< XLRT / XLRM room-and-light markers use these too.
    float radius{};          ///< XRDS, 4 bytes on 11,148.
    bool has_radius{};       ///< XRDS present (0 is a value).
    std::int32_t count{};    ///< XLCM, a levelled-list or stack count.

    /// XLKR, 8 bytes 12,467 times and 4 bytes 10 times (keyword omitted).
    /// Both are accepted; the short form leaves `keyword` null.
    struct LinkedReference {
        FormId keyword;
        FormId target;
    };
    std::vector<LinkedReference> linked_references;

    /// XLIG, 16 bytes 10,102 times and 20 bytes once. Kept raw; the last four
    /// bytes are undocumented.
    std::vector<std::byte> light_data;

    /// VMAD: scripts on this reference, on top of the base's.
    ScriptData scripts;

    /// XAPR, 8 bytes on all 3,451 (one field per parent): activating a
    /// parent activates this reference after `delay` seconds.
    struct ActivateParent {
        FormId ref;
        float delay{};
    };
    std::vector<ActivateParent> activate_parents;
    /// XAPD, 1 byte on all 3,040. Bit 0: only the parents can activate it.
    std::uint8_t activate_parent_flags{};

    /// XLOC, 20 bytes on all 1,277 in Skyrim.esm (SE): lock level (0-100,
    /// 255 needs a key), three padding bytes, key, flags, three padding bytes,
    /// eight unknown. Source: UESP, REFR record, XLOC.
    struct Lock {
        std::uint8_t level{};
        FormId key;
        std::uint8_t flags{}; ///< 0x04: levelled lock.
        std::array<std::uint32_t, 2> unknown{};
    };
    std::optional<Lock> lock;

    /// XRGD/XRGB (ragdoll state saved by the Creation Kit) and editor-only
    /// fields are tallied, not stored.

    /// Placement-relevant header flags, copied so consumers need no header.
    bool initially_disabled{};
    bool persistent{};
    bool deleted{};
};

// ---- parsing --------------------------------------------------------------
// Each takes the (already inflated) record payload and the plugin's
// FormContext.

[[nodiscard]] io::ParseResult<Static> parse_static(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Door> parse_door(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Light> parse_light(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Cell> parse_cell(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Worldspace> parse_worldspace(io::SpanReader& data,
                                                           const FormContext& ctx);
/// REFR also needs the header: "initially disabled" and "persistent" are header
/// flags.
[[nodiscard]] io::ParseResult<Reference> parse_reference(const RecordHeader& header,
                                                         io::SpanReader& data,
                                                         const FormContext& ctx);

/// All record types with a definition (across all forms_*.hpp), for the census
/// and the CLI.
[[nodiscard]] std::span<const FourCC> defined_types() noexcept;

[[nodiscard]] bool is_defined_type(FourCC type) noexcept;

} // namespace bethconv::record
