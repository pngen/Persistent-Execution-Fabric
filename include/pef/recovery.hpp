// Persistent Execution Fabric - recovery classification.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Classification is a pure function of durable state, current evidence, and
// policy. It does not consult the clock, the process table, or thread timing,
// so the same durable state always yields the same decision.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pef/records.hpp"

namespace pef {

enum class RecoveryDecision : std::uint8_t {
    // The durable progress frontier is itself a legal continuation point.
    ResumeFromCurrent = 0,
    // Resume from a durable checkpoint rather than the frontier.
    ResumeFromCheckpoint = 1,
    // An interrupted action may be re-driven without unsafe physical repetition.
    ReplaySafeAction = 2,
    // Required binding generations moved; the continuation must be revalidated
    // before it may become authoritative again.
    RevalidateBindings = 3,
    // The interrupted work may be recomputed from an earlier durable point.
    Recompute = 4,
    // The interrupted action must be compensated rather than repeated.
    Compensate = 5,
    // The physical outcome of an action cannot be determined.
    AmbiguousCompletion = 6,
    // The runtime refuses to decide; an operator or reconciler must.
    ManualResolutionRequired = 7,
    // The durable state describes something this build cannot continue.
    Unsupported = 8,
    // Nothing may continue.
    Terminal = 9,
};

inline constexpr std::uint8_t kRecoveryDecisionCount = 10;

[[nodiscard]] std::string_view recovery_decision_name(RecoveryDecision decision) noexcept;
[[nodiscard]] std::optional<RecoveryDecision> parse_recovery_decision(std::string_view text) noexcept;
[[nodiscard]] constexpr bool pef_valid_enum(RecoveryDecision v) noexcept {
    return static_cast<std::uint8_t>(v) <= 9;
}

// True when the decision permits execution to become authoritative again
// without an operator decision.
[[nodiscard]] bool recovery_allows_automatic_continue(RecoveryDecision decision) noexcept;

// True when the decision requires an explicit operator or reconciler action.
[[nodiscard]] bool recovery_requires_operator(RecoveryDecision decision) noexcept;

struct RecoveryInput {
    const ExecutionRecord* execution = nullptr;
    const ExecutionPolicy* policy = nullptr;
    // Coordinator epoch that is authoritative now.
    CoordinatorEpoch epoch;

    // Newest action that has not reached a sealed committed state, if any.
    const ActionRecord* in_flight = nullptr;
    // The execution's current checkpoint, when one exists.
    const CheckpointRecord* current_checkpoint = nullptr;
    // Seedable checkpoints in descending generation order.
    std::vector<const CheckpointRecord*> candidates;
    // Open ambiguity record, if any.
    const AmbiguityRecord* ambiguity = nullptr;

    // True when a required binding generation recorded on the execution no
    // longer matches the generation the caller can currently offer.
    bool binding_mismatch = false;
    // True when the execution references a checkpoint that is missing,
    // corrupt, or unsupported.
    bool checkpoint_unusable = false;
};

struct RecoveryPlan {
    RecoveryDecision decision = RecoveryDecision::Unsupported;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    CoordinatorEpoch epoch;
    ActionId action;
    ActionGeneration action_generation;
    CheckpointId checkpoint;
    CheckpointGeneration checkpoint_generation;
    ContinuationId continuation;
    ContinuationGeneration continuation_generation;
    AmbiguityId ambiguity;
    AmbiguityGeneration ambiguity_generation;
    // True when the coordinator may commit the logical progress of the
    // interrupted action from a durable side-effect receipt, without a worker.
    bool commit_from_receipt = false;
    // Deterministic, human-readable reason. Built from record fields only.
    std::string explanation;
};

[[nodiscard]] RecoveryPlan classify_recovery(const RecoveryInput& input);

}  // namespace pef
