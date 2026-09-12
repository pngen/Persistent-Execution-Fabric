// Persistent Execution Fabric - bounded byte codec.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/bytes.hpp"

#include <limits>

namespace pef {

const DecodeLimits& default_decode_limits() noexcept {
    static const DecodeLimits limits{};
    return limits;
}

void ByteWriter::u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v & 0xFFu));
    u8(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void ByteWriter::u32(std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) {
        u8(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }
}

void ByteWriter::u64(std::uint64_t v) {
    for (int shift = 0; shift < 64; shift += 8) {
        u8(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }
}

void ByteWriter::raw(const void* data, std::size_t size) {
    const auto* p = static_cast<const std::byte*>(data);
    buffer_.insert(buffer_.end(), p, p + size);
}

void ByteWriter::blob(const void* data, std::size_t size) {
    // Blobs are length-prefixed with a 64-bit length; the reader enforces the
    // configured bound before allocating.
    u64(static_cast<std::uint64_t>(size));
    raw(data, size);
}

void ByteWriter::text(std::string_view s) {
    u32(static_cast<std::uint32_t>(s.size()));
    raw(s.data(), s.size());
}

bool ByteReader::require(std::size_t n) noexcept {
    if (!ok_) {
        return false;
    }
    if (n > size_ - offset_) {
        ok_ = false;
        return false;
    }
    if (offset_ + n > limits_->max_total) {
        ok_ = false;
        return false;
    }
    return true;
}

std::uint8_t ByteReader::u8() {
    if (!require(1)) {
        return 0;
    }
    return static_cast<std::uint8_t>(data_[offset_++]);
}

std::uint16_t ByteReader::u16() {
    const std::uint16_t lo = u8();
    const std::uint16_t hi = u8();
    return static_cast<std::uint16_t>(lo | (hi << 8));
}

std::uint32_t ByteReader::u32() {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(u8()) << shift;
    }
    return value;
}

std::uint64_t ByteReader::u64() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(u8()) << shift;
    }
    return value;
}

bool ByteReader::boolean() {
    const std::uint8_t raw = u8();
    if (!ok_) {
        return false;
    }
    if (raw > 1u) {
        ok_ = false;
        return false;
    }
    return raw == 1u;
}

std::pair<const std::byte*, std::size_t> ByteReader::blob_view() {
    const std::uint64_t declared = u64();
    if (!ok_) {
        return {nullptr, 0};
    }
    if (declared > limits_->max_blob) {
        ok_ = false;
        return {nullptr, 0};
    }
    const auto size = static_cast<std::size_t>(declared);
    if (!require(size)) {
        return {nullptr, 0};
    }
    const std::byte* start = data_ + offset_;
    offset_ += size;
    return {start, size};
}

Bytes ByteReader::blob() {
    const auto view = blob_view();
    if (!ok_) {
        return {};
    }
    return Bytes(view.first, view.first + view.second);
}

std::string ByteReader::text() {
    const std::uint32_t declared = u32();
    if (!ok_) {
        return {};
    }
    if (declared > limits_->max_string) {
        ok_ = false;
        return {};
    }
    const auto size = static_cast<std::size_t>(declared);
    if (!require(size)) {
        return {};
    }
    std::string out(reinterpret_cast<const char*>(data_ + offset_), size);
    offset_ += size;
    return out;
}

std::string bytes_to_string(const Bytes& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

Bytes string_to_bytes(std::string_view s) {
    Bytes out(s.size());
    if (!s.empty()) {
        std::memcpy(out.data(), s.data(), s.size());
    }
    return out;
}

}  // namespace pef
