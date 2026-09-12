// Persistent Execution Fabric - record codec.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Records declare their durable field order once, as a tuple of member
// pointers, in a RecordTraits specialization. Encoding and decoding are then
// generated from that single declaration, so a field can never be written in
// one order and read in another.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "pef/bytes.hpp"
#include "pef/identity.hpp"

namespace pef {

template <class T>
struct is_id : std::false_type {};
template <class Tag>
struct is_id<Id<Tag>> : std::true_type {};

template <class T>
struct is_gen : std::false_type {};
template <class Tag>
struct is_gen<Gen<Tag>> : std::true_type {};

template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};

template <class T>
struct is_vector : std::false_type {};
template <class T, class A>
struct is_vector<std::vector<T, A>> : std::true_type {};

// Declared by each record type below.
template <class T>
struct RecordTraits;

template <class T>
void encode_record(ByteWriter& w, const T& record);

template <class T>
bool decode_record(ByteReader& r, T& out);

// True when a type describes its durable field order with RecordTraits. Types
// that do not must provide their own encode()/decode() members.
template <class T, class = void>
struct has_record_traits : std::false_type {};
template <class T>
struct has_record_traits<T, std::void_t<decltype(RecordTraits<T>::members)>>
    : std::true_type {};

// Declared by each enum in its own header as an ADL customization point.
// The primary template never matches; a missing overload is a compile error,
// which is intentional: every persisted enum must state its valid range.
template <class E>
constexpr bool pef_valid_enum(E) = delete;

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------
template <class I>
void write_integer(ByteWriter& w, I v) {
    if constexpr (std::is_signed_v<I>) {
        w.i64(static_cast<std::int64_t>(v));
    } else if constexpr (sizeof(I) == 1) {
        w.u8(static_cast<std::uint8_t>(v));
    } else if constexpr (sizeof(I) == 2) {
        w.u16(static_cast<std::uint16_t>(v));
    } else if constexpr (sizeof(I) == 4) {
        w.u32(static_cast<std::uint32_t>(v));
    } else {
        w.u64(static_cast<std::uint64_t>(v));
    }
}

template <class T>
void write_field(ByteWriter& w, const T& value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_id<U>::value) {
        w.u64(value.value());
    } else if constexpr (is_gen<U>::value) {
        w.u64(value.value());
    } else if constexpr (std::is_same_v<U, bool>) {
        w.boolean(value);
    } else if constexpr (std::is_enum_v<U>) {
        w.u8(static_cast<std::uint8_t>(value));
    } else if constexpr (std::is_integral_v<U>) {
        write_integer(w, value);
    } else if constexpr (std::is_same_v<U, std::string>) {
        w.text(value);
    } else if constexpr (std::is_same_v<U, Bytes>) {
        w.blob(value);
    } else if constexpr (is_optional<U>::value) {
        if (value.has_value()) {
            w.boolean(true);
            write_field(w, *value);
        } else {
            w.boolean(false);
        }
    } else if constexpr (is_vector<U>::value) {
        w.u32(static_cast<std::uint32_t>(value.size()));
        for (const auto& element : value) {
            write_field(w, element);
        }
    } else if constexpr (has_record_traits<U>::value) {
        encode_record(w, value);
    } else {
        value.encode(w);
    }
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------
template <class I>
bool read_integer(ByteReader& r, I& out) {
    if constexpr (std::is_signed_v<I>) {
        const auto raw = r.i64();
        out = static_cast<I>(raw);
    } else if constexpr (sizeof(I) == 1) {
        out = static_cast<I>(r.u8());
    } else if constexpr (sizeof(I) == 2) {
        out = static_cast<I>(r.u16());
    } else if constexpr (sizeof(I) == 4) {
        out = static_cast<I>(r.u32());
    } else {
        out = static_cast<I>(r.u64());
    }
    return r.ok();
}

template <class T>
bool read_field(ByteReader& r, T& out) {
    using U = std::remove_cvref_t<T>;
    bool ok = true;
    if constexpr (is_id<U>::value) {
        out = U{r.u64()};
    } else if constexpr (is_gen<U>::value) {
        out = U{r.u64()};
    } else if constexpr (std::is_same_v<U, bool>) {
        out = r.boolean();
    } else if constexpr (std::is_enum_v<U>) {
        // An out-of-range ordinal is a decoding failure, never a coerced value.
        const auto raw = r.u8();
        const U candidate = static_cast<U>(raw);
        if (!r.ok() || !pef_valid_enum(candidate)) {
            r.fail();
            ok = false;
        } else {
            out = candidate;
        }
    } else if constexpr (std::is_integral_v<U>) {
        ok = read_integer(r, out);
    } else if constexpr (std::is_same_v<U, std::string>) {
        out = r.text();
    } else if constexpr (std::is_same_v<U, Bytes>) {
        out = r.blob();
    } else if constexpr (is_optional<U>::value) {
        if (r.boolean() && r.ok()) {
            typename U::value_type inner{};
            if (read_field(r, inner)) {
                out = std::move(inner);
            } else {
                ok = false;
            }
        } else {
            out.reset();
        }
    } else if constexpr (is_vector<U>::value) {
        const std::uint32_t count = r.u32();
        if (!r.ok() || count > r.limits().max_container) {
            // A hostile or corrupt count must not drive an unbounded allocation.
            r.fail();
            ok = false;
        } else {
            out.clear();
            const std::size_t initial = count < 1024u ? static_cast<std::size_t>(count) : 1024u;
            out.reserve(initial);
            for (std::uint32_t i = 0; i < count && ok; ++i) {
                typename U::value_type element{};
                if (read_field(r, element)) {
                    out.push_back(std::move(element));
                } else {
                    ok = false;
                }
            }
        }
    } else if constexpr (has_record_traits<U>::value) {
        ok = decode_record(r, out);
    } else {
        ok = U::decode(r, out);
    }
    return ok && r.ok();
}

// ---------------------------------------------------------------------------
// Record level encode / decode driven by RecordTraits.
// ---------------------------------------------------------------------------
template <class T>
void encode_record(ByteWriter& w, const T& record) {
    std::apply([&](auto... member) { (write_field(w, record.*member), ...); },
               RecordTraits<T>::members);
}

template <class T>
bool decode_record(ByteReader& r, T& out) {
    bool ok = true;
    std::apply([&](auto... member) { ok = (read_field(r, out.*member) && ...); },
               RecordTraits<T>::members);
    return ok && r.ok();
}

// Encode a record into a standalone buffer.
template <class T>
[[nodiscard]] Bytes encode_to_bytes(const T& record) {
    ByteWriter w(256);
    encode_record(w, record);
    return w.take();
}

// Decode a record and require that no trailing bytes remain.
template <class T>
[[nodiscard]] bool decode_from_bytes(const Bytes& data, T& out) {
    ByteReader r(data);
    if (!decode_record(r, out)) {
        return false;
    }
    return r.at_end();
}

template <class T>
[[nodiscard]] std::optional<T> decode_optional(const Bytes& data) {
    T out{};
    if (!decode_from_bytes(data, out)) {
        return std::nullopt;
    }
    return out;
}

}  // namespace pef
