// SPDX-License-Identifier: GPL-3.0-or-later
#include "alpha_usage.hpp"

#include "bethconv/archive/vpath.hpp"
#include "bethconv/mesh/nif_reader.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace bethconv::pack {
namespace {

struct TextureUse {
    std::map<std::uint32_t, std::uint32_t> tested; ///< threshold -> materials
    std::uint32_t blended = 0;                     ///< diffuse, blended and not tested
    std::uint32_t other_slot = 0;                  ///< named in a slot other than 0
};

struct Tally {
    std::unordered_map<std::string, TextureUse> textures;
    std::uint64_t scanned = 0;
    std::uint64_t failed = 0;
    std::uint64_t tested_materials = 0;
    std::uint64_t tested_and_blended_materials = 0;
};

void merge(Tally& into, Tally&& from) {
    into.scanned += from.scanned;
    into.failed += from.failed;
    into.tested_materials += from.tested_materials;
    into.tested_and_blended_materials += from.tested_and_blended_materials;
    for (auto& [vpath, use] : from.textures) {
        TextureUse& target = into.textures[vpath];
        for (const auto& [threshold, count] : use.tested) {
            target.tested[threshold] += count;
        }
        target.blended += use.blended;
        target.other_slot += use.other_slot;
    }
}

[[nodiscard]] bool is_mesh_path(std::string_view vpath) {
    return vpath.ends_with(".nif") || vpath.ends_with(".btr") || vpath.ends_with(".bto");
}

void tally_model(const mesh::Model& model, Tally& tally) {
    for (const mesh::Material& material : model.materials) {
        // Trees name their diffuse map again as the glow slot; that is the same
        // texture, not a second use of it.
        const std::string diffuse = archive::normalize_vpath(material.textures[0]);
        for (std::size_t slot = 0; slot < material.textures.size(); ++slot) {
            const std::string& name = material.textures[slot];
            if (name.empty()) {
                continue;
            }
            std::string vpath = archive::normalize_vpath(name);
            if (!vpath.ends_with(".dds")) {
                continue;
            }
            TextureUse& use = tally.textures[vpath];
            if (slot != 0) {
                if (vpath != diffuse) {
                    ++use.other_slot;
                }
                continue;
            }
            if (material.alpha_mode == mesh::AlphaMode::mask) {
                const auto threshold = static_cast<std::uint32_t>(
                    std::lround(std::clamp(material.alpha_cutoff, 0.0F, 1.0F) * 255.0F));
                if (threshold == 0) {
                    // "Alpha greater than 0": only texels that are fully clear
                    // go, which no mip level changes. Not a cut-out.
                    continue;
                }
                ++use.tested[threshold];
                ++tally.tested_materials;
                if ((material.alpha_flags & 0x0001u) != 0) {
                    ++tally.tested_and_blended_materials;
                }
            } else if (material.alpha_mode == mesh::AlphaMode::blend) {
                ++use.blended;
            }
        }
    }
}

} // namespace

constexpr std::uint32_t k_lod_tree_threshold = 128; // 0.5, as lod_tree.gdshader

std::string coverage_recipe(std::uint32_t threshold) {
    return ";cov/1;t=" + std::to_string(threshold);
}

AlphaUsage scan_alpha_usage(const archive::ArchiveSet& set, unsigned jobs) {
    std::vector<std::string> meshes;
    std::vector<std::string> tree_lists;
    set.for_each([&](const archive::Resolution& entry) {
        if (is_mesh_path(entry.vpath)) {
            meshes.push_back(entry.vpath);
        } else if (entry.vpath.starts_with("meshes/terrain/") && entry.vpath.ends_with(".lst")) {
            tree_lists.push_back(entry.vpath);
        }
    });
    std::ranges::sort(meshes);

    mesh::ReadOptions read;
    read.read_collision = false;
    read.read_skinning = false;
    read.read_animations = false;

    const unsigned wanted = jobs != 0 ? jobs : std::thread::hardware_concurrency();
    const unsigned threads = std::clamp<unsigned>(
        static_cast<unsigned>(std::min<std::size_t>(wanted == 0 ? 1 : wanted, 64)), 1U,
        static_cast<unsigned>(std::max<std::size_t>(meshes.size(), 1)));

    std::vector<Tally> tallies(threads);
    std::atomic<std::size_t> next{0};
    const auto work = [&](unsigned id) {
        Tally& tally = tallies[id];
        for (std::size_t i = next.fetch_add(1); i < meshes.size(); i = next.fetch_add(1)) {
            auto bytes = set.read(meshes[i]);
            if (!bytes) {
                ++tally.failed;
                continue;
            }
            auto model = mesh::read_nif(*bytes, meshes[i], read);
            if (!model) {
                ++tally.failed;
                continue;
            }
            ++tally.scanned;
            tally_model(*model, tally);
        }
    };
    if (threads == 1) {
        work(0);
    } else {
        std::vector<std::jthread> pool;
        for (unsigned id = 0; id < threads; ++id) {
            pool.emplace_back(work, id);
        }
    }

    Tally total;
    for (Tally& tally : tallies) {
        merge(total, std::move(tally));
    }

    AlphaUsage out;
    AlphaCoverageRecord& s = out.summary;
    s.meshes_scanned = total.scanned;
    s.meshes_failed = total.failed;
    s.materials_alpha_tested = total.tested_materials;
    s.materials_tested_and_blended = total.tested_and_blended_materials;

    std::vector<std::string> names;
    names.reserve(total.textures.size());
    for (const auto& [vpath, use] : total.textures) {
        names.push_back(vpath);
    }
    std::ranges::sort(names);
    for (const std::string& vpath : names) {
        const TextureUse& use = total.textures[vpath];
        if (use.tested.empty()) {
            // Diffuse maps that are only ever blended; a normal map of another
            // material is not counted as one.
            if (use.blended != 0 && use.other_slot == 0) {
                ++s.textures_blend_only;
            }
            continue;
        }
        ++s.textures_alpha_tested;
        if (use.blended != 0) {
            ++s.textures_tested_and_blended;
        }
        // Most used threshold; the lowest wins a tie, so the choice never
        // depends on the order the meshes were read in.
        std::uint32_t chosen = 0;
        std::uint32_t best = 0;
        for (const auto& [threshold, count] : use.tested) {
            if (count > best) {
                best = count;
                chosen = threshold;
            }
        }
        if (use.tested.size() > 1) {
            ++s.textures_conflicting;
            AlphaCoverageRecord::Conflict conflict;
            conflict.vpath = vpath;
            conflict.chosen = chosen;
            conflict.uses.assign(use.tested.begin(), use.tested.end());
            s.conflicts.push_back(std::move(conflict));
        }
        if (use.other_slot != 0) {
            ++s.textures_shared_slot;
            continue;
        }
        if (chosen == 0) {
            continue; // threshold 0 passes everything: no coverage to keep
        }
        ++s.textures_treated;
        out.thresholds.emplace(vpath, chosen);
        s.treated.emplace_back(vpath, chosen);
    }

    // Tree LOD billboards are cut out of one atlas, named by the convention the
    // engine loads it by (meshes/terrain/<w>/trees/<w>.lst lists the types,
    // textures/terrain/<w>/trees/<w>treelod.dds is the atlas), and the engine's
    // shader tests it at 0.5 (shaders/lod_tree.gdshader). No mesh says so.
    std::ranges::sort(tree_lists);
    for (const std::string& list : tree_lists) {
        constexpr std::string_view prefix = "meshes/terrain/";
        const std::string_view rest = std::string_view(list).substr(prefix.size());
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) {
            continue;
        }
        const std::string world(rest.substr(0, slash));
        if (list != std::string(prefix) + world + "/trees/" + world + ".lst") {
            continue;
        }
        const std::string atlas = "textures/terrain/" + world + "/trees/" + world + "treelod.dds";
        if (!set.resolve(atlas) || out.thresholds.contains(atlas)) {
            continue;
        }
        out.thresholds.emplace(atlas, k_lod_tree_threshold);
        s.treated.emplace_back(atlas, k_lod_tree_threshold);
        ++s.textures_treated;
        ++s.textures_alpha_tested;
        ++s.lod_tree_atlases;
    }
    std::ranges::sort(s.treated);
    return out;
}

} // namespace bethconv::pack
