// Persistent Execution Fabric - persistent execution policy.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Policy is durable state, not configuration read at startup. A continuation
// carries the policy generation it was derived under; when the policy moves on,
// the continuation is revalidated or refused rather than silently reinterpreted.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "pef/bytes.hpp"
#include "pef/identity.hpp"

namespace pef {

// What the runtime does when automatic replay of an interrupted action is
// unsafe.
enum class AmbiguityHandling : std::uint8_t {
    // Refuse automatic retry and require an explicit resolution or receipt.
    RefuseAutomaticRetry = 0,
    // Refuse automatic retry, open an ambiguity record, and require an operator
    // or reconciler decision.
    ManualResolutionRequired = 1,
    // Refuse automatic retry and require a compensation record for actions
    // whose class is Compensatable.
    RequireCompensation = 2,
};

// Which durable point recovery prefers when several are valid.
enum class RecoveryPreference : std::uint8_t {
    // Prefer the newest fully valid checkpoint.
    PreferNewestValidCheckpoint = 0,
    // Prefer the newest valid checkpoint at or before the interrupted action.
    PreferCheckpointBeforeInterruption = 1,
    // Refuse automatic selection and require an explicit continuation choice.
    RequireManualSelection = 2,
};

// What happens when a continuation references a binding whose generation moved.
enum class StaleBindingBehavior : std::uint8_t {
    // Refuse the continuation outright.
    Refuse = 0,
    // Mark the continuation REVALIDATION_REQUIRED; an explicit revalidation
    // with the new binding generations may then make it VALID again.
    RequireRevalidation = 1,
};

// What the runtime may do with an action interrupted in flight.
enum class ReplayAllowance : std::uint8_t {
    // Only actions whose class is automatically replay-safe may be re-driven.
    SafeClassesOnly = 0,
    // Additionally allow replay of Compensatable actions when a compensation
    // record is present.
    AllowCompensatedReplay = 1,
    // Forbid all replay; recovery must come from a checkpoint or be refused.
    ForbidReplay = 2,
};

// When an acknowledged transition becomes durable.
enum class DurabilityMode : std::uint8_t {
    // The transition is appended to the journal and flushed to stable storage
    // before the caller is told it succeeded. A fresh coordinator can always
    // recover it.
    DurableOnCommit = 0,
    // The transition is appended to the journal and is visible to readers, but
    // the acknowledgement does not wait for a device flush. The durability
    // point is the next explicit barrier (shutdown, snapshot, or an operator
    // request). Acknowledged transitions in this mode are recoverable only if
    // the host did not lose the write-back cache.
    JournaledNotFlushed = 1,
};

struct ExecutionPolicy {
    PolicyId id;
    PolicyGeneration generation;

    // --- checkpoints -------------------------------------------------------
    // Register a checkpoint automatically every N committed actions. 0 means
    // checkpoints are operator-driven only.
    std::uint32_t checkpoint_interval_actions = 0;
    // Whether a checkpoint older than the newest valid one may seed a
    // continuation.
    bool allow_older_checkpoints = false;
    // Maximum checkpoints retained per execution. Older ones are superseded and
    // eventually compacted.
    std::uint32_t max_checkpoints_per_execution = 8;

    // --- replay ------------------------------------------------------------
    ReplayAllowance replay_allowance = ReplayAllowance::SafeClassesOnly;
    // Maximum number of replay generations a single action identity may carry.
    std::uint32_t max_replay_depth = 4;
    // Whether a PURE action may be recomputed from an earlier durable point
    // instead of replayed in place.
    bool allow_recompute = true;

    // --- ambiguity ---------------------------------------------------------
    AmbiguityHandling ambiguity_handling = AmbiguityHandling::RefuseAutomaticRetry;
    // Whether an unresolved ambiguity blocks every later progress commit for
    // the execution. Turning this off is a deliberate risk acceptance and is
    // recorded as such.
    bool ambiguity_blocks_later_progress = true;

    // --- recovery ----------------------------------------------------------
    RecoveryPreference recovery_preference = RecoveryPreference::PreferNewestValidCheckpoint;
    // Whether a Failed execution may enter recovery at all.
    bool allow_recovery_from_failed = false;

    // --- generations -------------------------------------------------------
    StaleBindingBehavior stale_binding_behavior = StaleBindingBehavior::RequireRevalidation;
    // Whether a policy generation change invalidates continuations outright
    // (true) or merely forces revalidation (false).
    bool policy_change_invalidates_continuations = true;

    // --- durability --------------------------------------------------------
    DurabilityMode durability = DurabilityMode::DurableOnCommit;

    // --- retention ---------------------------------------------------------
    std::uint32_t max_action_history = 100000;
    std::uint32_t max_replay_records = 4096;
    std::uint32_t max_ambiguity_records = 1024;
    std::uint32_t max_continuation_history = 4096;

    // Canonical durable encoding. Field order is declared in records.hpp.
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ExecutionPolicy& out);
};

// Structural validation of a policy before it is accepted as durable state.
// Returns a human-readable reason when the policy is unusable.
[[nodiscard]] std::optional<std::string_view> validate_policy(const ExecutionPolicy& policy) noexcept;

// Canonical digest of the policy's decision-relevant fields.
[[nodiscard]] std::uint64_t policy_digest(const ExecutionPolicy& policy) noexcept;

// The conservative default policy used when a caller does not supply one.
[[nodiscard]] ExecutionPolicy default_policy(PolicyId id);

}  // namespace pef
