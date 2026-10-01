// SPDX-License-Identifier: GPL-3.0-or-later
//
// World layers: terrain, navmesh, placed actors, and the region, location,
// climate and weather data that describe a cell, plus the LTEX and IMGS
// lookups they reference.
//
// LAND (335 MiB over 54,079 SE records, nearly all compressed) and NAVM
// (102 MiB over 20,057) are the largest payloads. Their bulk fields are kept
// raw, but their structure is still walked so truncation is an error.
//
// Source: <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format>, checked
// against xEdit.
#pragma once

#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/plugin.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bethconv::record {

/// LTEX: a landscape texture, referenced by terrain texture layers. 130 in SE.
struct LandTexture {
    std::string editor_id;
    FormId texture_set;   ///< TNAM, a TXST.
    FormId material_type; ///< MNAM, a MATT.
    std::uint8_t friction{};    ///< HNAM[0], 2 bytes on all 130.
    std::uint8_t restitution{}; ///< HNAM[1]
    std::uint8_t specular{};    ///< SNAM, 1 byte on all 130.
    std::vector<FormId> grasses; ///< GNAM, one GRAS per occurrence.

    /// INAM, 4 bytes on 42 of 130. Undocumented for Skyrim (an Oblivion-era
    /// texture index); kept as a raw word.
    std::uint32_t inam{};
};

/// IMGS: an image space (color grading). 506 in SE.
struct ImageSpace {
    std::string editor_id;

    /// HNAM, 36 bytes on 504 of 506: nine HDR floats (eye adaptation, bloom
    /// and so on).
    std::vector<std::byte> hdr;
    /// CNAM, 12 bytes: cinematic saturation, brightness, contrast.
    std::vector<std::byte> cinematic;
    /// TNAM, 16 bytes: tint amount and RGB.
    std::vector<std::byte> tint;
    /// DNAM, 16 bytes 486 times and 12 twice: depth of field strength,
    /// distance, range, and a word the short variant lacks. Both accepted.
    std::vector<std::byte> depth_of_field;
    /// ENAM, 56 bytes on two records: the pre-1.70 image space block.
    std::vector<std::byte> legacy;
};

/// CLMT: a climate (weather chances, sun timing). Nine in SE, six in LE.
struct Climate {
    std::string editor_id;
    ModelData model; ///< Night sky star model.

    /// WLST, 12 bytes per entry: WTHR, chance out of 100, optional gating GLOB.
    struct WeatherEntry {
        FormId weather;
        std::int32_t chance{};
        FormId global;
    };
    std::vector<WeatherEntry> weathers;
    static constexpr std::size_t k_weather_entry_size = 12;

    std::string sun_texture;       ///< FNAM
    std::string sun_glare_texture; ///< GNAM

    /// TNAM, 6 bytes on all SE and LE climates: four times of day in 10-minute
    /// units, volatility, and moon phase length with two phase bits in the
    /// high nibble.
    std::uint8_t sunrise_begin{};
    std::uint8_t sunrise_end{};
    std::uint8_t sunset_begin{};
    std::uint8_t sunset_end{};
    std::uint8_t volatility{};
    std::uint8_t moons_and_phase_length{};
};

/// WTHR: a weather. 177 in SE; the type with the most distinct fields.
struct Weather {
    std::string editor_id;
    ModelData model; ///< Aurora.

    /// Cloud layer textures. The tag encodes the layer (`00TX` to `L0TX`, first
    /// byte 0x30 + index), so it is matched by pattern rather than a table
    /// built from the layers vanilla happens to use. 29 slots, all used.
    static constexpr std::size_t k_cloud_layers = 29;
    static constexpr std::uint8_t k_cloud_tag_base = 0x30;
    std::array<std::string, k_cloud_layers> cloud_textures;

    /// DATA, 19 bytes on all 177: wind, transitions, sun glare/damage,
    /// precipitation and thunder timing, flags, lightning color, visual effect
    /// window. Kept raw; UESP and xEdit disagree on names.
    std::vector<std::byte> data;
    static constexpr std::size_t k_data_size = 19;

    /// Color and alpha tables (floats by time of day and layer), kept raw.
    std::vector<std::byte> weather_colours;   ///< NAM0, 272 bytes on 165 of 177.
    std::vector<std::byte> cloud_colours;     ///< PNAM, 512 bytes on 176.
    std::vector<std::byte> cloud_alphas;      ///< JNAM, 512 bytes on 176.
    std::vector<std::byte> cloud_speed_x;     ///< QNAM, 32 bytes on 176.
    std::vector<std::byte> cloud_speed_y;     ///< RNAM, 32 bytes on 176.
    std::vector<std::byte> fog_distance;      ///< FNAM, 32 bytes on all 177.
    /// DALC, 32 bytes 704 times and 24 four times: directional ambient light,
    /// one block per time of day.
    std::vector<std::vector<std::byte>> directional_ambient;

    std::uint32_t cloud_layers_disabled{}; ///< NAM1
    std::uint32_t lnam{};                  ///< LNAM, 4 bytes on all 177; undocumented.
    std::uint32_t onam{};                  ///< ONAM, on one record; undocumented.
    FormId precipitation;                  ///< MNAM, an SPGD.
    FormId visual_effect;                  ///< NNAM, an RFCT.
    std::vector<FormId> sky_statics;       ///< TNAM, one STAT per occurrence.
    std::vector<FormId> volumetric_lighting;///< HNAM, 16 bytes = four FormIDs, on 93.
    std::vector<FormId> image_spaces;      ///< IMSP, 16 bytes = four IMGS.

    /// SNAM, 8 bytes each: SNDR and the weather event that plays it.
    struct SoundEntry {
        FormId sound;
        std::uint32_t type{};
    };
    std::vector<SoundEntry> sounds;
    static constexpr std::size_t k_sound_entry_size = 8;

    /// NAM2 and NAM3, 16 bytes each on eleven records; unused and
    /// undocumented, kept raw.
    std::vector<std::byte> nam2;
    std::vector<std::byte> nam3;

    /// ANAM/BNAM/CNAM/DNAM: texture paths, on a single vanilla record.
    std::string anam;
    std::string bnam;
    std::string cnam;
    std::string dnam;
};

/// SPGD: shader particle geometry, a weather's rain or snow. 16 in SE.
struct ShaderParticleGeometry {
    std::string editor_id;
    std::string texture; ///< ICON, under textures/.

    /// DATA, 48 bytes on 14 and 40 on two (without box size and density):
    /// twelve 4-byte values, not the padded layout xEdit gives. Checked
    /// against all 16 vanilla records.
    float gravity_velocity{};   ///< Units per second, downwards.
    float rotation_velocity{};
    float particle_size_x{};
    float particle_size_y{};
    float center_offset_min{};
    float center_offset_max{};
    float initial_rotation_range{}; ///< Degrees.
    std::uint32_t subtextures_x{};
    std::uint32_t subtextures_y{};
    std::uint32_t type{};        ///< 0 rain, 1 snow.
    std::uint32_t box_size{};    ///< Units; 0 if absent.
    float particle_density{};    ///< 0 if absent.
    static constexpr std::size_t k_data_size = 48;
    static constexpr std::size_t k_short_data_size = 40;
};

/// REGN: a region polygon where grass, sound, weather and map color apply.
/// 389 in SE.
struct Region {
    std::string editor_id;
    std::uint32_t map_colour{}; ///< RCLR, RGBA, 4 bytes on all 389.
    FormId worldspace;          ///< WNAM
    std::string icon;           ///< ICON, on three records.

    /// RPLI + RPLD: edge falloff, then the polygon as (x, y) pairs in world
    /// units. A region can have several.
    struct Area {
        std::uint32_t edge_fall_off{};
        std::vector<std::pair<float, float>> points;
    };
    std::vector<Area> areas;
    static constexpr std::size_t k_point_size = 8;

    /// RDAT opens a data entry; following fields belong to it.
    struct Data {
        std::uint32_t type{};      ///< RDAT[0..3]
        std::uint8_t flags{};      ///< RDAT[4]
        std::uint8_t priority{};   ///< RDAT[5]
        std::uint16_t unknown{};   ///< RDAT[6..7]

        FormId music;              ///< RDMO
        LString map_name;          ///< RDMP

        /// RDSA, 12 bytes per entry: SNDR, flags, chance.
        struct Sound {
            FormId sound;
            std::uint32_t flags{};
            float chance{};
        };
        std::vector<Sound> sounds;

        /// RDWT, 12 bytes per entry: same layout as CLMT's WLST.
        std::vector<Climate::WeatherEntry> weathers;

        /// RDOT is 0 bytes on all 70 vanilla occurrences.
        bool has_objects{};
    };
    std::vector<Data> entries;
    static constexpr std::size_t k_sound_entry_size = 12;
    static constexpr std::size_t k_weather_entry_size = 12;
};

/// LCTN: a location (for quests, map markers, fast travel) and what it
/// contains. 841 in SE.
struct Location {
    std::string editor_id;
    LString name;
    KeywordList keywords;

    FormId parent;                 ///< PNAM
    FormId music;                  ///< NAM1
    FormId unreported_crime_faction; ///< FNAM
    FormId world_location_marker;  ///< MNAM
    float world_location_radius{}; ///< RNAM
    FormId horse_marker;           ///< NAM0
    std::uint32_t colour{};        ///< CNAM, RGBA on 133 records.

    /// A tracked reference and its cell. Six 12-byte field types share this
    /// layout and are kept in separate lists. Grid coordinates are int16 cell
    /// indices.
    struct CellRef {
        FormId a;
        FormId b;
        std::int16_t grid_x{};
        std::int16_t grid_y{};
    };
    static constexpr std::size_t k_cell_ref_size = 12;

    /// LCSR/ACSR: an extra FormID before the grid.
    struct StaticRef {
        FormId a;
        FormId b;
        FormId c;
        std::int16_t grid_x{};
        std::int16_t grid_y{};
    };
    static constexpr std::size_t k_static_ref_size = 16;

    /// LCEC/ACEC/RCEC: one FormID, then any number of cell coordinates.
    struct CellList {
        FormId owner;
        std::vector<std::pair<std::int16_t, std::int16_t>> cells;
    };
    static constexpr std::size_t k_cell_list_header = 4;
    static constexpr std::size_t k_cell_coord_size = 4;

    std::vector<CellRef> actor_cell_persistent;   ///< ACPR
    std::vector<CellRef> location_cell_persistent;///< LCPR
    std::vector<CellRef> actor_cell_encounter;    ///< ACEP
    std::vector<CellRef> location_cell_encounter; ///< LCEP
    std::vector<CellRef> actor_cell_unique;       ///< ACUN
    std::vector<CellRef> location_cell_unique;    ///< LCUN
    std::vector<StaticRef> actor_cell_static;     ///< ACSR
    std::vector<StaticRef> location_cell_static;  ///< LCSR
    std::vector<CellList> actor_cell_marker;      ///< ACEC
    std::vector<CellList> location_cell_marker;   ///< LCEC
    std::vector<CellList> ref_cell_marker;        ///< RCEC
    std::vector<FormId> actor_ids;                ///< ACID
    std::vector<FormId> location_ids;             ///< LCID
    std::vector<FormId> ref_persistent;           ///< RCPR
};

/// LAND: one cell's terrain. 54,079 in SE, 335 MiB; by far the largest type.
struct Landscape {
    std::uint32_t flags{}; ///< DATA, 4 bytes on all 54,079.

    /// Terrain is a 33x33 vertex grid. VNML and VCLR are 3,267 bytes
    /// (33 x 33 x 3) on all 53,896 records that have them; VHGT is 1,096 (float
    /// offset, 33 x 33 int8 deltas, 3 bytes padding).
    static constexpr std::size_t k_grid = 33;
    static constexpr std::size_t k_vnml_size = k_grid * k_grid * 3;
    static constexpr std::size_t k_vhgt_size = 4 + k_grid * k_grid + 3;

    std::vector<std::byte> normals;      ///< VNML
    std::vector<std::byte> heights;      ///< VHGT
    std::vector<std::byte> vertex_colours;///< VCLR, on 9,243 of 54,079.

    /// BTXT is a quadrant's base texture layer, ATXT an additional one followed
    /// by its VTXT opacity map. Both 8 bytes (152,844 ATXT, 49,345 BTXT).
    struct TextureLayer {
        FormId texture;          ///< An LTEX.
        std::uint8_t quadrant{}; ///< 0..3
        std::uint8_t unknown{};
        std::int16_t layer{};    ///< -1 for a base layer.
        /// VTXT, 8 bytes per point: vertex position, a word, opacity. Additional
        /// layers only.
        std::vector<std::byte> alpha_map;
    };
    std::vector<TextureLayer> base_layers;
    std::vector<TextureLayer> additional_layers;
    static constexpr std::size_t k_texture_layer_size = 8;
    static constexpr std::size_t k_alpha_point_size = 8;
};

/// NAVM: one cell's navmesh. 20,057 in SE (102 MiB), 19,986 compressed.
struct NavMesh {
    /// NVNM: vertices, triangles, edge links, cover and door portals in a
    /// variable-length layout (6,801 distinct sizes in vanilla). Kept raw;
    /// `decode_nav_mesh_geometry` reads it.
    std::vector<std::byte> geometry;

    std::vector<std::byte> onam; ///< ONAM, on 463 records.
    std::vector<std::byte> pnam; ///< PNAM
    std::vector<std::byte> nnam; ///< NNAM
};

/// NVNM decoded. Version 12 is the only one in LE, SE and VR; every vanilla
/// NAVM decodes with no bytes left over. Layout from xEdit's TES5 definitions.
struct NavMeshGeometry {
    static constexpr std::uint32_t k_version = 12;

    /// Triangle flag bits. Bits 0-2: edge k (v0-v1, v1-v2, v2-v0) is an edge
    /// link rather than a neighbour.
    static constexpr std::uint16_t k_edge_link_mask = 0x0007;
    static constexpr std::uint16_t k_preferred = 0x0040;
    static constexpr std::uint16_t k_water = 0x0200;
    static constexpr std::uint16_t k_door = 0x0400;

    struct Triangle {
        std::array<std::uint16_t, 3> vertices{};
        /// Per edge: the neighbouring triangle, -1 for none, or an index into
        /// `edge_links` if the edge's flag bit is set.
        std::array<std::int16_t, 3> edges{};
        std::uint16_t flags{};
        std::uint16_t cover{};
    };
    /// An edge leading into another navmesh. Type 0 is a portal (another
    /// cell's navmesh, edges side by side); 1 and 2 come in equal numbers
    /// and join edges far apart (ledges).
    struct EdgeLink {
        std::uint32_t type{};
        FormId navmesh;
        std::int16_t triangle{};
    };
    /// A triangle in front of a door.
    struct DoorTriangle {
        std::int16_t triangle{};
        std::uint32_t type{}; ///< A hash of the door type; meaning unknown.
        FormId door;          ///< The door's REFR.
    };

    FormId world; ///< Null for an interior navmesh.
    FormId cell;  ///< Interiors only.
    std::int16_t grid_x{};
    std::int16_t grid_y{};
    std::vector<Vec3> vertices;
    std::vector<Triangle> triangles;
    std::vector<EdgeLink> edge_links;
    std::vector<DoorTriangle> doors;
    std::vector<std::uint16_t> cover_triangles;
    // The search grid at the end is checked but not kept.
};

/// NAVI: the navigation index linking all NAVMs, one per worldspace plugin.
/// Eight in SE, among the largest records.
struct NavigationIndex {
    std::uint32_t version{}; ///< NVER, 4 bytes on all eight.

    /// NVMI, one per navmesh (5,461 in SE, 373 sizes); NVPP, preferred paths
    /// (25,696 bytes on all eight); NVSI, islands. Creation Kit bookkeeping,
    /// kept raw.
    std::vector<std::vector<std::byte>> mesh_info;
    std::vector<std::byte> preferred_paths;
    std::vector<std::vector<std::byte>> islands;
};

/// ACHR: a placed actor. 12,978 in SE. Same placement fields as REFR, different
/// extras.
struct ActorReference {
    std::string editor_id;
    ScriptData scripts; ///< VMAD

    FormId base;    ///< NAME, 4 bytes on all 13,075: the placed NPC_.
    Vec3 position;  ///< DATA[0..11], 24 bytes on 13,065 of 13,075.
    Vec3 rotation;  ///< DATA[12..23], radians.
    float scale{1.0F}; ///< XSCL; absent means 1.0.

    std::optional<Reference::EnableParent> enable_parent; ///< XESP
    std::vector<Reference::LinkedReference> linked_references; ///< XLKR

    FormId light_ref;      ///< XLRT
    std::int32_t count{};  ///< XLCM
    FormId location;       ///< XLCN
    FormId owner;          ///< XOWN
    FormId encounter_zone; ///< XEZN
    FormId ignored_by_sandbox; ///< INAM
    FormId horse;          ///< XHOR
    FormId location_ref;   ///< XLRL
    float patrol_idle_time{}; ///< XPRD

    /// XAPD + XAPR: activate parents. XAPD is one flags byte for the list, XAPR
    /// 8 bytes (reference, delay) per entry.
    std::uint8_t activate_parent_flags{};
    struct ActivateParent {
        FormId reference;
        float delay{};
    };
    std::vector<ActivateParent> activate_parents;

    /// XRGD and XRGB: ragdoll data saved by the Creation Kit (532 bytes on 564
    /// records, plus 16 other sizes). Kept raw.
    std::vector<std::byte> ragdoll;
    std::vector<std::byte> ragdoll_bones;

    /// PDTO, 8 bytes: topic override. XPPA ("is patrol") and XIS2 ("ignored by
    /// sandbox") are zero-length markers, stored as booleans.
    std::vector<std::byte> topic_override;
    bool is_patrol{};
    bool ignored_by_sandbox_marker{};

    /// Header flags, as on REFR.
    bool initially_disabled{};
    bool persistent{};
    bool deleted{};
};

// ---- parsing --------------------------------------------------------------

[[nodiscard]] io::ParseResult<LandTexture> parse_land_texture(io::SpanReader& data,
                                                              const FormContext& ctx);
[[nodiscard]] io::ParseResult<ImageSpace> parse_image_space(io::SpanReader& data,
                                                            const FormContext& ctx);
[[nodiscard]] io::ParseResult<Climate> parse_climate(io::SpanReader& data,
                                                     const FormContext& ctx);
[[nodiscard]] io::ParseResult<Weather> parse_weather(io::SpanReader& data,
                                                     const FormContext& ctx);
[[nodiscard]] io::ParseResult<ShaderParticleGeometry> parse_shader_particle_geometry(
    io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Region> parse_region(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Location> parse_location(io::SpanReader& data,
                                                       const FormContext& ctx);
[[nodiscard]] io::ParseResult<Landscape> parse_landscape(io::SpanReader& data,
                                                         const FormContext& ctx);
[[nodiscard]] io::ParseResult<NavMesh> parse_nav_mesh(io::SpanReader& data,
                                                      const FormContext& ctx);
/// Decode NAVM's NVNM. Indices are checked against the counts.
[[nodiscard]] io::ParseResult<NavMeshGeometry> decode_nav_mesh_geometry(
    std::span<const std::byte> nvnm);
[[nodiscard]] io::ParseResult<NavigationIndex> parse_navigation_index(io::SpanReader& data,
                                                                      const FormContext& ctx);
/// ACHR also needs the header, as REFR does.
[[nodiscard]] io::ParseResult<ActorReference> parse_actor_reference(const RecordHeader& header,
                                                                    io::SpanReader& data,
                                                                    const FormContext& ctx);

} // namespace bethconv::record
