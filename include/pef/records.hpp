// Persistent Execution Fabric - durable record types.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Every record here is durable state. Field order in RecordTraits is the
// canonical durable order; changing it is a schema change.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pef/codec.hpp"
#include "pef/identity.hpp"
#include "pef/lifecycle.hpp"
#include "pef/policy.hpp"
#include "pef/side_effect.hpp"

namespace pef {

// ---------------------------------------------------------------------------
// Enumerations carried by durable records.
// ---------------------------------------------------------------------------
enum class BindingDomain : std::uint8_t {
    Memory = 0,
    Model = 1,
    Tool = 2,
    Resource = 3,
    Environment = 4,
    Filesystem = 5,
    Hardware = 6,
    Custom = 7,
};

[[nodiscard]] std::string_view binding_domain_name(BindingDomain domain) noexcept;
[[nodiscard]] std::optional<BindingDomain> parse_binding_domain(std::string_view text) noexcept;
constexpr bool pef_valid_enum(BindingDomain v) noexcept {
    return static_cast<std::uint8_t>(v) <= 7;
}

enum class CheckpointState : std::uint8_t {
    // Bytes were registered but integrity and generations are unchecked.
    Created = 0,
    // Integrity verified, generations checked against the execution.
    Verified = 1,
    // The newest checkpoint that a continuation may currently be derived from.
    Current = 2,
    // A newer checkpoint exists and policy does not allow falling back.
    Stale = 3,
    // Explicitly replaced by a later checkpoint.
    Superseded = 4,
    // Integrity check failed.
    Corrupt = 5,
    // A referenced generation moved; explicit revalidation is required.
    RevalidationRequired = 6,
    // The checkpoint declares a feature this build cannot interpret.
    Unsupported = 7,
};

[[nodiscard]] std::string_view checkpoint_state_name(CheckpointState state) noexcept;
[[nodiscard]] std::optional<CheckpointState> parse_checkpoint_state(std::string_view text) noexcept;
constexpr bool pef_valid_enum(CheckpointState v) noexcept {
    return static_cast<std::uint8_t>(v) <= 7;
}

// True when this checkpoint state may seed a continuation.
[[nodiscard]] bool checkpoint_can_seed_continuation(CheckpointState state) noexcept;

enum class ContinuationState : std::uint8_t {
    Valid = 0,
    Stale = 1,
    RevalidationRequired = 2,
    Ambiguous = 3,
    Unsupported = 4,
    Invalid = 5,
};

[[nodiscard]] std::string_view continuation_state_name(ContinuationState state) noexcept;
[[nodiscard]] std::optional<ContinuationState> parse_continuation_state(std::string_view text) noexcept;
constexpr bool pef_valid_enum(ContinuationState v) noexcept {
    return static_cast<std::uint8_t>(v) <= 5;
}

enum class LeaseState : std::uint8_t {
    Active = 0,
    Superseded = 1,
    Revoked = 2,
};

[[nodiscard]] std::string_view lease_state_name(LeaseState state) noexcept;
[[nodiscard]] std::optional<LeaseState> parse_lease_state(std::string_view text) noexcept;
constexpr bool pef_valid_enum(LeaseState v) noexcept {
    return static_cast<std::uint8_t>(v) <= 2;
}

enum class ReplayStatus : std::uint8_t {
    // Replay intent recorded durably; the action may be re-driven.
    Admitted = 0,
    // Replay was refused; the record states why by carrying no admission.
    Rejected = 1,
    // The replayed action reached a durable commit.
    Completed = 2,
};

[[nodiscard]] std::string_view replay_status_name(ReplayStatus status) noexcept;
[[nodiscard]] std::optional<ReplayStatus> parse_replay_status(std::string_view text) noexcept;
constexpr bool pef_valid_enum(ReplayStatus v) noexcept {
    return static_cast<std::uint8_t>(v) <= 2;
}

constexpr bool pef_valid_enum(AmbiguityHandling v) noexcept {
    return static_cast<std::uint8_t>(v) <= 2;
}
constexpr bool pef_valid_enum(RecoveryPreference v) noexcept {
    return static_cast<std::uint8_t>(v) <= 2;
}
constexpr bool pef_valid_enum(StaleBindingBehavior v) noexcept {
    return static_cast<std::uint8_t>(v) <= 1;
}
constexpr bool pef_valid_enum(ReplayAllowance v) noexcept {
    return static_cast<std::uint8_t>(v) <= 2;
}
constexpr bool pef_valid_enum(DurabilityMode v) noexcept {
    return static_cast<std::uint8_t>(v) <= 1;
}
constexpr bool pef_valid_enum(Lifecycle v) noexcept {
    return static_cast<std::uint8_t>(v) <= 14;
}
constexpr bool pef_valid_enum(SideEffectClass v) noexcept {
    return static_cast<std::uint8_t>(v) <= 5;
}
constexpr bool pef_valid_enum(EvidenceKind v) noexcept {
    return static_cast<std::uint8_t>(v) <= 3;
}
constexpr bool pef_valid_enum(ActionStatus v) noexcept {
    return static_cast<std::uint8_t>(v) <= 9;
}
constexpr bool pef_valid_enum(AmbiguityState v) noexcept {
    return static_cast<std::uint8_t>(v) <= 5;
}

// ---------------------------------------------------------------------------
// Evidence provenance.
// ---------------------------------------------------------------------------
struct Evidence {
    EvidenceId id;
    EvidenceGeneration generation;
    EvidenceKind kind = EvidenceKind::None;
    // Free-form origin label, e.g. "tcp-loopback", "msvc-process-kill",
    // "operator", "external-receipt". Bounded, never parsed for decisions.
    std::string source;
    // Monotonic logical observation ordinal. Never a wall clock.
    std::uint64_t observed_sequence = 0;
    // Digest of the observed payload, when one exists.
    std::uint64_t observed_digest = 0;

    [[nodiscard]] bool present() const noexcept { return kind != EvidenceKind::None; }
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, Evidence& out);
};

// ---------------------------------------------------------------------------
// External bindings referenced by an execution.
// ---------------------------------------------------------------------------
struct BindingRef {
    BindingDomain domain = BindingDomain::Custom;
    BindingId id;
    BindingGeneration generation;
    // Human label for the bound object. Bounded, never parsed for decisions.
    std::string label;

    friend bool operator==(const BindingRef& a, const BindingRef& b);
    friend bool operator<(const BindingRef& a, const BindingRef& b);
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, BindingRef& out);
};

// A canonical, sorted set of binding generations. Ordering is by
// (domain, id, generation) so that two calls describing the same dependency
// set always encode to identical bytes.
struct BindingSet {
    std::vector<BindingRef> refs;

    void canonicalize();
    [[nodiscard]] bool contains(BindingId id) const;
    [[nodiscard]] const BindingRef* find(BindingId id) const;
    [[nodiscard]] bool empty() const noexcept { return refs.empty(); }
    // Compares the generation of every binding present in both, and reports
    // bindings that are required by this set but missing from other.
    [[nodiscard]] bool equals_generations(const BindingSet& other) const;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, BindingSet& out);
};

// ---------------------------------------------------------------------------
// Action record. Physical outcome and logical completion are separate fields.
// ---------------------------------------------------------------------------
struct ActionRecord {
    ActionId id;
    ActionGeneration generation;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    ExecutionIncarnationGeneration incarnation_generation;
    // Monotone per-execution ordinal. Determines the progress frontier.
    std::uint64_t sequence = 0;
    AttemptId attempt;
    AttemptGeneration attempt_generation;
    SideEffectClass effect_class = SideEffectClass::Unknown;
    // Stable external request/idempotency key. Required for
    // RepeatableWithKey and honoured for every other class as well.
    std::string request_key;
    ActionStatus status = ActionStatus::Prepared;
    SideEffectId effect_id;
    SideEffectGeneration effect_generation;
    // Durable external receipt describing whether the physical effect applied.
    Evidence receipt;
    ReplayGeneration replay_generation;
    // Set when the action was admitted as a replay of an earlier generation.
    ActionId replayed_from;
    ActionGeneration replayed_from_generation;
    CommitId commit;
    // Lifetime of the attempt: each physical dispatch advances this.
    std::uint32_t dispatch_count = 0;

    [[nodiscard]] bool sealed() const noexcept { return action_is_physically_sealed(status); }
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ActionRecord& out);
};

// A logical action is identified by its ActionId. Attempts to re-drive that same
// logical action advance its ActionGeneration. Durable records are keyed by the
// pair so that replay history is never overwritten by a newer generation.
struct ActionKey {
    ActionId id;
    ActionGeneration generation;

    friend bool operator==(const ActionKey& a, const ActionKey& b) {
        return a.id == b.id && a.generation == b.generation;
    }
    friend bool operator<(const ActionKey& a, const ActionKey& b) {
        if (a.id != b.id) {
            return a.id < b.id;
        }
        return a.generation < b.generation;
    }
};

// ---------------------------------------------------------------------------
// Durable progress. Only a committed progress record is authoritative.
// ---------------------------------------------------------------------------
struct ProgressRecord {
    ProgressId id;
    ProgressGeneration generation;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    ActionId action;
    ActionGeneration action_generation;
    CommitId commit;
    CoordinatorEpoch epoch;
    // Ordinal copied from the action so the frontier is checkable without a
    // second lookup.
    std::uint64_t ordinal = 0;
    bool committed = false;
    // Sequence in the durable journal at which this record was written. Used
    // for deterministic ordering, never a clock.
    std::uint64_t durable_sequence = 0;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ProgressRecord& out);
};

// ---------------------------------------------------------------------------
// Checkpoint. Existence of bytes is not resumability.
// ---------------------------------------------------------------------------
struct CheckpointRecord {
    CheckpointId id;
    CheckpointGeneration generation;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    ExecutionIncarnationGeneration incarnation_generation;
    ProgressGeneration progress_generation;
    ActionGeneration action_generation;
    PolicyId policy;
    PolicyGeneration policy_generation;
    BindingSet bindings;
    CoordinatorEpoch epoch;
    WorkerId worker;
    WorkerBootId boot;

    // Lineage. A checkpoint without a consistent parent chain is not usable.
    CheckpointId parent;
    CheckpointGeneration parent_generation;
    std::uint32_t lineage_depth = 0;

    // Side-effect boundary: the action ordinal below which every effect is
    // durably accounted for.
    std::uint64_t effect_boundary_ordinal = 0;
    ActionId boundary_action;

    // Content metadata. The fabric stores the description, not the bytes.
    std::uint64_t content_size = 0;
    std::uint64_t content_hash = 0;
    CheckpointState state = CheckpointState::Created;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, CheckpointRecord& out);
};

// ---------------------------------------------------------------------------
// Continuation: the durable statement that execution may continue from a point.
// ---------------------------------------------------------------------------
struct ContinuationRecord {
    ContinuationId id;
    ContinuationGeneration generation;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    ExecutionIncarnationId incarnation;
    ExecutionIncarnationGeneration incarnation_generation;
    CheckpointId checkpoint;
    CheckpointGeneration checkpoint_generation;
    ProgressGeneration progress_generation;
    ActionGeneration action_generation;
    PolicyId policy;
    PolicyGeneration policy_generation;
    BindingSet bindings;
    CoordinatorEpoch epoch;
    RecoveryId recovery;
    RecoveryGeneration recovery_generation;
    ContinuationState state = ContinuationState::Invalid;
    // The ordinal from which execution continues.
    std::uint64_t resume_ordinal = 0;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ContinuationRecord& out);
};

// ---------------------------------------------------------------------------
// Lease: continuation authority bound to one worker boot and one epoch.
// ---------------------------------------------------------------------------
struct LeaseRecord {
    LeaseId id;
    LeaseGeneration generation;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    ExecutionIncarnationId incarnation;
    ExecutionIncarnationGeneration incarnation_generation;
    WorkerId worker;
    WorkerBootId boot;
    CoordinatorEpoch epoch;
    PolicyGeneration policy_generation;
    BindingSet bindings;
    LeaseState state = LeaseState::Active;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, LeaseRecord& out);
};

// ---------------------------------------------------------------------------
// Replay: an explicit, generation-bound intent to re-drive an action.
// ---------------------------------------------------------------------------
struct ReplayRecord {
    ReplayId id;
    ReplayGeneration generation;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    ActionId action;
    ActionGeneration action_generation;
    ActionId new_action;
    ActionGeneration new_action_generation;
    SideEffectClass effect_class = SideEffectClass::Unknown;
    std::string request_key;
    ProgressGeneration progress_generation;
    CheckpointGeneration checkpoint_generation;
    PolicyGeneration policy_generation;
    CoordinatorEpoch epoch;
    ReplayStatus status = ReplayStatus::Rejected;
    Evidence evidence;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ReplayRecord& out);
};

// ---------------------------------------------------------------------------
// Ambiguity: the durable admission that the runtime does not know.
// ---------------------------------------------------------------------------
struct AmbiguityRecord {
    AmbiguityId id;
    AmbiguityGeneration generation;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    ActionId action;
    ActionGeneration action_generation;
    SideEffectClass effect_class = SideEffectClass::Unknown;
    std::uint64_t action_ordinal = 0;
    AmbiguityState state = AmbiguityState::Open;
    // Why the runtime could not decide.
    std::string reason;
    // Evidence that resolves it, when one exists.
    Evidence resolution_evidence;
    std::string resolution_note;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, AmbiguityRecord& out);
};

// ---------------------------------------------------------------------------
// Logical commit identity. One per (action identity, progress generation).
// ---------------------------------------------------------------------------
struct CommitRecord {
    CommitId id;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    ActionId action;
    ActionGeneration action_generation;
    ProgressId progress;
    ProgressGeneration progress_generation;
    CoordinatorEpoch epoch;
    RequestId request;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, CommitRecord& out);
};

// ---------------------------------------------------------------------------
// Execution: the durable object everything else hangs from.
// ---------------------------------------------------------------------------
struct ExecutionRecord {
    ExecutionId id;
    ExecutionGeneration generation;
    ExecutionIncarnationId incarnation;
    ExecutionIncarnationGeneration incarnation_generation;

    Lifecycle lifecycle = Lifecycle::Created;

    ContinuationId continuation;
    ContinuationGeneration continuation_generation;
    ProgressId progress;
    ProgressGeneration progress_generation;
    ActionGeneration action_generation;
    // Ordinal of the newest action identity allocated.
    std::uint64_t action_frontier = 0;
    // Highest ordinal for which progress may still be committed. Set when an
    // unresolved ambiguity blocks the frontier.
    std::uint64_t blocked_above_ordinal = 0;
    bool blocked = false;

    CheckpointId checkpoint;
    CheckpointGeneration checkpoint_generation;

    PolicyId policy;
    PolicyGeneration policy_generation;
    CoordinatorEpoch epoch;

    WorkerId worker;
    WorkerBootId boot;
    LeaseId lease;
    LeaseGeneration lease_generation;

    CommitId last_commit;
    RecoveryId recovery;
    RecoveryGeneration recovery_generation;

    AmbiguityId ambiguity;
    AmbiguityGeneration ambiguity_generation;

    BindingSet bindings;

    std::uint64_t committed_actions = 0;
    std::uint64_t replay_count = 0;
    // Durable counter used to derive deterministic child identities.
    std::uint64_t created_sequence = 0;

    [[nodiscard]] bool has_worker() const noexcept { return worker.valid(); }
    [[nodiscard]] bool has_lease() const noexcept { return lease.valid(); }
    [[nodiscard]] bool ambiguity_open() const noexcept { return ambiguity.valid(); }

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ExecutionRecord& out);
};

// ---------------------------------------------------------------------------
// Recovery attempt.
// ---------------------------------------------------------------------------
struct RecoveryRecord {
    RecoveryId id;
    RecoveryGeneration generation;
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    // Numeric form of the decision, stored so the durable record is stable
    // even if the enum gains members.
    std::uint8_t decision = 0;
    ContinuationId continuation;
    ContinuationGeneration continuation_generation;
    CheckpointId checkpoint;
    CheckpointGeneration checkpoint_generation;
    CoordinatorEpoch epoch;
    Evidence evidence;
    std::string explanation;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, RecoveryRecord& out);
};

// ---------------------------------------------------------------------------
// RecordTraits declarations (canonical durable field order).
// ---------------------------------------------------------------------------
template <>
struct RecordTraits<ExecutionPolicy> {
    static constexpr auto members = std::make_tuple(
        &ExecutionPolicy::id, &ExecutionPolicy::generation,
        &ExecutionPolicy::checkpoint_interval_actions,
        &ExecutionPolicy::allow_older_checkpoints,
        &ExecutionPolicy::max_checkpoints_per_execution,
        &ExecutionPolicy::replay_allowance, &ExecutionPolicy::max_replay_depth,
        &ExecutionPolicy::allow_recompute, &ExecutionPolicy::ambiguity_handling,
        &ExecutionPolicy::ambiguity_blocks_later_progress,
        &ExecutionPolicy::recovery_preference,
        &ExecutionPolicy::allow_recovery_from_failed,
        &ExecutionPolicy::stale_binding_behavior,
        &ExecutionPolicy::policy_change_invalidates_continuations,
        &ExecutionPolicy::durability, &ExecutionPolicy::max_action_history,
        &ExecutionPolicy::max_replay_records, &ExecutionPolicy::max_ambiguity_records,
        &ExecutionPolicy::max_continuation_history);
};

template <>
struct RecordTraits<Evidence> {
    static constexpr auto members = std::make_tuple(
        &Evidence::id, &Evidence::generation, &Evidence::kind, &Evidence::source,
        &Evidence::observed_sequence, &Evidence::observed_digest);
};

template <>
struct RecordTraits<BindingRef> {
    static constexpr auto members = std::make_tuple(
        &BindingRef::domain, &BindingRef::id, &BindingRef::generation, &BindingRef::label);
};

template <>
struct RecordTraits<BindingSet> {
    static constexpr auto members = std::make_tuple(&BindingSet::refs);
};

template <>
struct RecordTraits<ActionRecord> {
    static constexpr auto members = std::make_tuple(
        &ActionRecord::id, &ActionRecord::generation, &ActionRecord::execution,
        &ActionRecord::execution_generation, &ActionRecord::incarnation_generation,
        &ActionRecord::sequence, &ActionRecord::attempt, &ActionRecord::attempt_generation,
        &ActionRecord::effect_class, &ActionRecord::request_key, &ActionRecord::status,
        &ActionRecord::effect_id, &ActionRecord::effect_generation, &ActionRecord::receipt,
        &ActionRecord::replay_generation, &ActionRecord::replayed_from,
        &ActionRecord::replayed_from_generation, &ActionRecord::commit,
        &ActionRecord::dispatch_count);
};

template <>
struct RecordTraits<ProgressRecord> {
    static constexpr auto members = std::make_tuple(
        &ProgressRecord::id, &ProgressRecord::generation, &ProgressRecord::execution,
        &ProgressRecord::execution_generation, &ProgressRecord::action,
        &ProgressRecord::action_generation, &ProgressRecord::commit, &ProgressRecord::epoch,
        &ProgressRecord::ordinal, &ProgressRecord::committed, &ProgressRecord::durable_sequence);
};

template <>
struct RecordTraits<CheckpointRecord> {
    static constexpr auto members = std::make_tuple(
        &CheckpointRecord::id, &CheckpointRecord::generation, &CheckpointRecord::execution,
        &CheckpointRecord::execution_generation, &CheckpointRecord::incarnation_generation,
        &CheckpointRecord::progress_generation, &CheckpointRecord::action_generation,
        &CheckpointRecord::policy, &CheckpointRecord::policy_generation,
        &CheckpointRecord::bindings, &CheckpointRecord::epoch, &CheckpointRecord::worker,
        &CheckpointRecord::boot, &CheckpointRecord::parent, &CheckpointRecord::parent_generation,
        &CheckpointRecord::lineage_depth, &CheckpointRecord::effect_boundary_ordinal,
        &CheckpointRecord::boundary_action, &CheckpointRecord::content_size,
        &CheckpointRecord::content_hash, &CheckpointRecord::state);
};

template <>
struct RecordTraits<ContinuationRecord> {
    static constexpr auto members = std::make_tuple(
        &ContinuationRecord::id, &ContinuationRecord::generation, &ContinuationRecord::execution,
        &ContinuationRecord::execution_generation, &ContinuationRecord::incarnation,
        &ContinuationRecord::incarnation_generation, &ContinuationRecord::checkpoint,
        &ContinuationRecord::checkpoint_generation, &ContinuationRecord::progress_generation,
        &ContinuationRecord::action_generation, &ContinuationRecord::policy,
        &ContinuationRecord::policy_generation, &ContinuationRecord::bindings,
        &ContinuationRecord::epoch, &ContinuationRecord::recovery,
        &ContinuationRecord::recovery_generation, &ContinuationRecord::state,
        &ContinuationRecord::resume_ordinal);
};

template <>
struct RecordTraits<LeaseRecord> {
    static constexpr auto members = std::make_tuple(
        &LeaseRecord::id, &LeaseRecord::generation, &LeaseRecord::execution,
        &LeaseRecord::execution_generation, &LeaseRecord::incarnation,
        &LeaseRecord::incarnation_generation, &LeaseRecord::worker, &LeaseRecord::boot,
        &LeaseRecord::epoch, &LeaseRecord::policy_generation, &LeaseRecord::bindings,
        &LeaseRecord::state);
};

template <>
struct RecordTraits<ReplayRecord> {
    static constexpr auto members = std::make_tuple(
        &ReplayRecord::id, &ReplayRecord::generation, &ReplayRecord::execution,
        &ReplayRecord::execution_generation, &ReplayRecord::action,
        &ReplayRecord::action_generation, &ReplayRecord::new_action,
        &ReplayRecord::new_action_generation, &ReplayRecord::effect_class,
        &ReplayRecord::request_key, &ReplayRecord::progress_generation,
        &ReplayRecord::checkpoint_generation, &ReplayRecord::policy_generation,
        &ReplayRecord::epoch, &ReplayRecord::status, &ReplayRecord::evidence);
};

template <>
struct RecordTraits<AmbiguityRecord> {
    static constexpr auto members = std::make_tuple(
        &AmbiguityRecord::id, &AmbiguityRecord::generation, &AmbiguityRecord::execution,
        &AmbiguityRecord::execution_generation, &AmbiguityRecord::action,
        &AmbiguityRecord::action_generation, &AmbiguityRecord::effect_class,
        &AmbiguityRecord::action_ordinal, &AmbiguityRecord::state, &AmbiguityRecord::reason,
        &AmbiguityRecord::resolution_evidence, &AmbiguityRecord::resolution_note);
};

template <>
struct RecordTraits<CommitRecord> {
    static constexpr auto members = std::make_tuple(
        &CommitRecord::id, &CommitRecord::execution, &CommitRecord::execution_generation,
        &CommitRecord::action, &CommitRecord::action_generation, &CommitRecord::progress,
        &CommitRecord::progress_generation, &CommitRecord::epoch, &CommitRecord::request);
};

template <>
struct RecordTraits<ExecutionRecord> {
    static constexpr auto members = std::make_tuple(
        &ExecutionRecord::id, &ExecutionRecord::generation, &ExecutionRecord::incarnation,
        &ExecutionRecord::incarnation_generation, &ExecutionRecord::lifecycle,
        &ExecutionRecord::continuation, &ExecutionRecord::continuation_generation,
        &ExecutionRecord::progress, &ExecutionRecord::progress_generation,
        &ExecutionRecord::action_generation, &ExecutionRecord::action_frontier,
        &ExecutionRecord::blocked_above_ordinal, &ExecutionRecord::blocked,
        &ExecutionRecord::checkpoint, &ExecutionRecord::checkpoint_generation,
        &ExecutionRecord::policy, &ExecutionRecord::policy_generation, &ExecutionRecord::epoch,
        &ExecutionRecord::worker, &ExecutionRecord::boot, &ExecutionRecord::lease,
        &ExecutionRecord::lease_generation, &ExecutionRecord::last_commit,
        &ExecutionRecord::recovery, &ExecutionRecord::recovery_generation,
        &ExecutionRecord::ambiguity, &ExecutionRecord::ambiguity_generation,
        &ExecutionRecord::bindings, &ExecutionRecord::committed_actions,
        &ExecutionRecord::replay_count, &ExecutionRecord::created_sequence);
};

template <>
struct RecordTraits<RecoveryRecord> {
    static constexpr auto members = std::make_tuple(
        &RecoveryRecord::id, &RecoveryRecord::generation, &RecoveryRecord::execution,
        &RecoveryRecord::execution_generation, &RecoveryRecord::decision,
        &RecoveryRecord::continuation, &RecoveryRecord::continuation_generation,
        &RecoveryRecord::checkpoint, &RecoveryRecord::checkpoint_generation,
        &RecoveryRecord::epoch, &RecoveryRecord::evidence, &RecoveryRecord::explanation);
};

// ---------------------------------------------------------------------------
// Canonical digest of a record, computed over its canonical encoding.
// ---------------------------------------------------------------------------
template <class T>
[[nodiscard]] std::uint64_t record_digest(const T& record) {
    const Bytes bytes = encode_to_bytes(record);
    std::uint64_t h = kFnvOffsetBasis;
    for (std::byte b : bytes) {
        h = fnv1a_step(h, static_cast<std::uint8_t>(b));
    }
    return h;
}

// ---------------------------------------------------------------------------
// Deterministic derived identities. These are pure functions of canonical
// state, so replaying the same durable history yields the same identities.
// ---------------------------------------------------------------------------
[[nodiscard]] CommitId derive_commit_id(ExecutionId execution, ActionId action,
                                        ActionGeneration action_generation,
                                        ProgressGeneration progress_generation) noexcept;
[[nodiscard]] SideEffectId derive_side_effect_id(ExecutionId execution, ActionId action,
                                                 ActionGeneration action_generation) noexcept;
[[nodiscard]] ReplayId derive_replay_id(ActionId action, ReplayGeneration generation) noexcept;
[[nodiscard]] ContinuationId derive_continuation_id(ExecutionId execution,
                                                    ExecutionIncarnationGeneration incarnation,
                                                    ProgressGeneration progress,
                                                    ContinuationGeneration generation) noexcept;
[[nodiscard]] LeaseId derive_lease_id(ExecutionId execution, WorkerId worker, WorkerBootId boot,
                                      LeaseGeneration generation) noexcept;
[[nodiscard]] CheckpointId derive_checkpoint_id(ExecutionId execution,
                                                CheckpointGeneration generation) noexcept;
[[nodiscard]] ProgressId derive_progress_id(ExecutionId execution,
                                            ProgressGeneration generation) noexcept;
[[nodiscard]] ActionId derive_action_id(ExecutionId execution, std::uint64_t ordinal) noexcept;
[[nodiscard]] AmbiguityId derive_ambiguity_id(ExecutionId execution, ActionId action,
                                              AmbiguityGeneration generation) noexcept;
[[nodiscard]] RecoveryId derive_recovery_id(ExecutionId execution,
                                            RecoveryGeneration generation) noexcept;
[[nodiscard]] ExecutionId derive_execution_id(StoreId store, std::uint64_t sequence) noexcept;
[[nodiscard]] ExecutionIncarnationId derive_incarnation_id(
    ExecutionId execution, ExecutionIncarnationGeneration generation) noexcept;
[[nodiscard]] PolicyId derive_policy_id(ExecutionId execution, PolicyGeneration generation) noexcept;
[[nodiscard]] AttemptId derive_attempt_id(ActionId action, AttemptGeneration generation) noexcept;
// Structural check of a binding set. Returns a reason when the set is not
// canonical or contains duplicate ids with conflicting generations.
[[nodiscard]] std::optional<std::string_view> binding_set_defect(const BindingSet& set) noexcept;

}  // namespace pef
