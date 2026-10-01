// SPDX-License-Identifier: GPL-3.0-or-later
//
// Asset names in a pack: BLAKE3(source bytes ‖ converter version ‖ settings).
// Gives dedupe (Skyrim reuses assets heavily) and incremental rebuilds.
//
//   * It hashes the input, so it is known before conversion and duplicates are
//     never converted. It changes only when the settings string says the
//     output changed.
//   * Converter version and settings are included, so packs from different
//     versions or settings never collide.
//
// Each component is prefixed with its 64-bit length; plain concatenation would
// hash bytes `ab` + settings `c` the same as `a` + `bc`.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace bethconv::pack {

/// A BLAKE3-256 digest.
struct ContentHash {
    std::array<std::byte, 32> bytes{};

    /// 64 lowercase hex characters; the asset's filename stem.
    [[nodiscard]] std::string hex() const;

    /// First byte as two hex characters: the `<bb>` fanout directory (256
    /// buckets, a few hundred files each for vanilla).
    [[nodiscard]] std::string prefix() const;

    friend bool operator==(const ContentHash&, const ContentHash&) = default;
};

/// Incremental BLAKE3-256. Public because packs also hash non-assets (plugins,
/// load orders).
class Hasher {
public:
    Hasher();
    ~Hasher();
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    Hasher(Hasher&&) noexcept;
    Hasher& operator=(Hasher&&) noexcept;

    /// Absorb `bytes` preceded by its length. The prefix is per call, so two
    /// calls differ from one call over the concatenation: one call per
    /// component.
    void absorb(std::span<const std::byte> bytes);
    void absorb(std::string_view text);

    /// Absorb without a length prefix, to stream one component in chunks. The
    /// caller handles framing.
    void absorb_raw(std::span<const std::byte> bytes);

    [[nodiscard]] ContentHash finish() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The asset name formula, with length framing.
///
/// `converter` is the version string. `settings` fingerprints everything that
/// affects the output and is per asset kind, so texture settings never rename
/// meshes. See `pack::ConvertOptions::settings_for`.
[[nodiscard]] ContentHash content_hash(std::span<const std::byte> source,
                                       std::string_view converter, std::string_view settings);

} // namespace bethconv::pack
