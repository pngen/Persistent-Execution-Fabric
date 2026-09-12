// Persistent Execution Fabric - hashing primitives.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/hash.hpp"

#include <array>

namespace pef {
namespace {

// Reflected CRC-64/ECMA-182 table, built once at first use. The table is a pure
// function of the polynomial, so the value is identical on every platform.
struct Crc64Table {
    std::array<std::uint64_t, 256> values{};

    constexpr Crc64Table() noexcept {
        constexpr std::uint64_t kPoly = 0xC96C5795D7870F42ULL;  // reflected ECMA-182
        for (std::uint64_t i = 0; i < 256; ++i) {
            std::uint64_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1ULL) ? ((crc >> 1) ^ kPoly) : (crc >> 1);
            }
            values[static_cast<std::size_t>(i)] = crc;
        }
    }
};

constexpr Crc64Table kCrc64Table{};

}  // namespace

std::uint64_t crc64_ecma(const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t index = static_cast<std::uint8_t>(crc ^ p[i]);
        crc = kCrc64Table.values[index] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFFFFFFFFFULL;
}

void HashBuilder::u16(std::uint16_t v) noexcept {
    byte(static_cast<std::uint8_t>(v & 0xFFu));
    byte(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void HashBuilder::u32(std::uint32_t v) noexcept {
    for (int shift = 0; shift < 32; shift += 8) {
        byte(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }
}

void HashBuilder::u64(std::uint64_t v) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        byte(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }
}

void HashBuilder::text(std::string_view s) noexcept {
    u64(static_cast<std::uint64_t>(s.size()));
    raw(s);
}

std::string hex64(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xFu];
        value >>= 4;
    }
    return out;
}

std::string hex32(std::uint32_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xFu];
        value >>= 4;
    }
    return out;
}

std::string hex16(std::uint16_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(4, '0');
    for (int i = 3; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xFu];
        value = static_cast<std::uint16_t>(value >> 4);
    }
    return out;
}

bool parse_hex_u64(std::string_view text, std::uint64_t& out) noexcept {
    if (text.size() != 16) {
        return false;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

}  // namespace pef
