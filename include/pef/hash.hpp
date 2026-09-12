// Persistent Execution Fabric - deterministic hashing primitives.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Every hash used by the fabric is a pure function of canonical bytes. No
// pointer values, no time, no ASLR-dependent data, no random-device output.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pef {

// FNV-1a 64: used for content identity and cheap canonical digests.
inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x00000100000001b3ULL;

[[nodiscard]] constexpr std::uint64_t fnv1a_step(std::uint64_t h, std::uint8_t b) noexcept {
    return (h ^ static_cast<std::uint64_t>(b)) * kFnvPrime;
}

[[nodiscard]] constexpr std::uint64_t fnv1a(std::string_view data) noexcept {
    std::uint64_t h = kFnvOffsetBasis;
    for (char c : data) {
        h = fnv1a_step(h, static_cast<std::uint8_t>(c));
    }
    return h;
}

// CRC-64/ECMA-182 (reflected, poly 0x42F0E1EBA9EA3693 reversed). Used for
// integrity protection of persisted frames and protocol frames. This detects
// corruption and truncation; it is not a cryptographic authenticator.
[[nodiscard]] std::uint64_t crc64_ecma(const void* data, std::size_t size) noexcept;

[[nodiscard]] inline std::uint64_t crc64_ecma(std::string_view data) noexcept {
    return crc64_ecma(data.data(), data.size());
}

// Incremental canonical hash builder. Values are fed in a fixed, documented
// order so that equivalent canonical state yields the same digest.
class HashBuilder {
public:
    HashBuilder() noexcept = default;
    explicit HashBuilder(std::uint64_t seed) noexcept : state_(seed) {}

    void byte(std::uint8_t b) noexcept { state_ = fnv1a_step(state_, b); }

    void raw(const void* data, std::size_t size) noexcept {
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            byte(p[i]);
        }
    }
    void raw(std::string_view s) noexcept { raw(s.data(), s.size()); }

    void u16(std::uint16_t v) noexcept;
    void u32(std::uint32_t v) noexcept;
    void u64(std::uint64_t v) noexcept;

    // Length-prefixed so that distinct sequences cannot collide by
    // concatenation ambiguity.
    void text(std::string_view s) noexcept;

    [[nodiscard]] std::uint64_t digest() const noexcept { return state_; }

private:
    std::uint64_t state_ = kFnvOffsetBasis;
};

[[nodiscard]] std::string hex64(std::uint64_t value);
[[nodiscard]] std::string hex32(std::uint32_t value);
[[nodiscard]] std::string hex16(std::uint16_t value);
[[nodiscard]] bool parse_hex_u64(std::string_view text, std::uint64_t& out) noexcept;

}  // namespace pef
