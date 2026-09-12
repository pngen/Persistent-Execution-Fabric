// Persistent Execution Fabric - execution authority runtime.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <sstream>

#include "pef/version.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pef {
namespace {

// Result of a previously seen request identity. Repeating a request must not
// repeat its durable effect.
struct CachedOutcome {
    Code code = Code::Internal;
    std::uint64_t a = 0;
    std::uint64_t b = 0;
};

struct SessionInfo {
    WorkerId worker;
    WorkerBootId boot;
};

[[nodiscard]] bool flush_for(const ExecutionPolicy* policy) noexcept {
    return policy == nullptr || policy->durability == DurabilityMode::DurableOnCommit;
}

template <class Rec>
[[nodiscard]] Status append_record(FileDurableStore& store, RecordKind kind, const Rec& record,
                                   bool flush) {
    ByteWriter writer(512);
    encode_record(writer, record);
    std::uint64_t sequence = 0;
    return store.append(kind, writer.bytes(), flush, sequence);
}

[[nodiscard]] Status require_transition(Lifecycle from, Lifecycle to, const char* what) {
    if (!is_legal_transition(from, to)) {
        std::string message = "illegal lifecycle transition for ";
        message += what;
        message += ": ";
        message += lifecycle_name(from);
        message += " -> ";
        message += lifecycle_name(to);
        return err(Code::LifecycleRefused, std::move(message));
    }
    return ok_status();
}

[[nodiscard]] std::string describe_execution(ExecutionId id) {
    return "execution " + hex64(id.value());
}

// Marks when the current process started. Combined with a per-process counter
// this makes a boot identity unique even when the operating system reuses a
// process id. This is identity mintage only: no recovery decision consults it.
[[nodiscard]] std::uint64_t process_boot_marker() {
#if defined(_WIN32)
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user) != 0) {
        return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
               static_cast<std::uint64_t>(created.dwLowDateTime);
    }
    return 0;
#else
    return static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
#endif
}

}  // namespace

struct Runtime::Impl {
    mutable std::mutex mu;
    FileDurableStore store;
    DurableState state;
    RuntimeConfig config;
    bool open = false;
    bool shutting_down = false;
    std::uint64_t next_session = 1;
    std::unordered_map<SessionId, SessionInfo, IdHasher<SessionIdTag>> sessions;
    std::unordered_map<RequestId, CachedOutcome, IdHasher<RequestIdTag>> request_cache;
    std::deque<RequestId> request_order;
    // Derived index: execution -> action keys in ascending ordinal order. It is
    // a pure function of durable state and is rebuilt on open.
    // Derived indexes: execution identity to the records that belong to it.
    // They are pure functions of durable state, rebuilt once on open, and
    // maintained in O(1) amortized on every write. Without them, a query or a
    // lookup would degrade to a scan of the whole store.
    std::unordered_map<ExecutionId, std::vector<ActionKey>, IdHasher<ExecutionIdTag>> action_index;
    std::unordered_map<ExecutionId, std::vector<CheckpointId>, IdHasher<ExecutionIdTag>>
        checkpoint_index;
    std::unordered_map<ExecutionId, std::vector<CommitId>, IdHasher<ExecutionIdTag>> commit_index;
    std::unordered_map<ExecutionId, std::vector<ReplayId>, IdHasher<ExecutionIdTag>> replay_index;

    void rebuild_indexes() {
        action_index.clear();
        checkpoint_index.clear();
        commit_index.clear();
        replay_index.clear();
        for (const auto& action : state.actions.insertion_order()) {
            action_index[action.execution].push_back(ActionKey{action.id, action.generation});
        }
        for (const auto& checkpoint : state.checkpoints.insertion_order()) {
            checkpoint_index[checkpoint.execution].push_back(checkpoint.id);
        }
        for (const auto& commit : state.commits.insertion_order()) {
            commit_index[commit.execution].push_back(commit.id);
        }
        for (const auto& replay : state.replays.insertion_order()) {
            replay_index[replay.execution].push_back(replay.id);
        }
        for (auto& entry : action_index) {
            std::sort(entry.second.begin(), entry.second.end(),
                      [](const ActionKey& a, const ActionKey& b) { return a < b; });
        }
    }

    // Action keys arrive in ascending ordinal order in the common case, so the
    // index is appended to rather than re-sorted on every insert.
    void index_action(const ActionRecord& action) {
        auto& keys = action_index[action.execution];
        const ActionKey key{action.id, action.generation};
        if (!keys.empty() && !(keys.back() < key)) {
            const auto position = std::lower_bound(keys.begin(), keys.end(), key);
            if (position != keys.end() && *position == key) {
                return;
            }
            keys.insert(position, key);
            return;
        }
        if (!keys.empty() && keys.back() == key) {
            return;
        }
        keys.push_back(key);
    }

    [[nodiscard]] const ActionRecord* latest_action(ExecutionId execution) const {
        const auto it = action_index.find(execution);
        if (it == action_index.end() || it->second.empty()) {
            return nullptr;
        }
        return state.actions.find(it->second.back());
    }

    // ---- persistence ------------------------------------------------------
    Status put_store_meta(bool flush) {
        StoreMetaRecord meta;
        meta.store = state.store;
        meta.epoch = state.epoch;
        meta.next_execution_sequence = state.next_execution_sequence;
        meta.sequence = state.sequence;
        meta.schema = kPersistenceSchemaVersion;
        return append_record(store, RecordKind::StoreMeta, meta, flush);
    }
    Status put_policy(const ExecutionPolicy& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Policy, r, flush));
        state.policies.upsert(r);
        return ok_status();
    }
    Status put_execution(const ExecutionRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Execution, r, flush));
        state.executions.upsert(r);
        return ok_status();
    }
    Status put_action(const ActionRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Action, r, flush));
        state.actions.upsert(r);
        index_action(r);
        return ok_status();
    }
    Status put_progress(const ProgressRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Progress, r, flush));
        state.progress.upsert(r);
        return ok_status();
    }
    Status put_checkpoint(const CheckpointRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Checkpoint, r, flush));
        const bool is_new = !state.checkpoints.contains(r.id);
        state.checkpoints.upsert(r);
        if (is_new) {
            checkpoint_index[r.execution].push_back(r.id);
        }
        return ok_status();
    }
    Status put_continuation(const ContinuationRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Continuation, r, flush));
        state.continuations.upsert(r);
        return ok_status();
    }
    Status put_lease(const LeaseRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Lease, r, flush));
        state.leases.upsert(r);
        return ok_status();
    }
    Status put_replay(const ReplayRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Replay, r, flush));
        const bool is_new = !state.replays.contains(r.id);
        state.replays.upsert(r);
        if (is_new) {
            replay_index[r.execution].push_back(r.id);
        }
        return ok_status();
    }
    Status put_ambiguity(const AmbiguityRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Ambiguity, r, flush));
        state.ambiguities.upsert(r);
        return ok_status();
    }
    Status put_commit(const CommitRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Commit, r, flush));
        const bool is_new = !state.commits.contains(r.id);
        state.commits.upsert(r);
        if (is_new) {
            commit_index[r.execution].push_back(r.id);
        }
        return ok_status();
    }
    Status put_recovery(const RecoveryRecord& r, bool flush) {
        PEF_TRY(append_record(store, RecordKind::Recovery, r, flush));
        state.recoveries.upsert(r);
        return ok_status();
    }

    // ---- request identity -------------------------------------------------
    bool lookup_request(RequestId id, CachedOutcome& out) {
        const auto it = request_cache.find(id);
        if (it == request_cache.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    void remember_request(RequestId id, const CachedOutcome& outcome) {
        if (!id.valid()) {
            return;
        }
        if (config.request_cache_size == 0) {
            return;
        }
        if (request_cache.find(id) != request_cache.end()) {
            request_cache[id] = outcome;
            return;
        }
        while (request_order.size() >= config.request_cache_size && !request_order.empty()) {
            const RequestId oldest = request_order.front();
            request_order.pop_front();
            request_cache.erase(oldest);
        }
        request_cache.emplace(id, outcome);
        request_order.push_back(id);
    }

    // ---- authority checks -------------------------------------------------
    Status check_epoch(CoordinatorEpoch epoch) const {
        if (epoch != state.epoch) {
            return err(Code::StaleEpoch,
                       "request carries coordinator epoch " + hex64(epoch.value()) +
                           " but the current epoch is " + hex64(state.epoch.value()));
        }
        return ok_status();
    }

    // No operation may begin after the store has been closed. Because every
    // public operation holds the runtime lock, an operation that has already
    // acquired it runs to completion before close() can be reached, and one
    // that arrives afterwards is refused before it mutates anything.
    Status check_open() const {
        if (!open) {
            return err(Code::ShuttingDown, "coordinator is not open");
        }
        if (shutting_down) {
            return err(Code::ShuttingDown, "coordinator is shutting down");
        }
        return ok_status();
    }

    Status check_caller(const CallerContext& caller) const {
        PEF_TRY(check_open());
        return check_epoch(caller.epoch);
    }

    Status validate_token(const LeaseToken& token, const ExecutionRecord*& execution,
                          const LeaseRecord*& lease) const {
        PEF_TRY(check_epoch(token.epoch));
        execution = state.executions.find(token.execution);
        if (execution == nullptr) {
            return err(Code::NotFound, "execution is not present in the durable store");
        }
        if (execution->generation != token.execution_generation) {
            return err(Code::StaleExecutionGeneration,
                       "token execution generation " +
                           hex64(token.execution_generation.value()) +
                           " does not match the current generation " +
                           hex64(execution->generation.value()));
        }
        if (execution->incarnation_generation != token.incarnation_generation) {
            return err(Code::StaleIncarnation,
                       "token incarnation generation " +
                           hex64(token.incarnation_generation.value()) +
                           " does not match the current incarnation generation " +
                           hex64(execution->incarnation_generation.value()));
        }
        if (execution->incarnation != token.incarnation) {
            return err(Code::StaleIncarnation,
                       "token incarnation identity does not match the current incarnation");
        }
        if (execution->lease != token.lease ||
            execution->lease_generation != token.lease_generation) {
            return err(Code::StaleLease, "token lease is not the execution's current lease");
        }
        lease = state.leases.find(token.lease);
        if (lease == nullptr) {
            return err(Code::StaleLease, "token lease is not present in the durable store");
        }
        if (lease->generation != token.lease_generation) {
            return err(Code::StaleLease, "token lease generation does not match the stored lease");
        }
        if (lease->state != LeaseState::Active) {
            return err(Code::AuthorityRevoked,
                       std::string("lease state is ") + std::string(lease_state_name(lease->state)));
        }
        if (lease->epoch != state.epoch) {
            return err(Code::StaleEpoch, "lease was issued under a previous coordinator epoch");
        }
        if (lease->worker != token.worker || lease->boot != token.boot) {
            return err(Code::StaleWorkerBoot, "token worker boot does not match the current lease");
        }
        if (execution->worker != token.worker || execution->boot != token.boot) {
            return err(Code::StaleWorkerBoot,
                       "token worker boot does not match the execution binding");
        }
        if (execution->policy_generation != token.policy_generation) {
            return err(Code::StalePolicyGeneration,
                       "token policy generation " + hex64(token.policy_generation.value()) +
                           " does not match the current policy generation " +
                           hex64(execution->policy_generation.value()));
        }
        return ok_status();
    }

    [[nodiscard]] const ExecutionPolicy* policy_of(const ExecutionRecord& execution) const {
        return state.policies.find(execution.policy);
    }

    // A logical commit computed but not yet installed on the execution record.
    // The execution record is always persisted last by the caller so that it
    // never references a record that is not durable.
    struct StagedCommit {
        CommitGroupRecord group;
        ProgressGeneration progress_generation;
    };

    // Computes one logical commit and writes it as a single journal record, so
    // that a crash can never leave a commit without its progress or progress
    // without the aggregate that promotes it.
    Status stage_commit(const ExecutionRecord& execution, const ActionRecord& action,
                        RequestId request, const Evidence& evidence, bool flush,
                        StagedCommit& out) {
        out.progress_generation = execution.progress_generation.next();
        ProgressRecord progress;
        progress.id = derive_progress_id(execution.id, out.progress_generation);
        progress.generation = out.progress_generation;
        progress.execution = execution.id;
        progress.execution_generation = execution.generation;
        progress.action = action.id;
        progress.action_generation = action.generation;
        progress.epoch = state.epoch;
        progress.ordinal = action.sequence;
        progress.committed = true;
        progress.durable_sequence = state.sequence;

        CommitRecord commit;
        commit.id = derive_commit_id(execution.id, action.id, action.generation,
                                     out.progress_generation);
        commit.execution = execution.id;
        commit.execution_generation = execution.generation;
        commit.action = action.id;
        commit.action_generation = action.generation;
        commit.progress = progress.id;
        commit.progress_generation = out.progress_generation;
        commit.epoch = state.epoch;
        commit.request = request;
        if (state.commits.find(commit.id) != nullptr) {
            return err(Code::DuplicateCommit,
                       "a durable commit already exists for this action identity and generation");
        }
        progress.commit = commit.id;

        ActionRecord sealed = action;
        sealed.status = ActionStatus::Committed;
        sealed.commit = commit.id;
        sealed.receipt = evidence;

        ExecutionRecord next = execution;
        next.progress_generation = out.progress_generation;
        next.progress = progress.id;
        next.last_commit = commit.id;
        next.committed_actions = execution.committed_actions + 1;
        next.epoch = state.epoch;

        out.group.commit = commit;
        out.group.progress = progress;
        out.group.action = sealed;
        out.group.execution = next;

        PEF_TRY(append_record(store, RecordKind::CommitGroup, out.group, flush));
        const bool new_commit = !state.commits.contains(commit.id);
        state.commits.upsert(commit);
        if (new_commit) {
            commit_index[commit.execution].push_back(commit.id);
        }
        state.progress.upsert(progress);
        state.actions.upsert(sealed);
        index_action(sealed);
        state.executions.upsert(next);
        return ok_status();
    }

    // Only one continuation may be VALID for an execution at a time: the one the
    // execution currently points at. Every other continuation is retired when a
    // newer durable point is established.
    Status retire_other_continuations(ExecutionId execution, ContinuationId keep, bool flush) {
        std::vector<ContinuationRecord> to_retire;
        for (const auto& continuation : state.continuations.insertion_order()) {
            if (continuation.execution != execution || continuation.id == keep) {
                continue;
            }
            if (continuation.state != ContinuationState::Valid) {
                continue;
            }
            ContinuationRecord stale = continuation;
            stale.state = ContinuationState::Stale;
            to_retire.push_back(stale);
        }
        for (const auto& continuation : to_retire) {
            PEF_TRY(put_continuation(continuation, flush));
        }
        return ok_status();
    }

    // Applies a lifecycle transition, dropping authority when the target state
    // must not retain any, and persists the execution record.
    Status persist_transition(ExecutionRecord next, Lifecycle target, const char* what,
                              bool flush) {
        PEF_TRY(require_transition(next.lifecycle, target, what));
        next.lifecycle = target;
        const bool drop_authority = target == Lifecycle::Fenced ||
                                    target == Lifecycle::Suspended || is_terminal_state(target);
        if (drop_authority && next.lease.valid()) {
            if (const LeaseRecord* lease = state.leases.find(next.lease);
                lease != nullptr && lease->state == LeaseState::Active) {
                LeaseRecord revoked = *lease;
                revoked.state = LeaseState::Revoked;
                PEF_TRY(put_lease(revoked, flush));
            }
            next.lease = LeaseId{};
            next.worker = WorkerId{};
            next.boot = WorkerBootId{};
        }
        next.epoch = state.epoch;
        return put_execution(next, flush);
    }

    Status find_checked(const CallerContext& caller, ExecutionId id,
                        const ExecutionRecord*& out) {
        PEF_TRY(check_caller(caller));
        out = state.executions.find(id);
        if (out == nullptr) {
            return err(Code::NotFound, "execution is not present in the durable store");
        }
        return ok_status();
    }

    // Issues a fresh continuation bound to the current durable point and
    // installs it on the execution. The caller persists in the correct order.
    [[nodiscard]] ContinuationRecord mint_continuation(const ExecutionRecord& execution,
                                                       const ExecutionRecord& next,
                                                       ContinuationState initial_state,
                                                       std::uint64_t resume_ordinal) const {
        ContinuationRecord continuation;
        continuation.generation = execution.continuation_generation.next();
        continuation.id = derive_continuation_id(execution.id, next.incarnation_generation,
                                                 next.progress_generation, continuation.generation);
        continuation.execution = execution.id;
        continuation.execution_generation = next.generation;
        continuation.incarnation = next.incarnation;
        continuation.incarnation_generation = next.incarnation_generation;
        continuation.checkpoint = next.checkpoint;
        continuation.checkpoint_generation = next.checkpoint_generation;
        continuation.progress_generation = next.progress_generation;
        continuation.action_generation = next.action_generation;
        continuation.policy = next.policy;
        continuation.policy_generation = next.policy_generation;
        continuation.bindings = next.bindings;
        continuation.epoch = state.epoch;
        continuation.recovery = next.recovery;
        continuation.recovery_generation = next.recovery_generation;
        continuation.state = initial_state;
        continuation.resume_ordinal = resume_ordinal;
        return continuation;
    }
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;

// ---------------------------------------------------------------------------
// Open / shutdown
// ---------------------------------------------------------------------------
Status Runtime::open(const RuntimeConfig& config, OpenOutcome& outcome) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    if (impl_->open) {
        return err(Code::AlreadyExists, "runtime is already open");
    }
    impl_->config = config;
    config_ = config;
    // A runtime that was shut down may be opened again, exactly as a fresh
    // coordinator process would open the same store.
    impl_->shutting_down = false;

    PEF_TRY(impl_->store.open(config.store_path, config.create_if_missing, config.store_limits));

    LoadReport report;
    PEF_TRY(impl_->store.load(impl_->state, report));
    outcome.load = report;
    outcome.previous_epoch = impl_->state.epoch;
    outcome.created = !report.snapshot_present && report.records_loaded == 0;

    if (!impl_->state.store.valid()) {
        impl_->state.store = impl_->store.store_id();
    } else if (impl_->state.store != impl_->store.store_id()) {
        return err(Code::CorruptState, "journal store id does not match the recovered state");
    }
    if (impl_->state.schema != 0 && impl_->state.schema != kPersistenceSchemaVersion) {
        return err(Code::Unsupported, "durable state schema is not supported by this build");
    }
    impl_->state.schema = kPersistenceSchemaVersion;
    impl_->rebuild_indexes();

    // A torn journal tail can leave durable detail records that the aggregate
    // record never promoted. The detail records are the durable facts, so the
    // aggregate is brought forward and the repair is itself journaled.
    {
        std::vector<CommitGroupRecord> repairs;
        if (reconcile_state(impl_->state, repairs)) {
            impl_->rebuild_indexes();
            for (const auto& repair : repairs) {
                PEF_TRY(append_record(impl_->store, RecordKind::CommitGroup, repair, true));
            }
            outcome.reconciled_commits = repairs.size();
            outcome.detail += "reconciled " + std::to_string(repairs.size()) +
                              " commit(s) whose aggregate update was lost; ";
        }
    }

    // Coordinator restart advances the epoch. Persisted execution identities
    // survive; process-local sessions and every lease from the previous epoch do
    // not.
    const CoordinatorEpoch new_epoch = impl_->state.epoch.next();
    impl_->state.epoch = new_epoch;
    PEF_TRY(impl_->put_store_meta(true));
    outcome.epoch = new_epoch;

    // Revoke every lease that is not from the current epoch, clear the worker
    // binding it authorised, and force every execution that was doing work into
    // RecoveryRequired. RUNNING is process-local and is never revived from
    // durable bytes.
    std::vector<LeaseRecord> leases_to_revoke;
    for (const auto& lease : impl_->state.leases.insertion_order()) {
        if (lease.state == LeaseState::Active && lease.epoch != new_epoch) {
            leases_to_revoke.push_back(lease);
        }
    }
    for (auto lease : leases_to_revoke) {
        lease.state = LeaseState::Revoked;
        PEF_TRY(impl_->put_lease(lease, true));
        ++outcome.leases_revoked;
    }

    std::vector<ExecutionRecord> executions_to_update;
    for (const auto& execution : impl_->state.executions.insertion_order()) {
        ExecutionRecord next = execution;
        bool changed = false;
        if (next.lease.valid()) {
            const LeaseRecord* lease = impl_->state.leases.find(next.lease);
            if (lease == nullptr || lease->state != LeaseState::Active ||
                lease->epoch != new_epoch) {
                next.lease = LeaseId{};
                next.worker = WorkerId{};
                next.boot = WorkerBootId{};
                changed = true;
            }
        }
        if (is_active_state(next.lifecycle) || next.lifecycle == Lifecycle::Fenced) {
            next.lifecycle = Lifecycle::RecoveryRequired;
            changed = true;
        }
        if (changed) {
            executions_to_update.push_back(next);
        }
    }
    for (const auto& next : executions_to_update) {
        PEF_TRY(impl_->put_execution(next, true));
        ++outcome.executions_reclassified;
        outcome.reclassified.push_back(next.id);
    }

    // Continuations issued under a previous epoch cannot be authoritative.
    std::vector<ContinuationRecord> stale_continuations;
    for (const auto& continuation : impl_->state.continuations.insertion_order()) {
        if (continuation.epoch != new_epoch && continuation.state == ContinuationState::Valid) {
            stale_continuations.push_back(continuation);
        }
    }
    for (auto continuation : stale_continuations) {
        continuation.state = ContinuationState::Stale;
        PEF_TRY(impl_->put_continuation(continuation, true));
    }

    impl_->open = true;

    // Classify in-flight actions for every execution that requires recovery and
    // apply the durable consequences that are safe without an operator. This is
    // the "recover valid continuations" step of a coordinator restart.
    std::vector<ExecutionId> pending;
    for (const auto& execution : impl_->state.executions.insertion_order()) {
        if (execution.lifecycle == Lifecycle::RecoveryRequired) {
            pending.push_back(execution.id);
        }
    }
    CallerContext caller;
    caller.epoch = new_epoch;
    for (ExecutionId id : pending) {
        RecoveryOutcome recovered;
        const Status status = recover_locked(caller, RequestId{}, id, recovered);
        if (!status.ok()) {
            outcome.detail += describe_execution(id) + ": " + status.to_string() + "; ";
        } else {
            outcome.detail += describe_execution(id) + ": " +
                              std::string(recovery_decision_name(recovered.plan.decision)) + "; ";
        }
    }
    return ok_status();
}

Status Runtime::shutdown() {
    std::lock_guard<std::mutex> guard(impl_->mu);
    if (!impl_->open) {
        return ok_status();
    }
    impl_->shutting_down = true;
    // Stop new authority grants before touching the store: every session is
    // dropped, so no further operation can present a valid lease token for a
    // session that no longer exists.
    impl_->sessions.clear();
    impl_->request_cache.clear();
    impl_->request_order.clear();
    PEF_TRY(impl_->store.flush());
    PEF_TRY(impl_->store.close());
    impl_->open = false;
    return ok_status();
}

bool Runtime::is_open() const noexcept {
    std::lock_guard<std::mutex> guard(impl_->mu);
    return impl_->open;
}

CoordinatorEpoch Runtime::epoch() const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    return impl_->state.epoch;
}

StoreId Runtime::store_id() const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    return impl_->state.store;
}

std::uint64_t Runtime::journal_sequence() const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    return impl_->state.sequence;
}

std::size_t Runtime::session_count() const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    return impl_->sessions.size();
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------
Status Runtime::begin_session(WorkerId worker, WorkerBootId boot, CallerContext& out,
                              SessionId& out_session) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    if (!impl_->open) {
        return err(Code::NotFound, "runtime is not open");
    }
    if (impl_->shutting_down) {
        return err(Code::ShuttingDown, "coordinator is shutting down");
    }
    if (!worker.valid() || !boot.valid()) {
        return err(Code::InvalidArgument, "worker id and worker boot id must both be non-nil");
    }
    const SessionId session{impl_->next_session++};
    impl_->sessions.emplace(session, SessionInfo{worker, boot});
    out.session = session;
    out.epoch = impl_->state.epoch;
    out_session = session;
    return ok_status();
}

Status Runtime::end_session(SessionId session) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const auto it = impl_->sessions.find(session);
    if (it == impl_->sessions.end()) {
        return err(Code::NotFound, "session is not present");
    }
    const SessionInfo info = it->second;
    impl_->sessions.erase(it);

    // Worker loss. Every execution bound to this worker boot is fenced: the old
    // boot loses authority immediately and permanently.
    std::vector<ExecutionRecord> to_fence;
    for (const auto& execution : impl_->state.executions.insertion_order()) {
        if (execution.worker == info.worker && execution.boot == info.boot &&
            !is_terminal_state(execution.lifecycle)) {
            to_fence.push_back(execution);
        }
    }
    for (const auto& execution : to_fence) {
        ExecutionRecord next = execution;
        if (!is_legal_transition(next.lifecycle, Lifecycle::Fenced)) {
            // A resting state that cannot be fenced is left as it is; authority
            // has already been removed by revoking the lease below.
            if (!next.lease.valid()) {
                continue;
            }
        } else {
            next.lifecycle = Lifecycle::Fenced;
        }
        if (next.lease.valid()) {
            if (const LeaseRecord* lease = impl_->state.leases.find(next.lease); lease != nullptr) {
                LeaseRecord revoked = *lease;
                revoked.state = LeaseState::Revoked;
                PEF_TRY(impl_->put_lease(revoked, true));
            }
            next.lease = LeaseId{};
            next.worker = WorkerId{};
            next.boot = WorkerBootId{};
        }
        PEF_TRY(impl_->put_execution(next, true));
    }
    return ok_status();
}

// ---------------------------------------------------------------------------
// Execution creation
// ---------------------------------------------------------------------------
Status Runtime::create_execution(const CallerContext& caller, const CreateExecutionRequest& request,
                                 CreateExecutionResult& out) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_caller(caller));

    CachedOutcome cached;
    if (impl_->lookup_request(request.request, cached)) {
        out.execution = ExecutionId{cached.a};
        out.generation = ExecutionGeneration{cached.b};
        out.existing = true;
        const ExecutionRecord* existing = impl_->state.executions.find(out.execution);
        if (existing != nullptr) {
            out.policy = existing->policy;
            out.policy_generation = existing->policy_generation;
        }
        return ok_status();
    }

    if (impl_->state.executions.size() >= impl_->config.max_executions) {
        return err(Code::LimitExceeded, "the configured execution bound has been reached");
    }

    const std::uint64_t sequence = impl_->state.next_execution_sequence;
    const ExecutionId execution_id = derive_execution_id(impl_->state.store, sequence);

    ExecutionPolicy policy = request.policy;
    policy.id = derive_policy_id(execution_id, PolicyGeneration{1});
    policy.generation = PolicyGeneration{1};
    if (const auto reason = validate_policy(policy); reason.has_value()) {
        return err(Code::InvalidArgument, std::string("policy rejected: ") + std::string(*reason));
    }

    // The derived-identity counter is advanced before anything references it, so
    // a failure part-way through can only burn an identity, never reuse one.
    impl_->state.next_execution_sequence = sequence + 1;
    PEF_TRY(impl_->put_store_meta(true));

    ExecutionRecord execution;
    execution.id = execution_id;
    execution.generation = ExecutionGeneration{1};
    execution.lifecycle = Lifecycle::Created;
    execution.policy = policy.id;
    execution.policy_generation = policy.generation;
    execution.epoch = impl_->state.epoch;
    execution.created_sequence = sequence;

    PEF_TRY(impl_->put_policy(policy, true));
    PEF_TRY(impl_->put_execution(execution, true));

    impl_->remember_request(request.request, CachedOutcome{Code::Ok, execution_id.value(),
                                                           execution.generation.value()});
    out.execution = execution_id;
    out.generation = execution.generation;
    out.policy = policy.id;
    out.policy_generation = policy.generation;
    out.existing = false;
    return ok_status();
}

Status Runtime::update_policy(const CallerContext& caller, RequestId request,
                              ExecutionId execution_id, ExecutionPolicy policy, bool apply) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_caller(caller));
    CachedOutcome cached;
    if (impl_->lookup_request(request, cached)) {
        return err(Code::DuplicateRequest, "policy update request was already applied");
    }
    const ExecutionRecord* execution = impl_->state.executions.find(execution_id);
    if (execution == nullptr) {
        return err(Code::NotFound, "execution is not present in the durable store");
    }
    policy.id = execution->policy;
    policy.generation = execution->policy_generation.next();
    if (const auto reason = validate_policy(policy); reason.has_value()) {
        return err(Code::InvalidArgument, std::string("policy rejected: ") + std::string(*reason));
    }
    if (!apply) {
        return ok_status();
    }
    PEF_TRY(impl_->put_policy(policy, true));

    ExecutionRecord next = *execution;
    next.policy_generation = policy.generation;
    // A policy change invalidates the authority that was granted under the old
    // generation: lease and continuations alike.
    if (next.lease.valid()) {
        if (const LeaseRecord* lease = impl_->state.leases.find(next.lease); lease != nullptr) {
            LeaseRecord revoked = *lease;
            revoked.state = LeaseState::Revoked;
            PEF_TRY(impl_->put_lease(revoked, true));
        }
        next.lease = LeaseId{};
        next.worker = WorkerId{};
        next.boot = WorkerBootId{};
        if (is_active_state(next.lifecycle)) {
            PEF_TRY(require_transition(next.lifecycle, Lifecycle::RecoveryRequired,
                                       "policy update"));
            next.lifecycle = Lifecycle::RecoveryRequired;
        }
    }
    for (const auto& continuation : impl_->state.continuations.insertion_order()) {
        if (continuation.execution != execution_id || continuation.state != ContinuationState::Valid) {
            continue;
        }
        ContinuationRecord updated = continuation;
        updated.state = policy.policy_change_invalidates_continuations
                            ? ContinuationState::Stale
                            : ContinuationState::RevalidationRequired;
        PEF_TRY(impl_->put_continuation(updated, true));
    }
    PEF_TRY(impl_->put_execution(next, true));
    impl_->remember_request(request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

// ---------------------------------------------------------------------------
// Worker binding and lifecycle
// ---------------------------------------------------------------------------
Status Runtime::bind_worker(const CallerContext& caller, const BindWorkerRequest& request,
                            BindWorkerResult& out) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_caller(caller));
    CachedOutcome cached;
    if (impl_->lookup_request(request.request, cached)) {
        out.incarnation = ExecutionIncarnationId{cached.a};
        out.lease = LeaseId{cached.b};
        const ExecutionRecord* existing = impl_->state.executions.find(request.execution);
        if (existing != nullptr) {
            out.incarnation_generation = existing->incarnation_generation;
            out.lease_generation = existing->lease_generation;
            out.lifecycle = existing->lifecycle;
        }
        return ok_status();
    }
    if (!request.worker.valid() || !request.boot.valid()) {
        return err(Code::InvalidArgument, "worker id and worker boot id must both be non-nil");
    }
    const ExecutionRecord* execution = impl_->state.executions.find(request.execution);
    if (execution == nullptr) {
        return err(Code::NotFound, "execution is not present in the durable store");
    }
    if (const auto defect = binding_set_defect(request.bindings); defect.has_value()) {
        return err(Code::InvalidArgument, "bindings rejected: " + std::string(*defect));
    }
    const ExecutionPolicy* policy = impl_->policy_of(*execution);

    switch (execution->lifecycle) {
        case Lifecycle::Created:
        case Lifecycle::Ready:
        case Lifecycle::Fenced:
        case Lifecycle::RecoveryRequired:
        case Lifecycle::Recovering:
        case Lifecycle::Resuming:
        case Lifecycle::Suspended:
            break;
        default:
            return err(Code::LifecycleRefused,
                       std::string("cannot bind a worker while the execution is ") +
                           std::string(lifecycle_name(execution->lifecycle)));
    }

    // A live binding to a different boot must be fenced explicitly first. The
    // coordinator does this automatically when the worker's session ends; a
    // caller cannot do it implicitly by binding a replacement.
    if (execution->lease.valid() && execution->boot != request.boot) {
        const LeaseRecord* lease = impl_->state.leases.find(execution->lease);
        if (lease != nullptr && lease->state == LeaseState::Active) {
            return err(Code::Fenced,
                       "execution is still bound to a live worker boot; fence it first");
        }
    }

    ExecutionRecord next = *execution;
    const bool same_boot = execution->boot.valid() && execution->boot == request.boot;
    out.incarnation_advanced = !same_boot;
    if (!same_boot) {
        // A new process or worker assumes execution: the incarnation changes.
        ExecutionIncarnationGeneration generation = execution->incarnation_generation.established()
                                                        ? execution->incarnation_generation.next()
                                                        : ExecutionIncarnationGeneration{1};
        next.incarnation_generation = generation;
        next.incarnation = derive_incarnation_id(execution->id, generation);
    }

    if (execution->lease.valid()) {
        if (const LeaseRecord* lease = impl_->state.leases.find(execution->lease); lease != nullptr) {
            if (lease->state == LeaseState::Active) {
                LeaseRecord superseded = *lease;
                superseded.state = LeaseState::Superseded;
                PEF_TRY(impl_->put_lease(superseded, true));
            }
        }
    }

    LeaseRecord lease;
    lease.generation = execution->lease_generation.next();
    lease.id = derive_lease_id(execution->id, request.worker, request.boot, lease.generation);
    lease.execution = execution->id;
    lease.execution_generation = execution->generation;
    lease.incarnation = next.incarnation;
    lease.incarnation_generation = next.incarnation_generation;
    lease.worker = request.worker;
    lease.boot = request.boot;
    lease.epoch = impl_->state.epoch;
    lease.policy_generation = next.policy_generation;
    lease.bindings = request.bindings;
    lease.state = LeaseState::Active;

    next.worker = request.worker;
    next.boot = request.boot;
    next.lease = lease.id;
    next.lease_generation = lease.generation;
    next.bindings = request.bindings;
    next.epoch = impl_->state.epoch;
    if (next.lifecycle == Lifecycle::Created) {
        next.lifecycle = Lifecycle::Ready;
    } else if (next.lifecycle == Lifecycle::Fenced) {
        // FENCED must pass through recovery; binding a worker does not, by
        // itself, restore authority.
        PEF_TRY(require_transition(next.lifecycle, Lifecycle::RecoveryRequired, "worker bind"));
        next.lifecycle = Lifecycle::RecoveryRequired;
    }

    PEF_TRY(impl_->put_lease(lease, flush_for(policy)));
    PEF_TRY(impl_->put_execution(next, flush_for(policy)));

    impl_->remember_request(request.request,
                            CachedOutcome{Code::Ok, next.incarnation.value(), lease.id.value()});
    out.incarnation = next.incarnation;
    out.incarnation_generation = next.incarnation_generation;
    out.lease = lease.id;
    out.lease_generation = lease.generation;
    out.lifecycle = next.lifecycle;
    return ok_status();
}

Status Runtime::start(const StartRequest& request) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_open());
    const ExecutionRecord* execution = nullptr;
    const LeaseRecord* lease = nullptr;
    PEF_TRY(impl_->validate_token(request.token, execution, lease));
    CachedOutcome cached;
    if (impl_->lookup_request(request.request, cached)) {
        return ok_status();
    }
    if (execution->lifecycle != Lifecycle::Ready) {
        return err(Code::LifecycleRefused,
                   std::string("cannot start an execution that is ") +
                       std::string(lifecycle_name(execution->lifecycle)));
    }
    ExecutionRecord next = *execution;
    PEF_TRY(require_transition(next.lifecycle, Lifecycle::Running, "start"));
    next.lifecycle = Lifecycle::Running;
    next.epoch = impl_->state.epoch;

    // Starting execution establishes the first durable continuation point. The
    // progress frontier itself is a legal resume point even before any
    // checkpoint exists.
    const ContinuationRecord continuation =
        impl_->mint_continuation(next, next, ContinuationState::Valid, next.action_frontier);
    next.continuation = continuation.id;
    next.continuation_generation = continuation.generation;

    const bool flush = flush_for(impl_->policy_of(next));
    PEF_TRY(impl_->retire_other_continuations(next.id, continuation.id, flush));
    PEF_TRY(impl_->put_continuation(continuation, flush));
    PEF_TRY(impl_->put_execution(next, flush));
    impl_->remember_request(request.request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
Status Runtime::begin_action(const BeginActionRequest& request, BeginActionResult& out) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_open());
    const ExecutionRecord* execution = nullptr;
    const LeaseRecord* lease = nullptr;
    PEF_TRY(impl_->validate_token(request.token, execution, lease));
    CachedOutcome cached;
    if (impl_->lookup_request(request.request, cached)) {
        out.action = ActionId{cached.a};
        out.ordinal = cached.b;
        const ActionRecord* existing = impl_->state.find_latest_action(out.action);
        if (existing != nullptr) {
            out.action_generation = existing->generation;
            out.attempt = existing->attempt;
            out.attempt_generation = existing->attempt_generation;
            out.effect_id = existing->effect_id;
            out.effect_generation = existing->effect_generation;
        }
        return ok_status();
    }
    if (execution->lifecycle != Lifecycle::Running) {
        return err(Code::LifecycleRefused,
                   std::string("cannot begin an action while the execution is ") +
                       std::string(lifecycle_name(execution->lifecycle)));
    }
    if (requires_request_key(request.effect_class) && request.request_key.empty()) {
        return err(Code::InvalidArgument,
                   "side-effect class REPEATABLE_WITH_KEY requires a stable request key");
    }
    if (request.request_key.size() > 512) {
        return err(Code::LimitExceeded, "request key exceeds 512 bytes");
    }
    const ExecutionPolicy* policy = impl_->policy_of(*execution);
    if (policy != nullptr && execution->action_frontier >= policy->max_action_history) {
        return err(Code::LimitExceeded,
                   "action history has reached the policy bound; compact the execution");
    }

    // A frontier action that holds a durable failure (for example after an
    // ambiguity was resolved as not-applied) is re-driven as the *same* logical
    // action under a new generation. No new logical action is invented.
    const ActionRecord* latest = impl_->latest_action(execution->id);
    bool is_replay = false;
    std::uint64_t ordinal = execution->action_frontier + 1;
    ActionGeneration generation{1};
    const bool retryable_frontier =
        latest != nullptr &&
        (latest->status == ActionStatus::Failed ||
         latest->status == ActionStatus::EffectNotApplied);
    if (latest != nullptr && latest->sequence == execution->action_frontier && retryable_frontier) {
        is_replay = true;
        ordinal = execution->action_frontier;
        generation = latest->generation.next();
        if (policy != nullptr && generation.value() > policy->max_replay_depth) {
            return err(Code::PolicyRefused,
                       "the replay depth bound for this action identity has been reached");
        }
    }
    if (policy != nullptr && execution->replay_count >= policy->max_replay_records) {
        return err(Code::LimitExceeded, "the replay record bound has been reached");
    }
    if (execution->blocked && ordinal > execution->blocked_above_ordinal) {
        return err(Code::UnresolvedAmbiguity,
                   "progress above ordinal " +
                       std::to_string(execution->blocked_above_ordinal) +
                       " is blocked by an unresolved ambiguous action");
    }

    ActionRecord action;
    action.id = derive_action_id(execution->id, ordinal);
    action.generation = generation;
    action.execution = execution->id;
    action.execution_generation = execution->generation;
    action.incarnation_generation = execution->incarnation_generation;
    action.sequence = ordinal;
    action.attempt_generation = AttemptGeneration{1};
    action.attempt = derive_attempt_id(action.id, action.attempt_generation);
    action.effect_class = request.effect_class;
    action.request_key = request.request_key;
    action.status = ActionStatus::InFlight;
    action.effect_id = derive_side_effect_id(execution->id, action.id, action.generation);
    action.effect_generation = SideEffectGeneration{1};
    action.receipt = request.evidence;
    action.dispatch_count = 1;

    ReplayRecord replay;
    if (is_replay) {
        action.replayed_from = latest->id;
        action.replayed_from_generation = latest->generation;
        action.replay_generation = latest->replay_generation.next();
        replay.id = derive_replay_id(action.id, action.replay_generation);
        replay.generation = action.replay_generation;
        replay.execution = execution->id;
        replay.execution_generation = execution->generation;
        replay.action = latest->id;
        replay.action_generation = latest->generation;
        replay.new_action = action.id;
        replay.new_action_generation = action.generation;
        replay.effect_class = action.effect_class;
        replay.request_key = action.request_key;
        replay.progress_generation = execution->progress_generation;
        replay.checkpoint_generation = execution->checkpoint_generation;
        replay.policy_generation = execution->policy_generation;
        replay.epoch = impl_->state.epoch;
        replay.status = ReplayStatus::Admitted;
        replay.evidence = request.evidence;
    }

    ExecutionRecord next = *execution;
    next.action_frontier = ordinal;
    next.action_generation = action.generation;
    next.blocked = false;
    next.blocked_above_ordinal = 0;
    if (is_replay) {
        next.replay_count = execution->replay_count + 1;
    }

    const bool flush = flush_for(policy);
    if (is_replay) {
        PEF_TRY(impl_->put_replay(replay, flush));
    }
    PEF_TRY(impl_->put_action(action, flush));
    PEF_TRY(impl_->put_execution(next, flush));

    impl_->remember_request(request.request, CachedOutcome{Code::Ok, action.id.value(), ordinal});
    out.action = action.id;
    out.action_generation = action.generation;
    out.ordinal = ordinal;
    out.attempt = action.attempt;
    out.attempt_generation = action.attempt_generation;
    out.effect_id = action.effect_id;
    out.effect_generation = action.effect_generation;
    return ok_status();
}

Status Runtime::complete_action(const CompleteActionRequest& request, CompleteActionResult& out) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_open());
    const ExecutionRecord* execution = nullptr;
    const LeaseRecord* lease = nullptr;

    // A duplicate completion must be answerable even after the token has gone
    // stale, so that a retry observes the already-committed result instead of
    // committing a second time. Only the durable commit identity is returned.
    const ActionRecord* existing_action =
        impl_->state.find_action(request.action, request.action_generation);
    if (existing_action != nullptr && existing_action->status == ActionStatus::Committed) {
        PEF_TRY(impl_->check_epoch(request.token.epoch));
        const CommitRecord* commit = impl_->state.commits.find(existing_action->commit);
        if (commit != nullptr) {
            out.commit = commit->id;
            out.progress = commit->progress;
            out.progress_generation = commit->progress_generation;
            out.ordinal = existing_action->sequence;
            out.duplicate = true;
            return ok_status();
        }
    }

    PEF_TRY(impl_->validate_token(request.token, execution, lease));
    CachedOutcome cached;
    if (impl_->lookup_request(request.request, cached)) {
        out.commit = CommitId{cached.a};
        out.progress_generation = ProgressGeneration{cached.b};
        out.duplicate = true;
        if (existing_action != nullptr) {
            out.ordinal = existing_action->sequence;
            if (const ProgressRecord* progress = impl_->state.progress.find(
                    derive_progress_id(execution->id, out.progress_generation));
                progress != nullptr) {
                out.progress = progress->id;
            }
        }
        return ok_status();
    }

    if (execution->lifecycle != Lifecycle::Running) {
        return err(Code::LifecycleRefused,
                   std::string("cannot complete an action while the execution is ") +
                       std::string(lifecycle_name(execution->lifecycle)));
    }
    if (existing_action == nullptr) {
        return err(Code::NotFound, "action is not present in the durable store");
    }
    if (existing_action->execution != execution->id) {
        return err(Code::InvalidArgument, "action does not belong to this execution");
    }
    if (existing_action->generation != request.action_generation) {
        return err(Code::StaleContinuation, "action generation does not match the stored action");
    }
    if (existing_action->status == ActionStatus::Ambiguous ||
        existing_action->status == ActionStatus::Abandoned ||
        existing_action->status == ActionStatus::Compensated) {
        return err(Code::UnresolvedAmbiguity,
                   std::string("action is ") +
                       std::string(action_status_name(existing_action->status)) +
                       " and cannot be completed");
    }
    if (existing_action->status == ActionStatus::EffectNotApplied) {
        return err(Code::AmbiguousCompletion,
                   "action holds a durable receipt stating the effect did not apply");
    }
    if (existing_action->status == ActionStatus::Failed) {
        return err(Code::AmbiguousCompletion, "action is recorded as failed");
    }
    if (!request.completion.effect_applied) {
        return err(Code::InvalidArgument,
                   "complete_action requires an applied effect; use fail_action otherwise");
    }
    if (execution->blocked && existing_action->sequence > execution->blocked_above_ordinal) {
        return err(Code::UnresolvedAmbiguity,
                   "this action's ordinal is above the execution's unresolved ambiguity");
    }

    const ExecutionPolicy* policy = impl_->policy_of(*execution);
    const bool flush = flush_for(policy);

    Impl::StagedCommit staged;
    PEF_TRY(impl_->stage_commit(*execution, *existing_action, request.request,
                                request.completion.evidence, flush, staged));
    const std::uint64_t committed_actions = staged.group.execution.committed_actions;

    impl_->remember_request(
        request.request,
        CachedOutcome{Code::Ok, staged.group.commit.id.value(),
                      staged.progress_generation.value()});
    out.commit = staged.group.commit.id;
    out.progress = staged.group.progress.id;
    out.progress_generation = staged.progress_generation;
    out.ordinal = staged.group.action.sequence;
    out.duplicate = false;
    out.checkpoint_due = policy != nullptr && policy->checkpoint_interval_actions > 0 &&
                         (committed_actions % policy->checkpoint_interval_actions) == 0;
    return ok_status();
}

Status Runtime::fail_action(const FailActionRequest& request) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_open());
    const ExecutionRecord* execution = nullptr;
    const LeaseRecord* lease = nullptr;
    PEF_TRY(impl_->validate_token(request.token, execution, lease));
    CachedOutcome cached;
    if (impl_->lookup_request(request.request, cached)) {
        return ok_status();
    }
    const ActionRecord* existing =
        impl_->state.find_action(request.action, request.action_generation);
    if (existing == nullptr) {
        return err(Code::NotFound, "action is not present in the durable store");
    }
    if (existing->execution != execution->id) {
        return err(Code::InvalidArgument, "action does not belong to this execution");
    }
    if (existing->status == ActionStatus::Committed) {
        return err(Code::DuplicateCommit, "action is already committed and cannot be failed");
    }
    if (existing->status == ActionStatus::Ambiguous) {
        return err(Code::UnresolvedAmbiguity, "action is ambiguous and must be resolved");
    }
    ActionRecord action = *existing;
    action.status = ActionStatus::Failed;
    action.receipt = request.evidence;
    PEF_TRY(impl_->put_action(action, flush_for(impl_->policy_of(*execution))));
    impl_->remember_request(request.request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

Status Runtime::report_side_effect(const ReportSideEffectRequest& request) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_open());
    const ExecutionRecord* execution = nullptr;
    const LeaseRecord* lease = nullptr;
    PEF_TRY(impl_->validate_token(request.token, execution, lease));
    CachedOutcome cached;
    if (impl_->lookup_request(request.request, cached)) {
        return ok_status();
    }
    const ActionRecord* existing =
        impl_->state.find_action(request.action, request.action_generation);
    if (existing == nullptr) {
        return err(Code::NotFound, "action is not present in the durable store");
    }
    if (existing->status == ActionStatus::Committed) {
        return err(Code::DuplicateCommit, "action is already committed");
    }
    if (existing->status == ActionStatus::Ambiguous ||
        existing->status == ActionStatus::Abandoned) {
        return err(Code::UnresolvedAmbiguity,
                   "action is sealed; a receipt cannot change its recorded outcome");
    }
    ActionRecord action = *existing;
    action.status = request.applied ? ActionStatus::EffectApplied : ActionStatus::EffectNotApplied;
    action.receipt = request.evidence;
    PEF_TRY(impl_->put_action(action, flush_for(impl_->policy_of(*execution))));
    impl_->remember_request(request.request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

// ---------------------------------------------------------------------------
// Checkpoints
// ---------------------------------------------------------------------------
Status Runtime::register_checkpoint(const RegisterCheckpointRequest& request,
                                    RegisterCheckpointResult& out) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_open());
    const ExecutionRecord* execution = nullptr;
    const LeaseRecord* lease = nullptr;
    PEF_TRY(impl_->validate_token(request.token, execution, lease));
    CachedOutcome cached;
    if (impl_->lookup_request(request.request, cached)) {
        out.checkpoint = CheckpointId{cached.a};
        const CheckpointRecord* existing = impl_->state.checkpoints.find(out.checkpoint);
        if (existing != nullptr) {
            out.checkpoint_generation = existing->generation;
            out.state = existing->state;
            out.lineage_depth = existing->lineage_depth;
        }
        return ok_status();
    }
    if (execution->lifecycle != Lifecycle::Running &&
        execution->lifecycle != Lifecycle::Checkpointing) {
        return err(Code::LifecycleRefused,
                   std::string("cannot register a checkpoint while the execution is ") +
                       std::string(lifecycle_name(execution->lifecycle)));
    }
    if (request.content_hash == 0) {
        return err(Code::InvalidArgument,
                   "a checkpoint must carry a non-zero content integrity digest");
    }
    if (request.expected_checkpoint != execution->checkpoint ||
        request.expected_checkpoint_generation != execution->checkpoint_generation) {
        return err(Code::StaleCheckpointGeneration,
                   "the checkpoint the caller believes is current is not the current checkpoint");
    }
    const ExecutionPolicy* policy = impl_->policy_of(*execution);
    const bool flush = flush_for(policy);

    // Enter CHECKPOINTING durably before the checkpoint record exists. A
    // coordinator that dies here leaves a state that recovery classifies as
    // interrupted mid-checkpoint, with no new durable point.
    ExecutionRecord working = *execution;
    if (working.lifecycle == Lifecycle::Running) {
        PEF_TRY(require_transition(working.lifecycle, Lifecycle::Checkpointing, "checkpoint"));
        working.lifecycle = Lifecycle::Checkpointing;
        PEF_TRY(impl_->put_execution(working, flush));
    }

    CheckpointRecord checkpoint;
    checkpoint.generation = execution->checkpoint_generation.next();
    checkpoint.id = derive_checkpoint_id(execution->id, checkpoint.generation);
    checkpoint.execution = execution->id;
    checkpoint.execution_generation = execution->generation;
    checkpoint.incarnation_generation = execution->incarnation_generation;
    checkpoint.progress_generation = execution->progress_generation;
    checkpoint.action_generation = execution->action_generation;
    checkpoint.policy = execution->policy;
    checkpoint.policy_generation = execution->policy_generation;
    checkpoint.bindings = execution->bindings;
    checkpoint.epoch = impl_->state.epoch;
    checkpoint.worker = execution->worker;
    checkpoint.boot = execution->boot;
    checkpoint.parent = execution->checkpoint;
    checkpoint.parent_generation = execution->checkpoint_generation;
    checkpoint.lineage_depth = 0;
    if (checkpoint.parent.valid()) {
        const CheckpointRecord* parent = impl_->state.checkpoints.find(checkpoint.parent);
        if (parent == nullptr) {
            return err(Code::CorruptState, "the checkpoint lineage parent is missing");
        }
        if (parent->execution != execution->id) {
            return err(Code::CorruptState, "the checkpoint lineage parent belongs to another execution");
        }
        checkpoint.lineage_depth = parent->lineage_depth + 1;
    }
    checkpoint.effect_boundary_ordinal =
        request.effect_boundary_ordinal > execution->action_frontier
            ? execution->action_frontier
            : request.effect_boundary_ordinal;
    checkpoint.content_size = request.content_size;
    checkpoint.content_hash = request.content_hash;
    // A checkpoint that binds the current generations of a live execution and
    // carries a content digest is VERIFIED. The fabric does not own the bytes,
    // so it verifies the metadata it does own.
    checkpoint.state = CheckpointState::Verified;

    PEF_TRY(impl_->put_checkpoint(checkpoint, flush));

    ExecutionRecord next = working;
    next.checkpoint = checkpoint.id;
    next.checkpoint_generation = checkpoint.generation;

    // The previous current checkpoint is superseded unless policy still allows
    // deriving a continuation from an older durable point.
    if (execution->checkpoint.valid()) {
        if (const CheckpointRecord* previous =
                impl_->state.checkpoints.find(execution->checkpoint);
            previous != nullptr && previous->state != CheckpointState::Superseded) {
            CheckpointRecord demoted = *previous;
            demoted.state = (policy != nullptr && policy->allow_older_checkpoints)
                                ? CheckpointState::Verified
                                : CheckpointState::Superseded;
            PEF_TRY(impl_->put_checkpoint(demoted, flush));
        }
    }
    if (is_legal_transition(working.lifecycle, Lifecycle::Running)) {
        next.lifecycle = Lifecycle::Running;
    }

    const ContinuationRecord continuation =
        impl_->mint_continuation(*execution, next, ContinuationState::Valid,
                                 next.action_frontier);
    next.continuation = continuation.id;
    next.continuation_generation = continuation.generation;

    PEF_TRY(impl_->retire_other_continuations(execution->id, continuation.id, flush));
    PEF_TRY(impl_->put_continuation(continuation, flush));
    PEF_TRY(impl_->put_execution(next, flush));

    impl_->remember_request(request.request,
                            CachedOutcome{Code::Ok, checkpoint.id.value(), 0});
    out.checkpoint = checkpoint.id;
    out.checkpoint_generation = checkpoint.generation;
    out.state = checkpoint.state;
    out.continuation = continuation.id;
    out.continuation_generation = continuation.generation;
    out.lineage_depth = checkpoint.lineage_depth;
    return ok_status();
}

// ---------------------------------------------------------------------------
// Resume
// ---------------------------------------------------------------------------
namespace {
[[nodiscard]] ExecutionId execution_id_of(const ExecutionRecord& record) { return record.id; }
}  // namespace

Status Runtime::resume(const ResumeRequest& request, ResumeResult& out) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_open());
    const ExecutionRecord* execution = nullptr;
    const LeaseRecord* lease = nullptr;
    PEF_TRY(impl_->validate_token(request.token, execution, lease));
    CachedOutcome cached;
    if (impl_->lookup_request(request.request, cached)) {
        out.lifecycle = execution->lifecycle;
        return ok_status();
    }
    const ExecutionPolicy* policy = impl_->policy_of(*execution);
    const bool flush = flush_for(policy);

    switch (execution->lifecycle) {
        case Lifecycle::Suspended:
        case Lifecycle::RecoveryRequired:
        case Lifecycle::Recovering:
        case Lifecycle::Resuming:
            break;
        default:
            return err(Code::LifecycleRefused,
                       std::string("cannot resume an execution that is ") +
                           std::string(lifecycle_name(execution->lifecycle)));
    }

    // Recovery must have been classified and applied before a RecoveryRequired
    // execution can become authoritative again.
    if (execution->lifecycle == Lifecycle::RecoveryRequired) {
        if (!execution->recovery.valid()) {
            return err(Code::LifecycleRefused,
                       "execution requires recovery and no recovery has been applied");
        }
        const RecoveryRecord* recovery = impl_->state.recoveries.find(execution->recovery);
        if (recovery == nullptr) {
            return err(Code::CorruptState, "the recorded recovery is missing");
        }
        const auto decision = static_cast<RecoveryDecision>(recovery->decision);
        if (!recovery_allows_automatic_continue(decision)) {
            out.decision = decision;
            return err(Code::PolicyRefused,
                       std::string("recovery decision ") +
                           std::string(recovery_decision_name(decision)) +
                           " does not permit automatic continuation");
        }
    }

    // Bindings the worker can currently offer must match the durable set, or be
    // explicitly revalidated.
    if (!execution->bindings.equals_generations(request.observed_bindings)) {
        if (!request.revalidate_bindings) {
            return err(Code::StaleBindingGeneration,
                       "a required binding generation changed; revalidation is required");
        }
    }

    // Bindings the worker can currently offer are installed durably before the
    // continuation is minted, so the continuation is validated against the state
    // that will actually be authoritative.
    ExecutionRecord next = *execution;
    next.bindings = request.observed_bindings;
    next.epoch = impl_->state.epoch;

    if (next.lifecycle == Lifecycle::RecoveryRequired) {
        PEF_TRY(require_transition(next.lifecycle, Lifecycle::Recovering, "resume"));
        next.lifecycle = Lifecycle::Recovering;
    }
    if (next.lifecycle == Lifecycle::Recovering) {
        PEF_TRY(require_transition(next.lifecycle, Lifecycle::Resuming, "resume"));
        next.lifecycle = Lifecycle::Resuming;
    }
    if (next.lifecycle == Lifecycle::Suspended) {
        PEF_TRY(require_transition(next.lifecycle, Lifecycle::Resuming, "resume"));
        next.lifecycle = Lifecycle::Resuming;
    }
    PEF_TRY(impl_->put_execution(next, flush));

    const ContinuationRecord continuation =
        impl_->mint_continuation(next, next, ContinuationState::Valid, next.action_frontier);
    next.continuation = continuation.id;
    next.continuation_generation = continuation.generation;

    ContinuationState state = ContinuationState::Invalid;
    std::string reason;
    PEF_TRY(validate_continuation_state(impl_->state, continuation, state, reason));
    if (state != ContinuationState::Valid) {
        out.continuation_state = state;
        return err(Code::StaleContinuation,
                   "the continuation minted for resume is not valid: " + reason);
    }

    PEF_TRY(require_transition(next.lifecycle, Lifecycle::Running, "resume"));
    next.lifecycle = Lifecycle::Running;

    PEF_TRY(impl_->retire_other_continuations(execution_id_of(next), continuation.id, flush));
    PEF_TRY(impl_->put_continuation(continuation, flush));
    PEF_TRY(impl_->put_execution(next, flush));

    impl_->remember_request(request.request, CachedOutcome{Code::Ok, 0, 0});
    out.continuation_state = state;
    out.lifecycle = next.lifecycle;
    return ok_status();
}

// ---------------------------------------------------------------------------
// Continuation validation
// ---------------------------------------------------------------------------
Status validate_continuation_state(const DurableState& state,
                                   const ContinuationRecord& continuation,
                                   ContinuationState& out_state, std::string& out_reason) {
    out_state = ContinuationState::Invalid;
    out_reason.clear();

    const ExecutionRecord* execution = state.executions.find(continuation.execution);
    if (execution == nullptr) {
        out_reason = "continuation references an execution that does not exist";
        return ok_status();
    }
    if (continuation.execution_generation > execution->generation) {
        out_reason = "continuation claims an execution generation that does not exist";
        return ok_status();
    }
    if (continuation.progress_generation > execution->progress_generation) {
        out_reason = "continuation claims progress beyond the authoritative frontier";
        return ok_status();
    }
    if (continuation.action_generation > execution->action_generation) {
        out_reason = "continuation claims an action generation that does not exist";
        return ok_status();
    }
    if (continuation.resume_ordinal > execution->action_frontier) {
        out_reason = "continuation claims a resume ordinal beyond the action frontier";
        return ok_status();
    }
    const ExecutionPolicy* policy = state.policies.find(execution->policy);

    // Unsupportable durable references are refused before any softer verdict.
    if (continuation.checkpoint.valid()) {
        const CheckpointRecord* checkpoint = state.checkpoints.find(continuation.checkpoint);
        if (checkpoint == nullptr) {
            out_reason = "continuation references a checkpoint that does not exist";
            return ok_status();
        }
        if (checkpoint->state == CheckpointState::Corrupt ||
            checkpoint->state == CheckpointState::Unsupported) {
            out_state = ContinuationState::Unsupported;
            out_reason = std::string("referenced checkpoint state is ") +
                         std::string(checkpoint_state_name(checkpoint->state));
            return ok_status();
        }
    }
    if (continuation.checkpoint_generation > execution->checkpoint_generation) {
        out_reason = "continuation claims a checkpoint generation that does not exist";
        return ok_status();
    }

    // An unresolved ambiguity at or below the resume point dominates.
    if (execution->blocked && execution->ambiguity.valid()) {
        const AmbiguityRecord* ambiguity = state.ambiguities.find(execution->ambiguity);
        if (ambiguity != nullptr && ambiguity_is_open(ambiguity->state) &&
            continuation.resume_ordinal >= ambiguity->action_ordinal) {
            out_state = ContinuationState::Ambiguous;
            out_reason = "an unresolved ambiguous action sits at or below this continuation point";
            return ok_status();
        }
    }

    if (continuation.epoch != state.epoch) {
        out_state = ContinuationState::Stale;
        out_reason = "continuation was issued under a previous coordinator epoch";
        return ok_status();
    }
    if (continuation.incarnation_generation != execution->incarnation_generation) {
        out_state = ContinuationState::Stale;
        out_reason = "continuation was issued for a previous execution incarnation";
        return ok_status();
    }
    if (continuation.execution_generation != execution->generation) {
        out_state = ContinuationState::Stale;
        out_reason = "continuation was issued for a previous execution generation";
        return ok_status();
    }
    if (continuation.policy_generation != execution->policy_generation) {
        if (policy != nullptr && policy->policy_change_invalidates_continuations) {
            out_reason = "continuation was issued under a policy generation that has been replaced";
            return ok_status();
        }
        out_state = ContinuationState::RevalidationRequired;
        out_reason = "continuation was issued under a previous policy generation";
        return ok_status();
    }
    if (continuation.bindings.refs.size() != execution->bindings.refs.size() ||
        !continuation.bindings.equals_generations(execution->bindings)) {
        if (policy != nullptr && policy->stale_binding_behavior == StaleBindingBehavior::Refuse) {
            out_reason = "a required binding generation changed and policy refuses revalidation";
            return ok_status();
        }
        out_state = ContinuationState::RevalidationRequired;
        out_reason = "a required binding generation changed";
        return ok_status();
    }
    if (continuation.checkpoint.valid()) {
        const CheckpointRecord* checkpoint = state.checkpoints.find(continuation.checkpoint);
        if (checkpoint != nullptr && !checkpoint_can_seed_continuation(checkpoint->state)) {
            out_state = ContinuationState::RevalidationRequired;
            out_reason = std::string("referenced checkpoint state is ") +
                         std::string(checkpoint_state_name(checkpoint->state));
            return ok_status();
        }
    }

    out_state = ContinuationState::Valid;
    out_reason = "all bound generations are current and the durable point is not ambiguous";
    return ok_status();
}

Status Runtime::validate_continuation(const CallerContext& caller, ExecutionId execution_id,
                                      ContinuationState& out, std::string& reason) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_caller(caller));
    const ExecutionRecord* execution = impl_->state.executions.find(execution_id);
    if (execution == nullptr) {
        return err(Code::NotFound, "execution is not present in the durable store");
    }
    if (!execution->continuation.valid()) {
        out = ContinuationState::Invalid;
        reason = "the execution has no current continuation";
        return ok_status();
    }
    const ContinuationRecord* continuation =
        impl_->state.continuations.find(execution->continuation);
    if (continuation == nullptr) {
        out = ContinuationState::Invalid;
        reason = "the execution's current continuation is missing from the durable store";
        return ok_status();
    }
    return validate_continuation_state(impl_->state, *continuation, out, reason);
}

// ---------------------------------------------------------------------------
// Classification and recovery
// ---------------------------------------------------------------------------
Status Runtime::classify(const CallerContext& caller, ExecutionId execution_id,
                         RecoveryPlan& out) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_caller(caller));
    const ExecutionRecord* execution = impl_->state.executions.find(execution_id);
    if (execution == nullptr) {
        return err(Code::NotFound, "execution is not present in the durable store");
    }
    RecoveryInput input;
    input.execution = execution;
    input.policy = impl_->policy_of(*execution);
    input.epoch = impl_->state.epoch;

    if (const ActionRecord* latest = impl_->latest_action(execution_id); latest != nullptr) {
        switch (latest->status) {
            case ActionStatus::Committed:
            case ActionStatus::Compensated:
            case ActionStatus::Abandoned:
                break;
            default:
                input.in_flight = latest;
                break;
        }
    }
    if (execution->checkpoint.valid()) {
        const CheckpointRecord* checkpoint = impl_->state.checkpoints.find(execution->checkpoint);
        if (checkpoint == nullptr) {
            input.checkpoint_unusable = true;
        } else if (checkpoint->state == CheckpointState::Corrupt ||
                   checkpoint->state == CheckpointState::Unsupported) {
            input.checkpoint_unusable = true;
        } else if (checkpoint_can_seed_continuation(checkpoint->state)) {
            input.current_checkpoint = checkpoint;
        }
    }
    std::vector<const CheckpointRecord*> candidates;
    for (const CheckpointRecord* checkpoint : impl_->state.checkpoints.canonical_order()) {
        if (checkpoint->execution != execution_id) {
            continue;
        }
        if (!checkpoint_can_seed_continuation(checkpoint->state)) {
            continue;
        }
        candidates.push_back(checkpoint);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const CheckpointRecord* a, const CheckpointRecord* b) {
                         return a->generation > b->generation;
                     });
    input.candidates = std::move(candidates);
    if (execution->ambiguity.valid()) {
        input.ambiguity = impl_->state.ambiguities.find(execution->ambiguity);
    }
    out = classify_recovery(input);
    return ok_status();
}

Status Runtime::recover(const CallerContext& caller, RequestId request, ExecutionId execution_id,
                        RecoveryOutcome& out) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    return recover_locked(caller, request, execution_id, out);
}

Status Runtime::recover_locked(const CallerContext& caller, RequestId request,
                               ExecutionId execution_id, RecoveryOutcome& out) {
    PEF_TRY(impl_->check_caller(caller));
    const ExecutionRecord* execution = impl_->state.executions.find(execution_id);
    if (execution == nullptr) {
        return err(Code::NotFound, "execution is not present in the durable store");
    }
    if (is_terminal_state(execution->lifecycle)) {
        return err(Code::LifecycleRefused,
                   std::string("cannot recover an execution that is ") +
                       std::string(lifecycle_name(execution->lifecycle)));
    }

    RecoveryPlan plan;
    {
        RecoveryInput input;
        input.execution = execution;
        input.policy = impl_->policy_of(*execution);
        input.epoch = impl_->state.epoch;
        if (const ActionRecord* latest = impl_->latest_action(execution_id); latest != nullptr) {
            switch (latest->status) {
                case ActionStatus::Committed:
                case ActionStatus::Compensated:
                case ActionStatus::Abandoned:
                    break;
                default:
                    input.in_flight = latest;
                    break;
            }
        }
        if (execution->checkpoint.valid()) {
            const CheckpointRecord* checkpoint =
                impl_->state.checkpoints.find(execution->checkpoint);
            if (checkpoint == nullptr || checkpoint->state == CheckpointState::Corrupt ||
                checkpoint->state == CheckpointState::Unsupported) {
                input.checkpoint_unusable = true;
            } else if (checkpoint_can_seed_continuation(checkpoint->state)) {
                input.current_checkpoint = checkpoint;
            }
        }
        std::vector<const CheckpointRecord*> candidates;
        for (const CheckpointRecord* checkpoint : impl_->state.checkpoints.canonical_order()) {
            if (checkpoint->execution == execution_id &&
                checkpoint_can_seed_continuation(checkpoint->state)) {
                candidates.push_back(checkpoint);
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const CheckpointRecord* a, const CheckpointRecord* b) {
                             return a->generation > b->generation;
                         });
        input.candidates = std::move(candidates);
        if (execution->ambiguity.valid()) {
            input.ambiguity = impl_->state.ambiguities.find(execution->ambiguity);
        }
        plan = classify_recovery(input);
    }

    const ExecutionPolicy* policy = impl_->policy_of(*execution);
    const bool flush = flush_for(policy);

    ExecutionRecord next = *execution;
    const ActionRecord* in_flight = nullptr;
    if (plan.action.valid()) {
        in_flight = impl_->state.find_action(plan.action, plan.action_generation);
    }

    AmbiguityRecord ambiguity;
    bool ambiguity_written = false;
    if (plan.decision == RecoveryDecision::AmbiguousCompletion ||
        plan.decision == RecoveryDecision::ManualResolutionRequired ||
        plan.decision == RecoveryDecision::Compensate) {
        if (in_flight != nullptr) {
            const AmbiguityRecord* existing =
                next.ambiguity.valid() ? impl_->state.ambiguities.find(next.ambiguity) : nullptr;
            if (existing != nullptr && ambiguity_is_open(existing->state)) {
                ambiguity = *existing;
            } else {
                ambiguity.generation = (existing != nullptr)
                                           ? existing->generation.next()
                                           : AmbiguityGeneration{1};
                ambiguity.id =
                    derive_ambiguity_id(execution_id, in_flight->id, ambiguity.generation);
                ambiguity.execution = execution_id;
                ambiguity.execution_generation = execution->generation;
                ambiguity.action = in_flight->id;
                ambiguity.action_generation = in_flight->generation;
                ambiguity.effect_class = in_flight->effect_class;
                ambiguity.action_ordinal = in_flight->sequence;
                ambiguity.state =
                    plan.decision == RecoveryDecision::AmbiguousCompletion
                        ? AmbiguityState::Open
                        : AmbiguityState::ManualResolutionRequired;
                ambiguity.reason = plan.explanation;
            }
            ActionRecord action = *in_flight;
            action.status = ActionStatus::Ambiguous;
            PEF_TRY(impl_->put_ambiguity(ambiguity, flush));
            ambiguity_written = true;
            PEF_TRY(impl_->put_action(action, flush));
            next.ambiguity = ambiguity.id;
            next.ambiguity_generation = ambiguity.generation;
            if (policy == nullptr || policy->ambiguity_blocks_later_progress) {
                next.blocked = true;
                next.blocked_above_ordinal = in_flight->sequence - 1;
            }
        }
    }

    bool committed = false;
    CommitId commit;
    if (plan.commit_from_receipt && in_flight != nullptr) {
        Impl::StagedCommit staged;
        PEF_TRY(impl_->stage_commit(*execution, *in_flight, request, in_flight->receipt, flush,
                                    staged));
        // stage_commit installed the promoted aggregate; continue from it.
        next = *impl_->state.executions.find(execution_id);
        committed = true;
        commit = staged.group.commit.id;
    }

    if (plan.decision == RecoveryDecision::ResumeFromCheckpoint && plan.checkpoint.valid()) {
        next.checkpoint = plan.checkpoint;
        next.checkpoint_generation = plan.checkpoint_generation;
    }

    RecoveryRecord recovery;
    recovery.generation = execution->recovery_generation.established()
                              ? execution->recovery_generation.next()
                              : RecoveryGeneration{1};
    recovery.id = derive_recovery_id(execution_id, recovery.generation);
    recovery.execution = execution_id;
    recovery.execution_generation = execution->generation;
    recovery.decision = static_cast<std::uint8_t>(plan.decision);
    recovery.continuation = next.continuation;
    recovery.continuation_generation = next.continuation_generation;
    recovery.checkpoint = next.checkpoint;
    recovery.checkpoint_generation = next.checkpoint_generation;
    recovery.epoch = impl_->state.epoch;
    recovery.evidence = in_flight != nullptr ? in_flight->receipt : Evidence{};
    recovery.explanation = plan.explanation;
    next.recovery = recovery.id;
    next.recovery_generation = recovery.generation;

    // RecoveryRequired is the mandatory stop. Only a decision that permits
    // automatic continuation may leave it, and only into RECOVERING.
    if (execution->lifecycle == Lifecycle::RecoveryRequired &&
        recovery_allows_automatic_continue(plan.decision)) {
        PEF_TRY(require_transition(next.lifecycle, Lifecycle::Recovering, "recover"));
        next.lifecycle = Lifecycle::Recovering;
    }
    next.epoch = impl_->state.epoch;

    (void)ambiguity_written;
    PEF_TRY(impl_->put_recovery(recovery, flush));
    PEF_TRY(impl_->put_execution(next, flush));

    if (request.valid()) {
        impl_->remember_request(request, CachedOutcome{Code::Ok, recovery.id.value(),
                                                       static_cast<std::uint64_t>(plan.decision)});
    }
    out.plan = plan;
    out.applied = true;
    out.committed = committed;
    out.commit = commit;
    out.recovery = recovery.id;
    out.recovery_generation = recovery.generation;
    out.ambiguity = next.ambiguity;
    out.ambiguity_generation = next.ambiguity_generation;
    out.continuation = next.continuation;
    out.continuation_generation = next.continuation_generation;
    return ok_status();
}

// ---------------------------------------------------------------------------
// Ambiguity resolution
// ---------------------------------------------------------------------------
Status Runtime::resolve_ambiguity(const ResolveAmbiguityRequest& request,
                                  ResolveAmbiguityResult& out) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    PEF_TRY(impl_->check_caller(request.caller));
    const ExecutionRecord* execution = impl_->state.executions.find(request.execution);
    if (execution == nullptr) {
        return err(Code::NotFound, "execution is not present in the durable store");
    }
    const AmbiguityRecord* ambiguity = impl_->state.ambiguities.find(request.ambiguity);
    if (ambiguity == nullptr) {
        return err(Code::NotFound, "ambiguity record is not present in the durable store");
    }
    if (ambiguity->execution != request.execution) {
        return err(Code::InvalidArgument, "ambiguity does not belong to this execution");
    }
    if (ambiguity->generation != request.ambiguity_generation) {
        return err(Code::StaleContinuation, "ambiguity generation does not match");
    }
    if (!ambiguity_is_open(ambiguity->state)) {
        return err(Code::DuplicateRequest, "ambiguity is already resolved");
    }
    const ActionRecord* action =
        impl_->state.find_action(ambiguity->action, ambiguity->action_generation);
    if (action == nullptr) {
        return err(Code::CorruptState, "the ambiguous action is missing from the durable store");
    }
    if (request.resolution == AmbiguityState::Open) {
        return err(Code::InvalidArgument, "OPEN is not a resolution");
    }

    const ExecutionPolicy* policy = impl_->policy_of(*execution);
    const bool flush = flush_for(policy);

    AmbiguityRecord resolved = *ambiguity;
    resolved.state = request.resolution;
    resolved.resolution_evidence = request.evidence;
    resolved.resolution_note = request.note;

    ExecutionRecord next = *execution;
    bool committed = false;
    CommitId commit;
    ProgressGeneration progress_generation = next.progress_generation;

    if (request.resolution == AmbiguityState::ResolvedApplied ||
        request.resolution == AmbiguityState::Accepted) {
        Impl::StagedCommit staged;
        PEF_TRY(impl_->stage_commit(*execution, *action, request.request, request.evidence, flush,
                                    staged));
        next = *impl_->state.executions.find(request.execution);
        committed = true;
        commit = staged.group.commit.id;
        progress_generation = staged.progress_generation;
        next.blocked = false;
        next.blocked_above_ordinal = 0;
    } else if (request.resolution == AmbiguityState::ResolvedNotApplied) {
        ActionRecord failed = *action;
        failed.status = ActionStatus::Failed;
        failed.receipt = request.evidence;
        PEF_TRY(impl_->put_action(failed, flush));
        next.blocked = false;
        next.blocked_above_ordinal = 0;
        out.replay_admitted = true;
    } else if (request.resolution == AmbiguityState::Abandoned) {
        ActionRecord abandoned = *action;
        abandoned.status = ActionStatus::Abandoned;
        abandoned.receipt = request.evidence;
        PEF_TRY(impl_->put_action(abandoned, flush));
        next.blocked = false;
        next.blocked_above_ordinal = 0;
    } else {
        // ManualResolutionRequired keeps the block in place.
        next.blocked = true;
    }

    PEF_TRY(impl_->put_ambiguity(resolved, flush));
    PEF_TRY(impl_->put_execution(next, flush));

    if (request.request.valid()) {
        impl_->remember_request(
            request.request,
            CachedOutcome{Code::Ok, resolved.id.value(), static_cast<std::uint64_t>(resolved.state)});
    }
    out.state = resolved.state;
    out.committed = committed;
    out.commit = commit;
    out.progress_generation = progress_generation;
    out.ambiguity = resolved.id;
    out.ambiguity_generation = resolved.generation;
    out.action = action->id;
    out.action_generation = action->generation;
    return ok_status();
}

// ---------------------------------------------------------------------------
// Lifecycle operations
// ---------------------------------------------------------------------------
Status Runtime::fence(const CallerContext& caller, RequestId request, ExecutionId execution_id,
                      std::string reason) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const ExecutionRecord* execution = nullptr;
    PEF_TRY(impl_->find_checked(caller, execution_id, execution));
    CachedOutcome cached;
    if (impl_->lookup_request(request, cached)) {
        return ok_status();
    }
    (void)reason;
    PEF_TRY(impl_->persist_transition(*execution, Lifecycle::Fenced, "fence", true));
    impl_->remember_request(request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

Status Runtime::retire(const CallerContext& caller, RequestId request, ExecutionId execution_id,
                       std::string reason) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const ExecutionRecord* execution = nullptr;
    PEF_TRY(impl_->find_checked(caller, execution_id, execution));
    CachedOutcome cached;
    if (impl_->lookup_request(request, cached)) {
        return ok_status();
    }
    (void)reason;
    if (execution->ambiguity.valid()) {
        const AmbiguityRecord* ambiguity = impl_->state.ambiguities.find(execution->ambiguity);
        if (ambiguity != nullptr && ambiguity_is_open(ambiguity->state)) {
            return err(Code::UnresolvedAmbiguity,
                       "an execution with an unresolved ambiguity cannot be retired");
        }
    }
    PEF_TRY(impl_->persist_transition(*execution, Lifecycle::Retired, "retire", true));
    impl_->remember_request(request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

Status Runtime::cancel(const CallerContext& caller, RequestId request, ExecutionId execution_id,
                       std::string reason) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const ExecutionRecord* execution = nullptr;
    PEF_TRY(impl_->find_checked(caller, execution_id, execution));
    CachedOutcome cached;
    if (impl_->lookup_request(request, cached)) {
        return ok_status();
    }
    (void)reason;
    PEF_TRY(impl_->persist_transition(*execution, Lifecycle::Cancelled, "cancel", true));
    impl_->remember_request(request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

Status Runtime::drain(const CallerContext& caller, RequestId request, ExecutionId execution_id) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const ExecutionRecord* execution = nullptr;
    PEF_TRY(impl_->find_checked(caller, execution_id, execution));
    CachedOutcome cached;
    if (impl_->lookup_request(request, cached)) {
        return ok_status();
    }
    PEF_TRY(impl_->persist_transition(*execution, Lifecycle::Draining, "drain", true));
    impl_->remember_request(request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

Status Runtime::complete_execution(const CallerContext& caller, RequestId request,
                                   ExecutionId execution_id) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const ExecutionRecord* execution = nullptr;
    PEF_TRY(impl_->find_checked(caller, execution_id, execution));
    CachedOutcome cached;
    if (impl_->lookup_request(request, cached)) {
        return ok_status();
    }
    if (execution->ambiguity.valid()) {
        const AmbiguityRecord* ambiguity = impl_->state.ambiguities.find(execution->ambiguity);
        if (ambiguity != nullptr && ambiguity_is_open(ambiguity->state)) {
            return err(Code::UnresolvedAmbiguity,
                       "an execution with an unresolved ambiguity cannot complete");
        }
    }
    PEF_TRY(impl_->persist_transition(*execution, Lifecycle::Completed, "complete", true));
    impl_->remember_request(request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

Status Runtime::advance_execution_generation(const CallerContext& caller, RequestId request,
                                             ExecutionId execution_id, std::string reason) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const ExecutionRecord* execution = nullptr;
    PEF_TRY(impl_->find_checked(caller, execution_id, execution));
    CachedOutcome cached;
    if (impl_->lookup_request(request, cached)) {
        return ok_status();
    }
    if (is_terminal_state(execution->lifecycle)) {
        return err(Code::LifecycleRefused,
                   std::string("cannot advance the generation of an execution that is ") +
                       std::string(lifecycle_name(execution->lifecycle)));
    }
    (void)reason;
    // Every continuation that bound the previous generation is stale by
    // construction; the generation change is what makes it stale.
    for (const auto& continuation : impl_->state.continuations.insertion_order()) {
        if (continuation.execution != execution_id ||
            continuation.state == ContinuationState::Stale) {
            continue;
        }
        ContinuationRecord stale = continuation;
        stale.state = ContinuationState::Stale;
        PEF_TRY(impl_->put_continuation(stale, true));
    }
    ExecutionRecord next = *execution;
    if (next.lease.valid()) {
        if (const LeaseRecord* lease = impl_->state.leases.find(next.lease);
            lease != nullptr && lease->state == LeaseState::Active) {
            LeaseRecord revoked = *lease;
            revoked.state = LeaseState::Revoked;
            PEF_TRY(impl_->put_lease(revoked, true));
        }
        next.lease = LeaseId{};
        next.worker = WorkerId{};
        next.boot = WorkerBootId{};
    }
    const bool was_active = is_active_state(next.lifecycle);
    next.generation = next.generation.next();
    next.continuation = ContinuationId{};
    next.continuation_generation = ContinuationGeneration{};
    if (was_active) {
        PEF_TRY(require_transition(next.lifecycle, Lifecycle::RecoveryRequired,
                                   "generation advance"));
        next.lifecycle = Lifecycle::RecoveryRequired;
    }
    next.epoch = impl_->state.epoch;
    PEF_TRY(impl_->put_execution(next, true));
    impl_->remember_request(request, CachedOutcome{Code::Ok, next.generation.value(), 0});
    return ok_status();
}

Status Runtime::suspend_begin(const CallerContext& caller, RequestId request,
                              ExecutionId execution_id) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const ExecutionRecord* execution = nullptr;
    PEF_TRY(impl_->find_checked(caller, execution_id, execution));
    CachedOutcome cached;
    if (impl_->lookup_request(request, cached)) {
        return ok_status();
    }
    PEF_TRY(impl_->persist_transition(*execution, Lifecycle::Suspending, "suspend", true));
    impl_->remember_request(request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

Status Runtime::suspend_commit(const CallerContext& caller, RequestId request,
                               ExecutionId execution_id) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const ExecutionRecord* execution = nullptr;
    PEF_TRY(impl_->find_checked(caller, execution_id, execution));
    CachedOutcome cached;
    if (impl_->lookup_request(request, cached)) {
        return ok_status();
    }
    PEF_TRY(impl_->persist_transition(*execution, Lifecycle::Suspended, "suspend", true));
    impl_->remember_request(request, CachedOutcome{Code::Ok, 0, 0});
    return ok_status();
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------
Status Runtime::list_executions(std::vector<ExecutionRecord>& out) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    out.clear();
    for (const ExecutionRecord* execution : impl_->state.executions.canonical_order()) {
        out.push_back(*execution);
    }
    return ok_status();
}

Status Runtime::query(ExecutionId execution_id, ExecutionView& out) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const ExecutionRecord* execution = impl_->state.executions.find(execution_id);
    if (execution == nullptr) {
        out = ExecutionView{};
        return err(Code::NotFound, "execution is not present in the durable store");
    }
    out = ExecutionView{};
    out.found = true;
    out.execution = *execution;
    if (const ExecutionPolicy* policy = impl_->policy_of(*execution); policy != nullptr) {
        out.policy = *policy;
    }
    if (execution->lease.valid()) {
        if (const LeaseRecord* lease = impl_->state.leases.find(execution->lease);
            lease != nullptr) {
            out.lease = *lease;
        }
    }
    if (execution->continuation.valid()) {
        if (const ContinuationRecord* continuation =
                impl_->state.continuations.find(execution->continuation);
            continuation != nullptr) {
            out.continuation = *continuation;
        }
    }
    if (execution->checkpoint.valid()) {
        if (const CheckpointRecord* checkpoint =
                impl_->state.checkpoints.find(execution->checkpoint);
            checkpoint != nullptr) {
            out.checkpoint = *checkpoint;
        }
    }
    if (execution->recovery.valid()) {
        if (const RecoveryRecord* recovery = impl_->state.recoveries.find(execution->recovery);
            recovery != nullptr) {
            out.recovery = *recovery;
        }
    }
    if (execution->ambiguity.valid()) {
        if (const AmbiguityRecord* ambiguity =
                impl_->state.ambiguities.find(execution->ambiguity);
            ambiguity != nullptr) {
            out.ambiguity = *ambiguity;
        }
    }
    const auto index_it = impl_->action_index.find(execution_id);
    if (index_it != impl_->action_index.end()) {
        out.action_count = index_it->second.size();
        for (auto it = index_it->second.rbegin(); it != index_it->second.rend(); ++it) {
            if (out.actions.size() >= impl_->config.max_view_actions) {
                break;
            }
            if (const ActionRecord* action = impl_->state.actions.find(*it); action != nullptr) {
                out.actions.push_back(*action);
            }
        }
    }
    if (const auto it = impl_->checkpoint_index.find(execution_id);
        it != impl_->checkpoint_index.end()) {
        for (CheckpointId id : it->second) {
            if (out.checkpoints.size() >= impl_->config.max_view_actions) {
                break;
            }
            if (const CheckpointRecord* checkpoint = impl_->state.checkpoints.find(id);
                checkpoint != nullptr) {
                out.checkpoints.push_back(*checkpoint);
            }
        }
        std::stable_sort(out.checkpoints.begin(), out.checkpoints.end(),
                         [](const CheckpointRecord& a, const CheckpointRecord& b) {
                             return a.generation > b.generation;
                         });
    }
    if (const auto it = impl_->replay_index.find(execution_id); it != impl_->replay_index.end()) {
        for (ReplayId id : it->second) {
            if (out.replays.size() >= impl_->config.max_view_actions) {
                break;
            }
            if (const ReplayRecord* replay = impl_->state.replays.find(id); replay != nullptr) {
                out.replays.push_back(*replay);
            }
        }
    }
    if (const auto it = impl_->commit_index.find(execution_id); it != impl_->commit_index.end()) {
        out.commit_count = it->second.size();
    }
    return ok_status();
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------
Status Runtime::take_snapshot(bool truncate_journal) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    if (!impl_->open) {
        return err(Code::NotFound, "runtime is not open");
    }
    PEF_TRY(impl_->store.flush());
    return impl_->store.write_snapshot(impl_->state, truncate_journal);
}

Status Runtime::audit(AuditReport& out) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    out = audit_state(impl_->state);
    return ok_status();
}

Status Runtime::flush() {
    std::lock_guard<std::mutex> guard(impl_->mu);
    if (!impl_->open) {
        return err(Code::NotFound, "runtime is not open");
    }
    return impl_->store.flush();
}

// ---------------------------------------------------------------------------
// Worker identity
// ---------------------------------------------------------------------------
WorkerId derive_worker_id(std::string_view name) {
    HashBuilder h;
    h.u64(0x2001);
    h.text(name);
    return WorkerId{h.digest()};
}

WorkerBootId mint_worker_boot_id(WorkerId worker) {
    // A boot identity must differ on every process start, including a restart
    // that happens to reuse the operating-system process id. It is composed from
    // the worker identity, an entropy source that mixes the process creation
    // time, and a per-process monotonic counter.
    static std::atomic<std::uint64_t> counter{0};
    HashBuilder h;
    h.u64(0x2002);
    h << worker;
    h.u64(counter.fetch_add(1, std::memory_order_relaxed));
    h.u64(process_boot_marker());
    return WorkerBootId{h.digest()};
}

}  // namespace pef
