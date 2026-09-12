// Persistent Execution Fabric - bounded, deterministic byte codec.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// All persisted and protocol payloads move through this codec. Encoding is
// little-endian and canonical: no padding, no host-endianness dependence, no
// pointer values, no time. Decoding is fully bounded: every declared length is
// checked against both the configured limit and the bytes actually available,
// so a corrupt length can never drive an unbounded allocation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace pef {

using Bytes = std::vector<std::byte>;

// Decoding limits. These are persistence limits and protocol limits chosen
// independently; a network frame limit is never reused as a persistence limit.
struct DecodeLimits {
    std::size_t max_blob = 16u * 1024u * 1024u;      // single byte blob
    std::size_t max_string = 64u * 1024u;            // single string
    std::size_t max_container = 1u << 20;            // elements in one container
    std::size_t max_total = 256u * 1024u * 1024u;    // whole payload
};

[[nodiscard]] const DecodeLimits& default_decode_limits() noexcept;

class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(std::size_t reserve) { buffer_.reserve(reserve); }

    void u8(std::uint8_t v) { buffer_.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void boolean(bool v) { u8(v ? 1u : 0u); }
    void blob(const void* data, std::size_t size);
    void blob(const Bytes& b) { blob(b.data(), b.size()); }
    void text(std::string_view s);
    void raw(const void* data, std::size_t size);

    [[nodiscard]] const Bytes& bytes() const noexcept { return buffer_; }
    [[nodiscard]] Bytes take() noexcept { return std::move(buffer_); }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    void clear() noexcept { buffer_.clear(); }

private:
    Bytes buffer_;
};

class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) noexcept
        : data_(reinterpret_cast<const std::byte*>(data)), size_(size) {}
    explicit ByteReader(const Bytes& b) noexcept : data_(b.data()), size_(b.size()) {}
    ByteReader(const Bytes& b, const DecodeLimits& limits) noexcept
        : data_(b.data()), size_(b.size()), limits_(&limits) {}
    ByteReader(const std::uint8_t* data, std::size_t size, const DecodeLimits& limits) noexcept
        : data_(reinterpret_cast<const std::byte*>(data)), size_(size), limits_(&limits) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool failed() const noexcept { return !ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? size_ - offset_ : 0; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    // Marks the reader failed without performing a read.
    void fail() noexcept { ok_ = false; }

    [[nodiscard]] std::uint8_t u8();
    [[nodiscard]] std::uint16_t u16();
    [[nodiscard]] std::uint32_t u32();
    [[nodiscard]] std::uint64_t u64();
    [[nodiscard]] std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    [[nodiscard]] bool boolean();
    [[nodiscard]] Bytes blob();
    [[nodiscard]] std::string text();

    // Reads a length-prefixed byte range in place. The returned pointer is
    // valid only while the underlying buffer is alive.
    [[nodiscard]] std::pair<const std::byte*, std::size_t> blob_view();

    // True when every byte has been consumed. Used to reject trailing bytes.
    [[nodiscard]] bool at_end() const noexcept { return ok_ && offset_ == size_; }

    void set_limits(const DecodeLimits& limits) noexcept { limits_ = &limits; }
    [[nodiscard]] const DecodeLimits& limits() const noexcept { return *limits_; }

private:
    [[nodiscard]] bool require(std::size_t n) noexcept;

    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
    bool ok_ = true;
    const DecodeLimits* limits_ = &default_decode_limits();
};

// Converts a byte buffer to a std::string of raw bytes (not hex).
[[nodiscard]] std::string bytes_to_string(const Bytes& b);
[[nodiscard]] Bytes string_to_bytes(std::string_view s);

}  // namespace pef
