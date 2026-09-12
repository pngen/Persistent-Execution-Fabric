// Persistent Execution Fabric - durable records, canonical ordering, derived ids.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/records.hpp"

#include <algorithm>

namespace pef {
namespace {

// Domain salts keep identities derived in different domains from colliding even
// when the input tuple happens to be the same shape.
constexpr std::uint64_t kSaltCommit = 0x1001;
constexpr std::uint64_t kSaltSideEffect = 0x1002;
constexpr std::uint64_t kSaltReplay = 0x1003;
constexpr std::uint64_t kSaltContinuation = 0x1004;
constexpr std::uint64_t kSaltLease = 0x1005;
constexpr std::uint64_t kSaltCheckpoint = 0x1006;
constexpr std::uint64_t kSaltProgress = 0x1007;
constexpr std::uint64_t kSaltAction = 0x1008;
constexpr std::uint64_t kSaltAmbiguity = 0x1009;
constexpr std::uint64_t kSaltRecovery = 0x100a;
constexpr std::uint64_t kSaltExecution = 0x100b;
constexpr std::uint64_t kSaltIncarnation = 0x100c;
constexpr std::uint64_t kSaltPolicy = 0x100d;
constexpr std::uint64_t kSaltAttempt = 0x100e;

}  // namespace

#define PEF_RECORD_IO(Type)                                          \
    void Type::encode(ByteWriter& w) const { encode_record(w, *this); } \
    bool Type::decode(ByteReader& r, Type& out) { return decode_record(r, out); }

PEF_RECORD_IO(ExecutionPolicy)
PEF_RECORD_IO(Evidence)
PEF_RECORD_IO(BindingRef)
PEF_RECORD_IO(BindingSet)
PEF_RECORD_IO(ActionRecord)
PEF_RECORD_IO(ProgressRecord)
PEF_RECORD_IO(CheckpointRecord)
PEF_RECORD_IO(ContinuationRecord)
PEF_RECORD_IO(LeaseRecord)
PEF_RECORD_IO(ReplayRecord)
PEF_RECORD_IO(AmbiguityRecord)
PEF_RECORD_IO(CommitRecord)
PEF_RECORD_IO(ExecutionRecord)
PEF_RECORD_IO(RecoveryRecord)

#undef PEF_RECORD_IO

// ---------------------------------------------------------------------------
// Enum names.
// ---------------------------------------------------------------------------
std::string_view binding_domain_name(BindingDomain domain) noexcept {
    switch (domain) {
        case BindingDomain::Memory: return "MEMORY";
        case BindingDomain::Model: return "MODEL";
        case BindingDomain::Tool: return "TOOL";
        case BindingDomain::Resource: return "RESOURCE";
        case BindingDomain::Environment: return "ENVIRONMENT";
        case BindingDomain::Filesystem: return "FILESYSTEM";
        case BindingDomain::Hardware: return "HARDWARE";
        case BindingDomain::Custom: return "CUSTOM";
    }
    return "UNKNOWN_DOMAIN";
}

std::optional<BindingDomain> parse_binding_domain(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i <= 7; ++i) {
        const auto candidate = static_cast<BindingDomain>(i);
        if (binding_domain_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string_view checkpoint_state_name(CheckpointState state) noexcept {
    switch (state) {
        case CheckpointState::Created: return "CREATED";
        case CheckpointState::Verified: return "VERIFIED";
        case CheckpointState::Current: return "CURRENT";
        case CheckpointState::Stale: return "STALE";
        case CheckpointState::Superseded: return "SUPERSEDED";
        case CheckpointState::Corrupt: return "CORRUPT";
        case CheckpointState::RevalidationRequired: return "REVALIDATION_REQUIRED";
        case CheckpointState::Unsupported: return "UNSUPPORTED";
    }
    return "UNKNOWN_CHECKPOINT_STATE";
}

std::optional<CheckpointState> parse_checkpoint_state(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i <= 7; ++i) {
        const auto candidate = static_cast<CheckpointState>(i);
        if (checkpoint_state_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool checkpoint_can_seed_continuation(CheckpointState state) noexcept {
    return state == CheckpointState::Verified || state == CheckpointState::Current;
}

std::string_view continuation_state_name(ContinuationState state) noexcept {
    switch (state) {
        case ContinuationState::Valid: return "VALID";
        case ContinuationState::Stale: return "STALE";
        case ContinuationState::RevalidationRequired: return "REVALIDATION_REQUIRED";
        case ContinuationState::Ambiguous: return "AMBIGUOUS";
        case ContinuationState::Unsupported: return "UNSUPPORTED";
        case ContinuationState::Invalid: return "INVALID";
    }
    return "UNKNOWN_CONTINUATION_STATE";
}

std::optional<ContinuationState> parse_continuation_state(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i <= 5; ++i) {
        const auto candidate = static_cast<ContinuationState>(i);
        if (continuation_state_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string_view lease_state_name(LeaseState state) noexcept {
    switch (state) {
        case LeaseState::Active: return "ACTIVE";
        case LeaseState::Superseded: return "SUPERSEDED";
        case LeaseState::Revoked: return "REVOKED";
    }
    return "UNKNOWN_LEASE_STATE";
}

std::optional<LeaseState> parse_lease_state(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i <= 2; ++i) {
        const auto candidate = static_cast<LeaseState>(i);
        if (lease_state_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string_view replay_status_name(ReplayStatus status) noexcept {
    switch (status) {
        case ReplayStatus::Admitted: return "ADMITTED";
        case ReplayStatus::Rejected: return "REJECTED";
        case ReplayStatus::Completed: return "COMPLETED";
    }
    return "UNKNOWN_REPLAY_STATUS";
}

std::optional<ReplayStatus> parse_replay_status(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i <= 2; ++i) {
        const auto candidate = static_cast<ReplayStatus>(i);
        if (replay_status_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// BindingSet.
// ---------------------------------------------------------------------------
bool operator==(const BindingRef& a, const BindingRef& b) {
    return a.domain == b.domain && a.id == b.id && a.generation == b.generation &&
           a.label == b.label;
}

bool operator<(const BindingRef& a, const BindingRef& b) {
    if (a.domain != b.domain) {
        return static_cast<std::uint8_t>(a.domain) < static_cast<std::uint8_t>(b.domain);
    }
    if (a.id != b.id) {
        return a.id < b.id;
    }
    if (a.generation != b.generation) {
        return a.generation < b.generation;
    }
    return a.label < b.label;
}

void BindingSet::canonicalize() {
    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    // A binding id may appear at most once. When a caller supplies duplicates
    // with different generations, the lowest generation is the one whose
    // validity was actually established first; the duplicate is dropped here
    // and reported by binding_set_is_canonical().
    std::vector<BindingRef> deduped;
    deduped.reserve(refs.size());
    for (const auto& ref : refs) {
        if (!deduped.empty() && deduped.back().id == ref.id) {
            continue;
        }
        deduped.push_back(ref);
    }
    refs.swap(deduped);
}

bool BindingSet::contains(BindingId id) const {
    return find(id) != nullptr;
}

const BindingRef* BindingSet::find(BindingId id) const {
    for (const auto& ref : refs) {
        if (ref.id == id) {
            return &ref;
        }
    }
    return nullptr;
}

bool BindingSet::equals_generations(const BindingSet& other) const {
    for (const auto& ref : refs) {
        const BindingRef* match = other.find(ref.id);
        if (match == nullptr || match->generation != ref.generation) {
            return false;
        }
    }
    for (const auto& ref : other.refs) {
        const BindingRef* match = find(ref.id);
        if (match == nullptr || match->generation != ref.generation) {
            return false;
        }
    }
    return true;
}

std::optional<std::string_view> binding_set_defect(const BindingSet& set) noexcept {
    // Independent of the sorted order in the set, the raw refs must be free of
    // conflicting duplicate ids. canonicalize() drops them, so the check runs
    // against a copy of the incoming vector.
    std::vector<BindingRef> sorted = set.refs;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i - 1].id == sorted[i].id) {
            if (sorted[i - 1].generation != sorted[i].generation) {
                return "binding set has duplicate ids with conflicting generations";
            }
            return "binding set contains a duplicate binding id";
        }
    }
    for (const auto& ref : set.refs) {
        if (!ref.id.valid()) {
            return "binding set contains a nil binding id";
        }
        if (ref.generation.is_none()) {
            return "binding set contains an unestablished binding generation";
        }
        if (ref.label.size() > 256) {
            return "binding set label exceeds 256 bytes";
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Derived identities.
// ---------------------------------------------------------------------------
CommitId derive_commit_id(ExecutionId execution, ActionId action,
                          ActionGeneration action_generation,
                          ProgressGeneration progress_generation) noexcept {
    HashBuilder h;
    h.u64(kSaltCommit);
    h << execution << action << action_generation << progress_generation;
    return CommitId{h.digest()};
}

SideEffectId derive_side_effect_id(ExecutionId execution, ActionId action,
                                   ActionGeneration action_generation) noexcept {
    HashBuilder h;
    h.u64(kSaltSideEffect);
    h << execution << action << action_generation;
    return SideEffectId{h.digest()};
}

ReplayId derive_replay_id(ActionId action, ReplayGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltReplay);
    h << action << generation;
    return ReplayId{h.digest()};
}

ContinuationId derive_continuation_id(ExecutionId execution,
                                      ExecutionIncarnationGeneration incarnation,
                                      ProgressGeneration progress,
                                      ContinuationGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltContinuation);
    h << execution << incarnation << progress << generation;
    return ContinuationId{h.digest()};
}

LeaseId derive_lease_id(ExecutionId execution, WorkerId worker, WorkerBootId boot,
                        LeaseGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltLease);
    h << execution << worker << boot << generation;
    return LeaseId{h.digest()};
}

CheckpointId derive_checkpoint_id(ExecutionId execution, CheckpointGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltCheckpoint);
    h << execution << generation;
    return CheckpointId{h.digest()};
}

ProgressId derive_progress_id(ExecutionId execution, ProgressGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltProgress);
    h << execution << generation;
    return ProgressId{h.digest()};
}

ActionId derive_action_id(ExecutionId execution, std::uint64_t ordinal) noexcept {
    HashBuilder h;
    h.u64(kSaltAction);
    h << execution;
    h.u64(ordinal);
    return ActionId{h.digest()};
}

AmbiguityId derive_ambiguity_id(ExecutionId execution, ActionId action,
                                AmbiguityGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltAmbiguity);
    h << execution << action << generation;
    return AmbiguityId{h.digest()};
}

RecoveryId derive_recovery_id(ExecutionId execution, RecoveryGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltRecovery);
    h << execution << generation;
    return RecoveryId{h.digest()};
}

ExecutionId derive_execution_id(StoreId store, std::uint64_t sequence) noexcept {
    HashBuilder h;
    h.u64(kSaltExecution);
    h << store;
    h.u64(sequence);
    return ExecutionId{h.digest()};
}

ExecutionIncarnationId derive_incarnation_id(ExecutionId execution,
                                             ExecutionIncarnationGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltIncarnation);
    h << execution << generation;
    return ExecutionIncarnationId{h.digest()};
}

PolicyId derive_policy_id(ExecutionId execution, PolicyGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltPolicy);
    h << execution << generation;
    return PolicyId{h.digest()};
}

AttemptId derive_attempt_id(ActionId action, AttemptGeneration generation) noexcept {
    HashBuilder h;
    h.u64(kSaltAttempt);
    h << action << generation;
    return AttemptId{h.digest()};
}

}  // namespace pef
