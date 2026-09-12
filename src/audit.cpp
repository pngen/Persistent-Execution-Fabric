// Persistent Execution Fabric - invariant auditor.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/audit.hpp"

#include <algorithm>
#include <set>
#include <sstream>

#include "pef/recovery.hpp"
#include "pef/version.hpp"

namespace pef {
namespace {

void add(AuditReport& report, std::string code, std::string detail, ExecutionId execution = {}) {
    AuditFinding finding;
    finding.code = std::move(code);
    finding.detail = std::move(detail);
    finding.execution = execution;
    report.findings.push_back(std::move(finding));
}

[[nodiscard]] std::string hex_id(std::uint64_t value) { return hex64(value); }

}  // namespace

std::string AuditReport::render() const {
    std::ostringstream out;
    out << "audit store=" << hex_id(store.value()) << " epoch=" << epoch.value()
        << " sequence=" << sequence << " violations=" << findings.size() << "\n";
    out << "  executions=" << counts.executions << " actions=" << counts.actions
        << " progress=" << counts.progress << " checkpoints=" << counts.checkpoints
        << " continuations=" << counts.continuations << " leases=" << counts.leases
        << " replays=" << counts.replays << " ambiguities=" << counts.ambiguities
        << " commits=" << counts.commits << " recoveries=" << counts.recoveries
        << " policies=" << counts.policies << "\n";
    // Findings are already produced in a deterministic order; sort defensively
    // by (code, execution, detail) so rendering never depends on iteration.
    std::vector<const AuditFinding*> ordered;
    ordered.reserve(findings.size());
    for (const auto& finding : findings) {
        ordered.push_back(&finding);
    }
    std::sort(ordered.begin(), ordered.end(), [](const AuditFinding* a, const AuditFinding* b) {
        if (a->code != b->code) {
            return a->code < b->code;
        }
        if (a->execution != b->execution) {
            return a->execution < b->execution;
        }
        return a->detail < b->detail;
    });
    for (const auto* finding : ordered) {
        out << "  VIOLATION " << finding->code;
        if (finding->execution.valid()) {
            out << " execution=" << hex_id(finding->execution.value());
        }
        out << " :: " << finding->detail << "\n";
    }
    return out.str();
}

std::uint64_t AuditReport::digest() const {
    HashBuilder h;
    h << store << epoch;
    h.u64(sequence);
    h.u64(counts.executions);
    h.u64(counts.actions);
    h.u64(counts.progress);
    h.u64(counts.checkpoints);
    h.u64(counts.continuations);
    h.u64(counts.leases);
    h.u64(counts.replays);
    h.u64(counts.ambiguities);
    h.u64(counts.commits);
    h.u64(counts.recoveries);
    h.u64(counts.policies);
    std::vector<const AuditFinding*> ordered;
    ordered.reserve(findings.size());
    for (const auto& finding : findings) {
        ordered.push_back(&finding);
    }
    std::sort(ordered.begin(), ordered.end(), [](const AuditFinding* a, const AuditFinding* b) {
        if (a->code != b->code) {
            return a->code < b->code;
        }
        if (a->execution != b->execution) {
            return a->execution < b->execution;
        }
        return a->detail < b->detail;
    });
    for (const auto* finding : ordered) {
        h.text(finding->code);
        h << finding->execution;
        h.text(finding->detail);
    }
    return h.digest();
}

AuditReport audit_state(const DurableState& state) {
    AuditReport report;
    report.epoch = state.epoch;
    report.store = state.store;
    report.sequence = state.sequence;
    report.counts.executions = state.executions.size();
    report.counts.actions = state.actions.size();
    report.counts.progress = state.progress.size();
    report.counts.checkpoints = state.checkpoints.size();
    report.counts.continuations = state.continuations.size();
    report.counts.leases = state.leases.size();
    report.counts.replays = state.replays.size();
    report.counts.ambiguities = state.ambiguities.size();
    report.counts.commits = state.commits.size();
    report.counts.recoveries = state.recoveries.size();
    report.counts.policies = state.policies.size();

    if (!state.store.valid()) {
        add(report, "STORE_ID_NIL", "durable state carries no store id");
    }
    if (state.schema != kPersistenceSchemaVersion) {
        add(report, "STORE_SCHEMA_UNEXPECTED", "durable state schema is not the supported version");
    }

    // ---- executions --------------------------------------------------------
    for (const auto& execution : state.executions.insertion_order()) {
        const ExecutionId id = execution.id;
        if (!id.valid()) {
            add(report, "EXECUTION_ID_NIL", "execution record carries a nil id");
            continue;
        }
        if (execution.generation.is_none()) {
            add(report, "EXECUTION_GENERATION_ZERO", "execution generation is not established", id);
        }
        if (execution.policy_generation.is_none()) {
            add(report, "EXECUTION_POLICY_GENERATION_ZERO",
                "execution references no policy generation", id);
        }
        if (execution.epoch > state.epoch) {
            add(report, "EXECUTION_EPOCH_AHEAD",
                "execution records a coordinator epoch the store has not reached", id);
        }
        const ExecutionPolicy* policy = state.policies.find(execution.policy);
        if (policy == nullptr) {
            add(report, "EXECUTION_POLICY_MISSING", "execution references a missing policy", id);
        }

        // Current incarnation identity must be derivable, not arbitrary.
        if (execution.incarnation.valid() || execution.incarnation_generation.established()) {
            if (execution.incarnation_generation.is_none()) {
                add(report, "INCARNATION_GENERATION_ZERO",
                    "execution has an incarnation but no incarnation generation", id);
            } else {
                const ExecutionIncarnationId expected =
                    derive_incarnation_id(id, execution.incarnation_generation);
                if (expected != execution.incarnation) {
                    add(report, "INCARNATION_ID_MISMATCH",
                        "execution incarnation id is not the derived identity for its generation",
                        id);
                }
            }
        }

        // ---- leases and worker authority ----------------------------------
        const LeaseRecord* lease = state.leases.find(execution.lease);
        if (execution.has_lease()) {
            if (lease == nullptr) {
                add(report, "EXECUTION_LEASE_MISSING", "execution references a missing lease", id);
            } else {
                if (lease->state != LeaseState::Active) {
                    add(report, "EXECUTION_REFERENCES_INACTIVE_LEASE",
                        "execution references a lease that is not active", id);
                }
                if (lease->execution != id) {
                    add(report, "LEASE_EXECUTION_MISMATCH",
                        "lease belongs to a different execution", id);
                }
                if (lease->worker != execution.worker || lease->boot != execution.boot) {
                    add(report, "LEASE_WORKER_MISMATCH",
                        "lease worker or boot does not match the execution binding", id);
                }
                if (lease->epoch != state.epoch) {
                    add(report, "LEASE_STALE_EPOCH",
                        "active lease carries an epoch that is not current", id);
                }
                if (lease->incarnation_generation != execution.incarnation_generation) {
                    add(report, "LEASE_INCARNATION_MISMATCH",
                        "active lease binds a different incarnation generation", id);
                }
                if (lease->policy_generation != execution.policy_generation) {
                    add(report, "LEASE_POLICY_MISMATCH",
                        "active lease binds a different policy generation", id);
                }
            }
            if (!execution.worker.valid() || !execution.boot.valid()) {
                add(report, "EXECUTION_WORKER_MISSING",
                    "execution holds a lease but records no worker binding", id);
            }
        } else if (execution.has_worker()) {
            add(report, "EXECUTION_WORKER_WITHOUT_LEASE",
                "execution records a worker binding but no lease", id);
        }

        // Terminal states must not retain authority.
        if (is_terminal_state(execution.lifecycle) && execution.has_lease()) {
            add(report, "TERMINAL_EXECUTION_HOLDS_LEASE",
                std::string("lifecycle ") + std::string(lifecycle_name(execution.lifecycle)) +
                    " still holds a lease",
                id);
        }
        if (execution.lifecycle == Lifecycle::Retired && lease != nullptr &&
            lease->state == LeaseState::Active) {
            add(report, "RETIRED_EXECUTION_ACTIVE_LEASE",
                "a retired execution has an active lease", id);
        }
        if (is_terminal_state(execution.lifecycle) && execution.ambiguity.valid()) {
            const AmbiguityRecord* ambiguity = state.ambiguities.find(execution.ambiguity);
            if (ambiguity != nullptr && ambiguity_is_open(ambiguity->state)) {
                add(report, "TERMINAL_EXECUTION_OPEN_AMBIGUITY",
                    "a terminal execution still has an open ambiguity", id);
            }
        }

        // ---- continuation --------------------------------------------------
        if (execution.continuation.valid()) {
            const ContinuationRecord* continuation =
                state.continuations.find(execution.continuation);
            if (continuation == nullptr) {
                add(report, "EXECUTION_CONTINUATION_MISSING",
                    "execution references a missing continuation", id);
            } else {
                if (continuation->execution != id) {
                    add(report, "CONTINUATION_EXECUTION_MISMATCH",
                        "continuation belongs to a different execution", id);
                }
                if (continuation->execution_generation > execution.generation) {
                    add(report, "CONTINUATION_GENERATION_AHEAD",
                        "continuation claims an execution generation that does not exist", id);
                }
                if (continuation->progress_generation > execution.progress_generation) {
                    add(report, "CONTINUATION_PROGRESS_AHEAD",
                        "continuation claims progress beyond the authoritative frontier", id);
                }
                if (continuation->action_generation > execution.action_generation) {
                    add(report, "CONTINUATION_ACTION_AHEAD",
                        "continuation claims an action generation that does not exist", id);
                }
                if (continuation->checkpoint.valid()) {
                    const CheckpointRecord* checkpoint =
                        state.checkpoints.find(continuation->checkpoint);
                    if (checkpoint == nullptr) {
                        add(report, "CONTINUATION_CHECKPOINT_MISSING",
                            "continuation references a missing checkpoint", id);
                    } else if (checkpoint->state == CheckpointState::Corrupt ||
                               checkpoint->state == CheckpointState::Unsupported) {
                        add(report, "CONTINUATION_CHECKPOINT_UNUSABLE",
                            "continuation references a checkpoint that cannot seed execution", id);
                    }
                }
                if (continuation->policy_generation > execution.policy_generation) {
                    add(report, "CONTINUATION_POLICY_AHEAD",
                        "continuation claims a policy generation that does not exist", id);
                }
                if (continuation->state == ContinuationState::Valid &&
                    continuation->epoch != state.epoch) {
                    add(report, "VALID_CONTINUATION_STALE_EPOCH",
                        "a continuation marked VALID carries a non-current epoch", id);
                }
            }
        }

        // ---- checkpoint ----------------------------------------------------
        if (execution.checkpoint.valid()) {
            const CheckpointRecord* checkpoint = state.checkpoints.find(execution.checkpoint);
            if (checkpoint == nullptr) {
                add(report, "EXECUTION_CHECKPOINT_MISSING",
                    "execution references a missing checkpoint", id);
            } else {
                if (checkpoint->execution != id) {
                    add(report, "CHECKPOINT_EXECUTION_MISMATCH",
                        "checkpoint belongs to a different execution", id);
                }
                if (checkpoint->content_hash == 0) {
                    add(report, "CHECKPOINT_CONTENT_HASH_ZERO",
                        "checkpoint carries no content integrity digest", id);
                }
                if (checkpoint->state == CheckpointState::Corrupt) {
                    add(report, "CURRENT_CHECKPOINT_CORRUPT",
                        "the execution's current checkpoint is marked corrupt", id);
                }
            }
        }

        // ---- progress ------------------------------------------------------
        std::vector<const ProgressRecord*> committed;
        for (const auto& progress : state.progress.insertion_order()) {
            if (progress.execution != id) {
                continue;
            }
            if (!progress.id.valid()) {
                add(report, "PROGRESS_ID_NIL", "progress record carries a nil id", id);
            }
            if (progress.generation.is_none()) {
                add(report, "PROGRESS_GENERATION_ZERO",
                    "progress record carries no generation", id);
            }
            if (progress.generation > execution.progress_generation) {
                add(report, "PROGRESS_BEYOND_FRONTIER",
                    "progress generation exceeds the execution's authoritative progress", id);
            }
            if (progress.execution_generation > execution.generation) {
                add(report, "PROGRESS_EXECUTION_GENERATION_AHEAD",
                    "progress record claims a future execution generation", id);
            }
            if (progress.committed) {
                committed.push_back(&progress);
            }
            if (progress.committed) {
                const CommitRecord* commit = state.commits.find(progress.commit);
                if (commit == nullptr) {
                    add(report, "COMMITTED_PROGRESS_WITHOUT_COMMIT",
                        "committed progress has no durable commit record", id);
                }
            }
            if (execution.blocked && progress.committed &&
                progress.ordinal > execution.blocked_above_ordinal) {
                add(report, "PROGRESS_BEYOND_AMBIGUOUS_ACTION",
                    "committed progress exists above an unresolved ambiguous action", id);
            }
        }
        std::sort(committed.begin(), committed.end(),
                  [](const ProgressRecord* a, const ProgressRecord* b) {
                      return a->generation < b->generation;
                  });
        for (std::size_t i = 0; i < committed.size(); ++i) {
            const std::uint64_t expected = static_cast<std::uint64_t>(i) + 1;
            if (committed[i]->generation.value() != expected) {
                add(report, "PROGRESS_GENERATION_NOT_CONTIGUOUS",
                    "committed progress generations are not contiguous from 1", id);
                break;
            }
        }
        for (std::size_t i = 1; i < committed.size(); ++i) {
            if (committed[i]->ordinal <= committed[i - 1]->ordinal) {
                add(report, "PROGRESS_ORDINAL_NOT_MONOTONIC",
                    "committed progress ordinals do not strictly increase", id);
                break;
            }
        }
        if (committed.size() != execution.committed_actions) {
            add(report, "COMMITTED_ACTION_COUNT_MISMATCH",
                "execution's committed action counter disagrees with its committed progress", id);
        }
        if (!committed.empty()) {
            if (execution.progress_generation != committed.back()->generation) {
                add(report, "FRONTIER_NOT_LATEST_COMMIT",
                    "authoritative progress generation is not the latest durable commit", id);
            }
            if (execution.last_commit != committed.back()->commit) {
                add(report, "LAST_COMMIT_MISMATCH",
                    "execution's last commit identity is not the latest durable commit", id);
            }
        } else if (execution.progress_generation.established()) {
            add(report, "FRONTIER_WITHOUT_COMMITS",
                "execution reports a progress generation with no committed progress", id);
        }

        // ---- ambiguity -----------------------------------------------------
        if (execution.ambiguity.valid()) {
            const AmbiguityRecord* ambiguity = state.ambiguities.find(execution.ambiguity);
            if (ambiguity == nullptr) {
                add(report, "EXECUTION_AMBIGUITY_MISSING",
                    "execution references a missing ambiguity record", id);
            } else {
                if (ambiguity->execution != id) {
                    add(report, "AMBIGUITY_EXECUTION_MISMATCH",
                        "ambiguity record belongs to a different execution", id);
                }
                if (!ambiguity_is_open(ambiguity->state) && execution.blocked) {
                    add(report, "BLOCK_WITH_RESOLVED_AMBIGUITY",
                        "execution is blocked but its ambiguity is resolved", id);
                }
            }
        } else if (execution.blocked) {
            add(report, "BLOCK_WITHOUT_AMBIGUITY",
                "execution is blocked but references no ambiguity record", id);
        }

        // ---- recovery ------------------------------------------------------
        if (execution.recovery.valid() && state.recoveries.find(execution.recovery) == nullptr) {
            add(report, "EXECUTION_RECOVERY_MISSING",
                "execution references a missing recovery record", id);
        }

        // ---- bindings ------------------------------------------------------
        if (const auto defect = binding_set_defect(execution.bindings); defect.has_value()) {
            add(report, "EXECUTION_BINDING_SET_DEFECT", std::string(*defect), id);
        }
    }

    // ---- actions -----------------------------------------------------------
    for (const auto& action : state.actions.insertion_order()) {
        if (!action.id.valid()) {
            add(report, "ACTION_ID_NIL", "action record carries a nil id");
            continue;
        }
        if (action.generation.is_none()) {
            add(report, "ACTION_GENERATION_ZERO", "action record carries no generation", action.execution);
        }
        const ExecutionRecord* execution = state.executions.find(action.execution);
        if (execution == nullptr) {
            add(report, "ACTION_EXECUTION_MISSING", "action references a missing execution");
            continue;
        }
        if (action.execution_generation > execution->generation) {
            add(report, "ACTION_EXECUTION_GENERATION_AHEAD",
                "action claims a future execution generation", action.execution);
        }
        if (action.sequence == 0) {
            add(report, "ACTION_ORDINAL_ZERO", "action has no ordinal", action.execution);
        }
        if (action.sequence > execution->action_frontier) {
            add(report, "ACTION_BEYOND_FRONTIER",
                "action ordinal exceeds the execution's action frontier", action.execution);
        }
        if (requires_request_key(action.effect_class) && action.request_key.empty()) {
            add(report, "ACTION_MISSING_REQUEST_KEY",
                "an action whose class requires a request key has none", action.execution);
        }
        if (action.sealed() && !action.receipt.present() &&
            action.status == ActionStatus::EffectApplied) {
            add(report, "ACTION_SEALED_WITHOUT_RECEIPT",
                "action is sealed as applied without durable evidence", action.execution);
        }
        if (action.status == ActionStatus::Committed) {
            const CommitRecord* commit = state.commits.find(action.commit);
            if (commit == nullptr) {
                add(report, "COMMITTED_ACTION_WITHOUT_COMMIT",
                    "action is committed but has no durable commit record", action.execution);
            } else {
                if (commit->action != action.id || commit->action_generation != action.generation) {
                    add(report, "COMMIT_ACTION_MISMATCH",
                        "commit record does not identify the committed action", action.execution);
                }
            }
        }
        if (action.replayed_from.valid()) {
            const ActionRecord* original = state.find_latest_action(action.replayed_from);
            if (original == nullptr) {
                add(report, "REPLAY_ORIGIN_MISSING",
                    "replayed action references a missing original action", action.execution);
            }
        }
    }

    // ---- checkpoints -------------------------------------------------------
    for (const auto& checkpoint : state.checkpoints.insertion_order()) {
        if (!checkpoint.id.valid()) {
            add(report, "CHECKPOINT_ID_NIL", "checkpoint record carries a nil id");
            continue;
        }
        const ExecutionRecord* execution = state.executions.find(checkpoint.execution);
        if (execution == nullptr) {
            add(report, "CHECKPOINT_EXECUTION_MISSING", "checkpoint references a missing execution");
            continue;
        }
        if (checkpoint.content_hash == 0) {
            add(report, "CHECKPOINT_HASH_ZERO",
                "checkpoint carries no content integrity digest", checkpoint.execution);
        }
        if (checkpoint.progress_generation > execution->progress_generation) {
            add(report, "CHECKPOINT_PROGRESS_AHEAD",
                "checkpoint claims progress beyond the authoritative frontier",
                checkpoint.execution);
        }
        if (checkpoint.generation.is_none()) {
            add(report, "CHECKPOINT_GENERATION_ZERO", "checkpoint carries no generation",
                checkpoint.execution);
        }
        if (checkpoint.parent.valid()) {
            const CheckpointRecord* parent = state.checkpoints.find(checkpoint.parent);
            if (parent == nullptr) {
                add(report, "CHECKPOINT_LINEAGE_BREAK",
                    "checkpoint references a missing parent", checkpoint.execution);
            } else {
                if (parent->execution != checkpoint.execution) {
                    add(report, "CHECKPOINT_LINEAGE_CROSS_EXECUTION",
                        "checkpoint parent belongs to a different execution",
                        checkpoint.execution);
                }
                if (checkpoint.lineage_depth != parent->lineage_depth + 1) {
                    add(report, "CHECKPOINT_LINEAGE_DEPTH_MISMATCH",
                        "checkpoint lineage depth does not follow its parent",
                        checkpoint.execution);
                }
                if (parent->generation >= checkpoint.generation) {
                    add(report, "CHECKPOINT_LINEAGE_NOT_BACKWARD",
                        "checkpoint parent generation is not older than the child",
                        checkpoint.execution);
                }
            }
        } else if (checkpoint.lineage_depth != 0) {
            add(report, "CHECKPOINT_LINEAGE_DEPTH_WITHOUT_PARENT",
                "checkpoint declares lineage depth but has no parent", checkpoint.execution);
        }
        if (const auto defect = binding_set_defect(checkpoint.bindings); defect.has_value()) {
            add(report, "CHECKPOINT_BINDING_SET_DEFECT", std::string(*defect),
                checkpoint.execution);
        }
    }

    // ---- continuations -----------------------------------------------------
    for (const auto& continuation : state.continuations.insertion_order()) {
        if (state.executions.find(continuation.execution) == nullptr) {
            add(report, "CONTINUATION_EXECUTION_MISSING",
                "continuation references a missing execution");
        }
        if (continuation.state == ContinuationState::Valid && continuation.checkpoint.valid()) {
            const CheckpointRecord* checkpoint = state.checkpoints.find(continuation.checkpoint);
            if (checkpoint == nullptr) {
                add(report, "VALID_CONTINUATION_MISSING_CHECKPOINT",
                    "a continuation marked VALID references a checkpoint that does not exist",
                    continuation.execution);
            } else if (!checkpoint_can_seed_continuation(checkpoint->state)) {
                add(report, "VALID_CONTINUATION_UNSEEDABLE_CHECKPOINT",
                    "a continuation marked VALID references a checkpoint that cannot seed",
                    continuation.execution);
            }
        }
    }

    // At most one continuation per execution may be VALID, and it must be the
    // execution's current continuation.
    for (const auto& execution : state.executions.insertion_order()) {
        std::size_t valid_count = 0;
        ContinuationId valid_id;
        for (const auto& continuation : state.continuations.insertion_order()) {
            if (continuation.execution != execution.id) {
                continue;
            }
            if (continuation.state != ContinuationState::Valid) {
                continue;
            }
            ++valid_count;
            valid_id = continuation.id;
        }
        if (valid_count > 1) {
            add(report, "MULTIPLE_VALID_CONTINUATIONS",
                "more than one continuation is marked VALID for one execution", execution.id);
        }
        if (valid_count == 1 && valid_id != execution.continuation) {
            add(report, "VALID_CONTINUATION_NOT_CURRENT",
                "a continuation is marked VALID but is not the execution's current continuation",
                execution.id);
        }
        // An execution that is actively exercising authority must have exactly
        // one valid continuation. Resting and recovery states may legitimately
        // hold a stale one, and a fresh execution may hold none.
        const bool exercising_authority = execution.lifecycle == Lifecycle::Running ||
                                          execution.lifecycle == Lifecycle::Checkpointing ||
                                          execution.lifecycle == Lifecycle::Draining;
        if (valid_count == 0 && exercising_authority) {
            add(report, "ACTIVE_EXECUTION_WITHOUT_VALID_CONTINUATION",
                "an execution that is exercising authority has no VALID continuation",
                execution.id);
        }
    }

    // ---- leases ------------------------------------------------------------
    for (const auto& lease : state.leases.insertion_order()) {
        if (lease.execution.is_nil()) {
            add(report, "LEASE_EXECUTION_NIL", "lease carries a nil execution id");
            continue;
        }
        const ExecutionRecord* execution = state.executions.find(lease.execution);
        if (execution == nullptr) {
            add(report, "LEASE_EXECUTION_MISSING", "lease references a missing execution");
            continue;
        }
        if (lease.epoch < state.epoch && lease.state == LeaseState::Active) {
            add(report, "STALE_LEASE_STILL_ACTIVE",
                "a lease from a previous coordinator epoch is still active", lease.execution);
        }
        if (lease.state == LeaseState::Active && execution->lease != lease.id) {
            add(report, "ORPHAN_ACTIVE_LEASE",
                "an active lease is not the execution's current lease", lease.execution);
        }
    }

    // ---- commits -----------------------------------------------------------
    std::set<std::string> commit_keys;
    for (const auto& commit : state.commits.insertion_order()) {
        if (!commit.id.valid()) {
            add(report, "COMMIT_ID_NIL", "commit record carries a nil id");
            continue;
        }
        const ActionRecord* action = state.find_action(commit.action, commit.action_generation);
        if (action == nullptr) {
            add(report, "COMMIT_ACTION_MISSING", "commit references a missing action",
                commit.execution);
            continue;
        }
        const std::string key =
            hex64(commit.action.value()) + ":" + hex64(commit.action_generation.value());
        if (!commit_keys.insert(key).second) {
            add(report, "DUPLICATE_LOGICAL_COMMIT",
                "two commit records exist for the same action identity and generation",
                commit.execution);
        }
        if (const ProgressRecord* progress = state.progress.find(commit.progress);
            progress == nullptr) {
            add(report, "COMMIT_PROGRESS_MISSING", "commit references a missing progress record",
                commit.execution);
        } else if (!progress->committed) {
            add(report, "COMMIT_PROGRESS_NOT_COMMITTED",
                "commit references a progress record that is not committed", commit.execution);
        }
    }

    // ---- replays -----------------------------------------------------------
    for (const auto& replay : state.replays.insertion_order()) {
        if (state.find_latest_action(replay.action) == nullptr) {
            add(report, "REPLAY_ACTION_MISSING", "replay references a missing action",
                replay.execution);
        }
        if (replay.new_action.valid() && state.find_latest_action(replay.new_action) == nullptr) {
            add(report, "REPLAY_TARGET_MISSING", "replay references a missing replacement action",
                replay.execution);
        }
        if (replay.status == ReplayStatus::Admitted && !replay.new_action.valid()) {
            add(report, "ADMITTED_REPLAY_WITHOUT_TARGET",
                "an admitted replay has no replacement action identity", replay.execution);
        }
    }

    // ---- ambiguities -------------------------------------------------------
    for (const auto& ambiguity : state.ambiguities.insertion_order()) {
        const ExecutionRecord* execution = state.executions.find(ambiguity.execution);
        if (execution == nullptr) {
            add(report, "AMBIGUITY_EXECUTION_MISSING",
                "ambiguity references a missing execution");
            continue;
        }
        if (state.find_action(ambiguity.action, ambiguity.action_generation) == nullptr) {
            add(report, "AMBIGUITY_ACTION_MISSING", "ambiguity references a missing action",
                ambiguity.execution);
        }
        if (ambiguity_is_open(ambiguity.state) && !execution->ambiguity.valid()) {
            add(report, "OPEN_AMBIGUITY_NOT_REFERENCED",
                "an open ambiguity is not referenced by its execution", ambiguity.execution);
        }
    }

    // ---- recoveries --------------------------------------------------------
    for (const auto& recovery : state.recoveries.insertion_order()) {
        if (state.executions.find(recovery.execution) == nullptr) {
            add(report, "RECOVERY_EXECUTION_MISSING",
                "recovery record references a missing execution");
        }
        if (recovery.decision >= kRecoveryDecisionCount) {
            add(report, "RECOVERY_DECISION_INVALID",
                "recovery record carries an unknown decision ordinal", recovery.execution);
        }
    }

    // ---- policies ----------------------------------------------------------
    for (const auto& policy : state.policies.insertion_order()) {
        if (const auto reason = validate_policy(policy); reason.has_value()) {
            add(report, "POLICY_INVALID", std::string(*reason));
        }
    }

    return report;
}

}  // namespace pef
