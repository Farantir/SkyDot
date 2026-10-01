// SPDX-License-Identifier: GPL-3.0-or-later
//
// Controllers, sequences and particle systems -> Model::animations and
// Model::particles. Private to the mesh reader; nifly types are fine here.
//
// The scene walk only records what it meets (`attach`, `add_particles`);
// everything is read in `finish`, once every node exists, because controllers
// and particle modifiers point at nodes anywhere in the file.
#pragma once

#include "bethconv/mesh/mesh_ir.hpp"

#include <NifFile.hpp>
#include <Particles.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bethconv::mesh::detail {

class ControllerReader {
public:
    ControllerReader(nifly::NifFile& nif, Model& model,
                     const std::unordered_map<std::uint32_t, std::size_t>& node_by_block,
                     const std::unordered_map<std::string, std::size_t>& node_by_name)
        : nif_(nif), model_(model), node_by_block_(node_by_block), node_by_name_(node_by_name) {}

    /// `owner` (a node, shader or alpha property) belongs to IR node `node`;
    /// its controller chain is read in finish().
    void attach(nifly::NiObjectNET* owner, std::size_t node);

    /// A NiParticleSystem walked as IR node `node`, drawn with `material`.
    void add_particles(nifly::NiParticleSystem* system, std::size_t node, std::size_t material);

    void finish();

private:
    struct Owner {
        nifly::NiObjectNET* object;
        std::size_t node;
    };
    struct PendingParticles {
        nifly::NiParticleSystem* system;
        std::size_t node;
        std::size_t material;
    };

    nifly::NifFile& nif_;
    Model& model_;
    const std::unordered_map<std::uint32_t, std::size_t>& node_by_block_;
    const std::unordered_map<std::string, std::size_t>& node_by_name_;

    std::vector<Owner> owners_;
    /// Block index of an owner -> its IR node, for sequence targets.
    std::unordered_map<std::uint32_t, std::size_t> node_by_owner_;
    std::vector<PendingParticles> pending_particles_;
    std::unordered_map<std::string, std::size_t> unsupported_;

    void read_particles(const PendingParticles& pending);
    void read_sequence(nifly::NiControllerSequence* sequence);

    /// Channels one controller/interpolator pair produces on `node`.
    void channels_for(nifly::NiTimeController* controller, std::string_view interp_id,
                      nifly::NiInterpolator* interpolator, std::size_t node,
                      std::vector<AnimationChannel>& out);
    void transform_channels(nifly::NiInterpolator* interpolator, std::size_t node,
                            std::vector<AnimationChannel>& out);
    /// Emitters get the birth rate their controllers start with.
    void resolve_birth_rates();

    [[nodiscard]] std::optional<std::size_t> node_of(std::uint32_t block) const;
    void note_unsupported(std::string_view what);
    void mark(std::size_t node);
};

} // namespace bethconv::mesh::detail
