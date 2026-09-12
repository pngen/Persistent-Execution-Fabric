// Persistent Execution Fabric - side-effect classification and evidence.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// The fabric does not understand domain-specific tool semantics. It understands
// only how safe it is to re-drive an action whose physical outcome may be
// unknown. Callers must classify their actions; the fabric refuses to guess.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace pef {

enum class SideEffectClass : std::uint8_t {
    // Recomputing produces the same result and leaves no external trace.
    Pure = 0,
    // Safe to repeat when the external system contract is idempotent.
    Idempotent = 1,
    // Safe to repeat only under the same idempotency/request key.
    RepeatableWithKey = 2,
    // Requires explicit compensation or rollback semantics.
    Compensatable = 3,
    // Automatic retry is forbidden after ambiguous completion.
    NonRepeatable = 4,
    // Unknown semantics. Automatic retry is forbidden.
    Unknown = 5,
};

inline constexpr std::uint8_t kSideEffectClassCount = 6;

[[nodiscard]] std::string_view side_effect_class_name(SideEffectClass cls) noexcept;
[[nodiscard]] std::optional<SideEffectClass> parse_side_effect_class(std::string_view text) noexcept;

// True when the fabric may re-drive the action without an external receipt.
[[nodiscard]] bool auto_replayable(SideEffectClass cls) noexcept;

// True when a stable request/idempotency key must be present before the action
// may be dispatched.
[[nodiscard]] bool requires_request_key(SideEffectClass cls) noexcept;

// True when automatic retry after ambiguous completion is forbidden outright.
[[nodiscard]] bool forbids_automatic_retry(SideEffectClass cls) noexcept;

// True when the action must be resolved by compensation rather than by replay.
[[nodiscard]] bool requires_compensation(SideEffectClass cls) noexcept;

// True when repeating the action physically is semantically harmless under the
// same logical identity.
[[nodiscard]] bool physically_repeatable(SideEffectClass cls) noexcept;

// ---------------------------------------------------------------------------
// Evidence provenance. Every durable record that carries an observation must
// label where the observation came from.
// ---------------------------------------------------------------------------
enum class EvidenceKind : std::uint8_t {
    None = 0,
    // Observed in this environment: real process, real socket, real storage,
    // real device.
    Real = 1,
    // Modelled or injected: the observation stands in for something the
    // environment cannot produce here.
    Synthetic = 2,
    // The environment cannot produce the observation at all.
    Unsupported = 3,
};

[[nodiscard]] std::string_view evidence_kind_name(EvidenceKind kind) noexcept;
[[nodiscard]] std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Action status. An action's physical outcome and its logical completion are
// separate facts and are recorded separately.
// ---------------------------------------------------------------------------
enum class ActionStatus : std::uint8_t {
    // Durable identity reserved; the action has not been dispatched.
    Prepared = 0,
    // Dispatched; no durable outcome recorded.
    InFlight = 1,
    // A durable external receipt states the physical effect applied.
    EffectApplied = 2,
    // A durable external receipt states the physical effect did not apply.
    EffectNotApplied = 3,
    // The action reached durable completion but its logical commit is not yet
    // recorded. Physical repetition is forbidden at this point.
    CompletedUnacknowledged = 4,
    // The logical progress commit is durable. Terminal for the action identity.
    Committed = 5,
    // Durable failure. Replay policy decides what happens next.
    Failed = 6,
    // Interrupted with unknown physical outcome. Terminal until resolved.
    Ambiguous = 7,
    // Compensation recorded.
    Compensated = 8,
    // Explicitly abandoned: the logical progress is forfeited deliberately.
    Abandoned = 9,
};

inline constexpr std::uint8_t kActionStatusCount = 10;

[[nodiscard]] std::string_view action_status_name(ActionStatus status) noexcept;
[[nodiscard]] std::optional<ActionStatus> parse_action_status(std::string_view text) noexcept;

// True when the action identity has reached a state from which it must never
// dispatch physically again.
[[nodiscard]] bool action_is_physically_sealed(ActionStatus status) noexcept;

// ---------------------------------------------------------------------------
// Ambiguity. First class, never inferred away.
// ---------------------------------------------------------------------------
enum class AmbiguityState : std::uint8_t {
    Open = 0,
    // Evidence established that the effect applied; logical completion proceeds.
    ResolvedApplied = 1,
    // Evidence established that the effect did not apply; replay may proceed
    // only if the side-effect class permits it.
    ResolvedNotApplied = 2,
    // Operator or system accepted the risk and advanced deliberately.
    Accepted = 3,
    // Progress through this action is deliberately forfeited.
    Abandoned = 4,
    // The runtime cannot resolve this and refuses to guess.
    ManualResolutionRequired = 5,
};

inline constexpr std::uint8_t kAmbiguityStateCount = 6;

[[nodiscard]] std::string_view ambiguity_state_name(AmbiguityState state) noexcept;
[[nodiscard]] std::optional<AmbiguityState> parse_ambiguity_state(std::string_view text) noexcept;
[[nodiscard]] bool ambiguity_is_open(AmbiguityState state) noexcept;

}  // namespace pef
