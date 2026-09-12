// Persistent Execution Fabric - execution authority runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Runtime is the coordinator's state machine. It owns durable execution
// authority: which execution may continue, from which durable point, under
// whose current authority. It is independent of networking so that the same
// code path is exercised by tests, by the CLI, and by the coordinator server.
//
// Durability rule enforced throughout: a mutation is computed as a new record,
// appended to the journal, and only then installed in memory. Memory therefore
// never contains state that a fresh coordinator could not recover.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pef/audit.hpp"
#include "pef/persistence.hpp"
#include "pef/policy.hpp"
#include "pef/records.hpp"
#include "pef/recovery.hpp"
#include "pef/status.hpp"

namespace pef {

// Authority carried by every worker operation. Every field is checked against
// durable state, and each mismatch has its own refusal code.
struct LeaseToken {
    ExecutionId execution;
    ExecutionGeneration execution_generation;
    ExecutionIncarnationId incarnation;
    ExecutionIncarnationGeneration incarnation_generation;
    LeaseId lease;
    LeaseGeneration lease_generation;
    CoordinatorEpoch epoch;
    WorkerId worker;
    WorkerBootId boot;
    PolicyGeneration policy_generation;
};

// Coordinator-side caller identity. Sessions are process-local and are never
// persisted; a restart invalidates all of them.
struct CallerContext {
    SessionId session;
    CoordinatorEpoch epoch;
};

struct RuntimeConfig {
    std::filesystem::path store_path;
    bool create_if_missing = false;
    StoreLimits store_limits{};
    std::size_t max_executions = 100000;
    std::size_t request_cache_size = 4096;
    std::size_t max_view_actions = 256;
};

struct OpenOutcome {
    CoordinatorEpoch previous_epoch;
    CoordinatorEpoch epoch;
    bool created = false;
    LoadReport load;
    std::vector<ExecutionId> reclassified;
    std::uint64_t leases_revoked = 0;
    std::uint64_t executions_reclassified = 0;
    // Commits whose detail records were durable but whose aggregate update was
    // lost to a torn journal tail, and which recovery brought forward.
    std::uint64_t reconciled_commits = 0;
    std::string detail;
};

struct CreateExecutionRequest {
    RequestId request;
    ExecutionPolicy policy;
};

struct CreateExecutionResult {
    ExecutionId execution;
    ExecutionGeneration generation;
    PolicyId policy;
    PolicyGeneration policy_generation;
    bool existing = false;
};

struct BindWorkerRequest {
    RequestId request;
    ExecutionId execution;
    WorkerId worker;
    WorkerBootId boot;
    BindingSet bindings;
};

struct BindWorkerResult {
    ExecutionIncarnationId incarnation;
    ExecutionIncarnationGeneration incarnation_generation;
    LeaseId lease;
    LeaseGeneration lease_generation;
    Lifecycle lifecycle = Lifecycle::Created;
    bool incarnation_advanced = false;
};

struct StartRequest {
    RequestId request;
    LeaseToken token;
};

struct BeginActionRequest {
    RequestId request;
    LeaseToken token;
    SideEffectClass effect_class = SideEffectClass::Unknown;
    std::string request_key;
    Evidence evidence;
};

struct BeginActionResult {
    ActionId action;
    ActionGeneration action_generation;
    std::uint64_t ordinal = 0;
    AttemptId attempt;
    AttemptGeneration attempt_generation;
    SideEffectId effect_id;
    SideEffectGeneration effect_generation;
};

struct CompletionEvidence {
    // The worker's durable statement about the physical effect.
    bool effect_applied = true;
    Evidence evidence;
};

struct CompleteActionRequest {
    RequestId request;
    LeaseToken token;
    ActionId action;
    ActionGeneration action_generation;
    CompletionEvidence completion;
};

struct CompleteActionResult {
    CommitId commit;
    ProgressId progress;
    ProgressGeneration progress_generation;
    std::uint64_t ordinal = 0;
    bool duplicate = false;
    bool checkpoint_due = false;
};

struct FailActionRequest {
    RequestId request;
    LeaseToken token;
    ActionId action;
    ActionGeneration action_generation;
    Evidence evidence;
    std::string reason;
};

struct ReportSideEffectRequest {
    RequestId request;
    LeaseToken token;
    ActionId action;
    ActionGeneration action_generation;
    bool applied = false;
    Evidence evidence;
};

struct RegisterCheckpointRequest {
    RequestId request;
    LeaseToken token;
    // The caller's belief about the current checkpoint. A mismatch is refused.
    CheckpointId expected_checkpoint;
    CheckpointGeneration expected_checkpoint_generation;
    std::uint64_t content_size = 0;
    std::uint64_t content_hash = 0;
    std::uint64_t effect_boundary_ordinal = 0;
};

struct RegisterCheckpointResult {
    CheckpointId checkpoint;
    CheckpointGeneration checkpoint_generation;
    CheckpointState state = CheckpointState::Created;
    ContinuationId continuation;
    ContinuationGeneration continuation_generation;
    std::uint32_t lineage_depth = 0;
};

struct ResolveAmbiguityRequest {
    RequestId request;
    CallerContext caller;
    ExecutionId execution;
    AmbiguityId ambiguity;
    AmbiguityGeneration ambiguity_generation;
    AmbiguityState resolution = AmbiguityState::ManualResolutionRequired;
    Evidence evidence;
    std::string note;
};

struct ResolveAmbiguityResult {
    AmbiguityState state = AmbiguityState::Open;
    bool committed = false;
    CommitId commit;
    ProgressGeneration progress_generation;
    // True when the resolution makes the interrupted action re-drivable under a
    // new generation of the same logical action identity.
    bool replay_admitted = false;
    ReplayId replay;
    AmbiguityId ambiguity;
    AmbiguityGeneration ambiguity_generation;
    ActionId action;
    ActionGeneration action_generation;
};

struct RecoveryOutcome {
    RecoveryPlan plan;
    bool applied = false;
    bool committed = false;
    CommitId commit;
    RecoveryId recovery;
    RecoveryGeneration recovery_generation;
    AmbiguityId ambiguity;
    AmbiguityGeneration ambiguity_generation;
    ContinuationId continuation;
    ContinuationGeneration continuation_generation;
};

struct ResumeRequest {
    RequestId request;
    LeaseToken token;
    // Bindings the worker can currently offer. Compared against the durable
    // binding set; a mismatch is refused unless revalidate is set.
    BindingSet observed_bindings;
    bool revalidate_bindings = false;
};

struct ResumeResult {
    ContinuationState continuation_state = ContinuationState::Invalid;
    Lifecycle lifecycle = Lifecycle::Created;
    RecoveryDecision decision = RecoveryDecision::Unsupported;
};

struct ExecutionView {
    bool found = false;
    ExecutionRecord execution;
    ExecutionPolicy policy;
    LeaseRecord lease;
    ContinuationRecord continuation;
    CheckpointRecord checkpoint;
    RecoveryRecord recovery;
    AmbiguityRecord ambiguity;
    std::vector<ActionRecord> actions;          // newest first, bounded
    std::vector<CheckpointRecord> checkpoints;  // newest first
    std::vector<ReplayRecord> replays;
    std::uint64_t action_count = 0;
    std::uint64_t commit_count = 0;
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Opens the store, advances the coordinator epoch, invalidates every
    // process-local session and every lease from the previous epoch, and
    // reclassifies executions that were active when the previous coordinator
    // stopped. Never revives RUNNING state on the strength of durable bytes.
    [[nodiscard]] Status open(const RuntimeConfig& config, OpenOutcome& outcome);
    [[nodiscard]] Status shutdown();

    [[nodiscard]] bool is_open() const noexcept;

    // ---- process-local session management ---------------------------------
    [[nodiscard]] Status begin_session(WorkerId worker, WorkerBootId boot, CallerContext& out,
                                       SessionId& out_session);
    [[nodiscard]] Status end_session(SessionId session);

    // Coordinator-level operations.
    [[nodiscard]] Status create_execution(const CallerContext& caller,
                                          const CreateExecutionRequest& request,
                                          CreateExecutionResult& out);
    [[nodiscard]] Status update_policy(const CallerContext& caller, RequestId request,
                                       ExecutionId execution, ExecutionPolicy policy,
                                       bool apply);
    [[nodiscard]] Status list_executions(std::vector<ExecutionRecord>& out) const;
    [[nodiscard]] Status query(ExecutionId execution, ExecutionView& out) const;
    [[nodiscard]] Status fence(const CallerContext& caller, RequestId request, ExecutionId execution,
                               std::string reason);
    [[nodiscard]] Status retire(const CallerContext& caller, RequestId request, ExecutionId execution,
                                std::string reason);
    [[nodiscard]] Status cancel(const CallerContext& caller, RequestId request, ExecutionId execution,
                                std::string reason);
    [[nodiscard]] Status drain(const CallerContext& caller, RequestId request, ExecutionId execution);
    [[nodiscard]] Status complete_execution(const CallerContext& caller, RequestId request,
                                            ExecutionId execution);
    [[nodiscard]] Status advance_execution_generation(const CallerContext& caller, RequestId request,
                                                      ExecutionId execution, std::string reason);
    [[nodiscard]] Status suspend_begin(const CallerContext& caller, RequestId request,
                                       ExecutionId execution);
    [[nodiscard]] Status suspend_commit(const CallerContext& caller, RequestId request,
                                        ExecutionId execution);
    [[nodiscard]] Status classify(const CallerContext& caller, ExecutionId execution,
                                  RecoveryPlan& out);
    [[nodiscard]] Status recover(const CallerContext& caller, RequestId request, ExecutionId execution,
                                 RecoveryOutcome& out);
    [[nodiscard]] Status resolve_ambiguity(const ResolveAmbiguityRequest& request,
                                           ResolveAmbiguityResult& out);
    [[nodiscard]] Status validate_continuation(const CallerContext& caller, ExecutionId execution,
                                               ContinuationState& out, std::string& reason) const;

    // ---- worker operations ------------------------------------------------
    [[nodiscard]] Status bind_worker(const CallerContext& caller, const BindWorkerRequest& request,
                                     BindWorkerResult& out);
    [[nodiscard]] Status start(const StartRequest& request);
    [[nodiscard]] Status begin_action(const BeginActionRequest& request, BeginActionResult& out);
    [[nodiscard]] Status complete_action(const CompleteActionRequest& request,
                                         CompleteActionResult& out);
    [[nodiscard]] Status fail_action(const FailActionRequest& request);
    [[nodiscard]] Status report_side_effect(const ReportSideEffectRequest& request);
    [[nodiscard]] Status register_checkpoint(const RegisterCheckpointRequest& request,
                                             RegisterCheckpointResult& out);
    [[nodiscard]] Status resume(const ResumeRequest& request, ResumeResult& out);

    // ---- maintenance ------------------------------------------------------
    [[nodiscard]] Status take_snapshot(bool truncate_journal);
    [[nodiscard]] Status audit(AuditReport& out) const;
    [[nodiscard]] Status flush();

    [[nodiscard]] CoordinatorEpoch epoch() const;
    [[nodiscard]] StoreId store_id() const;
    [[nodiscard]] const std::filesystem::path& store_path() const noexcept { return config_.store_path; }
    [[nodiscard]] std::uint64_t journal_sequence() const;
    // Number of live process-local sessions. Zero after a restart.
    [[nodiscard]] std::size_t session_count() const;

private:
    // Requires the runtime lock to be held by the caller. Used by open() during
    // coordinator restart recovery, where the lock is already held.
    [[nodiscard]] Status recover_locked(const CallerContext& caller, RequestId request,
                                        ExecutionId execution, RecoveryOutcome& out);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    RuntimeConfig config_;
};

// Validates a continuation record against durable state. Pure function of the
// durable image: no clocks, no process identity, no iteration order.
[[nodiscard]] Status validate_continuation_state(const DurableState& state,
                                                 const ContinuationRecord& continuation,
                                                 ContinuationState& out_state,
                                                 std::string& out_reason);

// Generates a worker boot id that cannot be confused with a previous boot of
// the same worker, even when the operating system reuses a process id.
[[nodiscard]] WorkerBootId mint_worker_boot_id(WorkerId worker);
// Generates a worker id from a stable caller-supplied name.
[[nodiscard]] WorkerId derive_worker_id(std::string_view name);

}  // namespace pef
