// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/content_hash.hpp"

#include <blake3.h>

#include <utility>

namespace bethconv::pack {
namespace {

/// Absorb a component's length as a fixed 8-byte little-endian value.
void absorb_length(blake3_hasher& state, std::uint64_t length) {
    std::array<std::uint8_t, 8> encoded{};
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        encoded[i] = static_cast<std::uint8_t>((length >> (8 * i)) & 0xFFu);
    }
    blake3_hasher_update(&state, static_cast<const void*>(encoded.data()), encoded.size());
}

constexpr char k_hex[] = "0123456789abcdef";

} // namespace

struct Hasher::Impl {
    blake3_hasher state{};
};

Hasher::Hasher() : impl_(std::make_unique<Impl>()) { blake3_hasher_init(&impl_->state); }
Hasher::~Hasher() = default;
Hasher::Hasher(Hasher&&) noexcept = default;
Hasher& Hasher::operator=(Hasher&&) noexcept = default;

void Hasher::absorb(std::span<const std::byte> bytes) {
    absorb_length(impl_->state, bytes.size());
    absorb_raw(bytes);
}

void Hasher::absorb(std::string_view text) {
    absorb_length(impl_->state, text.size());
    blake3_hasher_update(&impl_->state, static_cast<const void*>(text.data()), text.size());
}

void Hasher::absorb_raw(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return;
    }
    blake3_hasher_update(&impl_->state, static_cast<const void*>(bytes.data()), bytes.size());
}

ContentHash Hasher::finish() const {
    std::array<std::uint8_t, BLAKE3_OUT_LEN> out{};
    blake3_hasher_finalize(&impl_->state, out.data(), out.size());
    ContentHash hash;
    for (std::size_t i = 0; i < out.size(); ++i) {
        hash.bytes[i] = static_cast<std::byte>(out[i]);
    }
    return hash;
}

std::string ContentHash::hex() const {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes) {
        const auto value = static_cast<unsigned>(b);
        out.push_back(k_hex[(value >> 4) & 0x0Fu]);
        out.push_back(k_hex[value & 0x0Fu]);
    }
    return out;
}

std::string ContentHash::prefix() const {
    const auto value = static_cast<unsigned>(bytes[0]);
    return std::string{k_hex[(value >> 4) & 0x0Fu], k_hex[value & 0x0Fu]};
}

ContentHash content_hash(std::span<const std::byte> source, std::string_view converter,
                         std::string_view settings) {
    Hasher hasher;
    hasher.absorb(source);
    hasher.absorb(converter);
    hasher.absorb(settings);
    return hasher.finish();
}

} // namespace bethconv::pack
