// Persistent Execution Fabric - strongly typed identity domains.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Semantically different authority domains are different C++ types. A
// WorkerId cannot be passed where an ExecutionId is required, and an
// untyped integer literal cannot be passed for either.
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "pef/hash.hpp"

namespace pef {

// ---------------------------------------------------------------------------
// Id<Tag>: an opaque 64-bit identifier in a single authority domain.
// Value 0 is the nil id and is never issued by the fabric.
// ---------------------------------------------------------------------------
template <class Tag>
class Id {
public:
    using tag_type = Tag;

    constexpr Id() noexcept = default;
    constexpr explicit Id(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == 0; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Id a, Id b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(Id a, Id b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator>(Id a, Id b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator<=(Id a, Id b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>=(Id a, Id b) noexcept { return a.value_ >= b.value_; }

private:
    std::uint64_t value_ = 0;
};

template <class Tag>
struct IdHasher {
    [[nodiscard]] std::size_t operator()(Id<Tag> id) const noexcept {
        std::uint64_t x = id.value();
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<std::size_t>(x);
    }
};

// ---------------------------------------------------------------------------
// Gen<Tag>: a monotonically advancing generation counter in one domain.
// Value 0 means "no generation established yet".
// ---------------------------------------------------------------------------
template <class Tag>
class Gen {
public:
    using tag_type = Tag;

    constexpr Gen() noexcept = default;
    constexpr explicit Gen(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_none() const noexcept { return value_ == 0; }
    [[nodiscard]] constexpr bool established() const noexcept { return value_ != 0; }

    // The next generation. Generation 0 advances to 1.
    [[nodiscard]] constexpr Gen next() const noexcept { return Gen(value_ + 1); }
    [[nodiscard]] constexpr Gen advance(std::uint64_t delta) const noexcept {
        return Gen(value_ + delta);
    }

    friend constexpr bool operator==(Gen a, Gen b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Gen a, Gen b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(Gen a, Gen b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator>(Gen a, Gen b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator<=(Gen a, Gen b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>=(Gen a, Gen b) noexcept { return a.value_ >= b.value_; }

private:
    std::uint64_t value_ = 0;
};

template <class Tag>
struct GenHasher {
    [[nodiscard]] std::size_t operator()(Gen<Tag> g) const noexcept {
        return IdHasher<Tag>{}(Id<Tag>{g.value()});
    }
};

// ---------------------------------------------------------------------------
// Identity domains. Each tag is a distinct incomplete type.
// ---------------------------------------------------------------------------
#define PEF_DEFINE_ID_DOMAIN(Name)  \
    struct Name##Tag;               \
    using Name = Id<Name##Tag>

#define PEF_DEFINE_GEN_DOMAIN(Name) \
    struct Name##Tag;               \
    using Name = Gen<Name##Tag>

// Durable execution identity.
PEF_DEFINE_ID_DOMAIN(ExecutionId);
PEF_DEFINE_GEN_DOMAIN(ExecutionGeneration);
PEF_DEFINE_ID_DOMAIN(ExecutionIncarnationId);
PEF_DEFINE_GEN_DOMAIN(ExecutionIncarnationGeneration);
PEF_DEFINE_GEN_DOMAIN(CoordinatorEpoch);

// Process and worker identity. Worker incarnation is process-boot bound.
PEF_DEFINE_ID_DOMAIN(WorkerId);
PEF_DEFINE_ID_DOMAIN(WorkerBootId);
PEF_DEFINE_ID_DOMAIN(SessionId);
PEF_DEFINE_ID_DOMAIN(StoreId);

// Attempts.
PEF_DEFINE_ID_DOMAIN(AttemptId);
PEF_DEFINE_GEN_DOMAIN(AttemptGeneration);

// Continuation authority.
PEF_DEFINE_ID_DOMAIN(ContinuationId);
PEF_DEFINE_GEN_DOMAIN(ContinuationGeneration);

// Checkpoints and lineage.
PEF_DEFINE_ID_DOMAIN(CheckpointId);
PEF_DEFINE_GEN_DOMAIN(CheckpointGeneration);

// Durable progress.
PEF_DEFINE_ID_DOMAIN(ProgressId);
PEF_DEFINE_GEN_DOMAIN(ProgressGeneration);

// Actions and their external side effects.
PEF_DEFINE_ID_DOMAIN(ActionId);
PEF_DEFINE_GEN_DOMAIN(ActionGeneration);
PEF_DEFINE_ID_DOMAIN(SideEffectId);
PEF_DEFINE_GEN_DOMAIN(SideEffectGeneration);

// Replay intent.
PEF_DEFINE_ID_DOMAIN(ReplayId);
PEF_DEFINE_GEN_DOMAIN(ReplayGeneration);

// Continuation leases.
PEF_DEFINE_ID_DOMAIN(LeaseId);
PEF_DEFINE_GEN_DOMAIN(LeaseGeneration);

// External bindings the execution depends on.
PEF_DEFINE_ID_DOMAIN(BindingId);
PEF_DEFINE_GEN_DOMAIN(BindingGeneration);

// Policy.
PEF_DEFINE_ID_DOMAIN(PolicyId);
PEF_DEFINE_GEN_DOMAIN(PolicyGeneration);

// Recovery.
PEF_DEFINE_ID_DOMAIN(RecoveryId);
PEF_DEFINE_GEN_DOMAIN(RecoveryGeneration);

// Logical commit identity.
PEF_DEFINE_ID_DOMAIN(CommitId);

// Client request identity (idempotent request replay protection).
PEF_DEFINE_ID_DOMAIN(RequestId);

// Evidence provenance.
PEF_DEFINE_ID_DOMAIN(EvidenceId);
PEF_DEFINE_GEN_DOMAIN(EvidenceGeneration);

// Ambiguity records.
PEF_DEFINE_ID_DOMAIN(AmbiguityId);
PEF_DEFINE_GEN_DOMAIN(AmbiguityGeneration);

#undef PEF_DEFINE_ID_DOMAIN
#undef PEF_DEFINE_GEN_DOMAIN

// ---------------------------------------------------------------------------
// Text form: 16 lowercase hex digits. Nil renders as 0000000000000000.
// ---------------------------------------------------------------------------
template <class Tag>
[[nodiscard]] std::string to_string(Id<Tag> id) {
    return hex64(id.value());
}

template <class Tag>
[[nodiscard]] std::string to_string(Gen<Tag> gen) {
    return hex64(gen.value());
}

template <class Tag>
[[nodiscard]] std::string to_decimal(Gen<Tag> gen) {
    return std::to_string(gen.value());
}

// Strict parser: exactly 16 hex digits, or 0x-prefixed / bare decimal when
// allow_decimal is set. Rejects everything else, including trailing bytes.
template <class Tag>
[[nodiscard]] std::optional<Id<Tag>> parse_id(std::string_view text, bool allow_decimal = false) {
    if (text.size() == 16) {
        std::uint64_t value = 0;
        bool hex_ok = true;
        for (char c : text) {
            std::uint64_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint64_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint64_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint64_t>(c - 'A' + 10);
            } else {
                hex_ok = false;
                break;
            }
            value = (value << 4) | digit;
        }
        if (hex_ok) {
            return Id<Tag>{value};
        }
    }
    if (allow_decimal && !text.empty()) {
        std::uint64_t value = 0;
        bool ok = true;
        for (char c : text) {
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
            if (value > (0xFFFFFFFFFFFFFFFFULL - digit) / 10ULL) {
                ok = false;
                break;
            }
            value = value * 10ULL + digit;
        }
        if (ok) {
            return Id<Tag>{value};
        }
    }
    return std::nullopt;
}

template <class Tag>
[[nodiscard]] std::optional<Gen<Tag>> parse_gen(std::string_view text, bool allow_decimal = true) {
    const auto id = parse_id<Tag>(text, allow_decimal);
    if (!id.has_value()) {
        return std::nullopt;
    }
    return Gen<Tag>{id->value()};
}

// ---------------------------------------------------------------------------
// Canonical composition helpers. Used to derive deterministic identities from
// canonical state instead of from counters or randomness.
// ---------------------------------------------------------------------------
template <class Tag>
inline HashBuilder& operator<<(HashBuilder& h, Id<Tag> id) {
    h.u64(id.value());
    return h;
}

template <class Tag>
inline HashBuilder& operator<<(HashBuilder& h, Gen<Tag> gen) {
    h.u64(gen.value());
    return h;
}

}  // namespace pef
