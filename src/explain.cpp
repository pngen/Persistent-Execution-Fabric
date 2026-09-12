// Persistent Execution Fabric - deterministic explainability rendering.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/explain.hpp"

#include <algorithm>
#include <sstream>

namespace pef {
namespace {

[[nodiscard]] std::string hex(ExecutionId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(ActionId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(CheckpointId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(ContinuationId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(LeaseId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(CommitId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(AmbiguityId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(RecoveryId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(WorkerId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(WorkerBootId id) { return hex64(id.value()); }
[[nodiscard]] std::string hex(ExecutionIncarnationId id) { return hex64(id.value()); }
[[nodiscard]] std::string dec(Gen<ExecutionGenerationTag> gen) { return std::to_string(gen.value()); }

[[nodiscard]] const char* yes_no(bool value) noexcept { return value ? "yes" : "no"; }

// Why a checkpoint cannot currently seed a continuation. Deterministic, and
// stated in terms of durable facts rather than a generic failure.
[[nodiscard]] std::string checkpoint_resumability(const CheckpointRecord& checkpoint,
                                                  const ExecutionRecord& execution) {
    if (checkpoint.state == CheckpointState::Corrupt) {
        return "not resumable: integrity check failed";
    }
    if (checkpoint.state == CheckpointState::Unsupported) {
        return "not resumable: the checkpoint declares an unsupported feature";
    }
    if (checkpoint.state == CheckpointState::Superseded) {
        return "not resumable: superseded by a newer checkpoint and policy does not allow "
               "falling back";
    }
    if (checkpoint.state == CheckpointState::RevalidationRequired) {
        return "not resumable: a referenced generation changed and revalidation is required";
    }
    if (checkpoint.state == CheckpointState::Stale) {
        return "not resumable: a newer checkpoint exists and is preferred";
    }
    if (checkpoint.state == CheckpointState::Created) {
        return "not resumable: registered but not yet verified";
    }
    if (checkpoint.execution != execution.id) {
        return "not resumable: the checkpoint belongs to a different execution";
    }
    if (checkpoint.content_hash == 0) {
        return "not resumable: the checkpoint carries no content integrity digest";
    }
    if (checkpoint.execution_generation != execution.generation) {
        return "not resumable: created for a previous execution generation";
    }
    if (checkpoint.policy_generation != execution.policy_generation) {
        return "not resumable: created under a policy generation that has been replaced";
    }
    return "resumable: generations are current and integrity metadata is present";
}

}  // namespace

std::string evidence_label(const Evidence& evidence) {
    std::string out(evidence_kind_name(evidence.kind));
    if (!evidence.source.empty()) {
        out += " source=";
        out += evidence.source;
    }
    if (evidence.present()) {
        out += " observed_sequence=";
        out += std::to_string(evidence.observed_sequence);
    }
    return out;
}

std::string explain_execution(const ExecutionView& view) {
    if (!view.found) {
        return "execution: not present in the durable store\n";
    }
    const ExecutionRecord& execution = view.execution;
    std::ostringstream out;
    out << "execution " << hex(execution.id) << "\n";
    out << "  lifecycle                : " << lifecycle_name(execution.lifecycle) << "\n";
    out << "  execution generation     : " << execution.generation.value() << "\n";
    out << "  incarnation              : " << hex(execution.incarnation) << " generation "
        << execution.incarnation_generation.value() << "\n";
    out << "  coordinator epoch        : " << execution.epoch.value() << "\n";
    out << "  policy                   : " << hex64(execution.policy.value()) << " generation "
        << execution.policy_generation.value() << "\n";
    out << "  authoritative progress   : generation " << execution.progress_generation.value()
        << ", committed actions " << execution.committed_actions << "\n";
    out << "  action frontier          : ordinal " << execution.action_frontier << "\n";
    out << "  last logical commit      : " << hex(execution.last_commit) << "\n";
    out << "  worker binding           : worker " << hex(execution.worker) << " boot "
        << hex(execution.boot) << "\n";
    out << "  lease                    : " << hex(execution.lease) << " generation "
        << execution.lease_generation.value();
    if (view.lease.id.valid()) {
        out << " state " << lease_state_name(view.lease.state);
        out << " epoch " << view.lease.epoch.value();
    }
    out << "\n";
    out << "  replay records           : " << execution.replay_count << "\n";
    out << "  ambiguity block          : " << (execution.blocked ? "engaged" : "none");
    if (execution.blocked) {
        out << " (progress above ordinal " << execution.blocked_above_ordinal
            << " is not permitted)";
    }
    out << "\n";

    if (!execution.bindings.refs.empty()) {
        out << "  bindings:\n";
        for (const BindingRef& ref : execution.bindings.refs) {
            out << "    " << binding_domain_name(ref.domain) << " " << hex64(ref.id.value())
                << " generation " << ref.generation.value();
            if (!ref.label.empty()) {
                out << " (" << ref.label << ")";
            }
            out << "\n";
        }
    }

    if (view.execution.continuation.valid()) {
        out << "  continuation             : " << hex(view.continuation.id) << " generation "
            << view.continuation.generation.value() << " state "
            << continuation_state_name(view.continuation.state) << "\n";
        out << "    resume ordinal         : " << view.continuation.resume_ordinal << "\n";
        out << "    bound checkpoint       : " << hex(view.continuation.checkpoint) << "\n";
        out << "    bound policy generation: " << view.continuation.policy_generation.value()
            << "\n";
        out << "    bound epoch            : " << view.continuation.epoch.value() << "\n";
    } else {
        out << "  continuation             : none\n";
    }

    if (view.execution.checkpoint.valid()) {
        out << "  current checkpoint       : " << hex(view.checkpoint.id) << " generation "
            << view.checkpoint.generation.value() << " state "
            << checkpoint_state_name(view.checkpoint.state) << "\n";
        out << "    " << checkpoint_resumability(view.checkpoint, execution) << "\n";
    } else {
        out << "  current checkpoint       : none\n";
    }
    if (view.checkpoints.size() > 1) {
        out << "  checkpoint lineage       :\n";
        for (const CheckpointRecord& checkpoint : view.checkpoints) {
            out << "    " << hex(checkpoint.id) << " generation "
                << checkpoint.generation.value() << " depth " << checkpoint.lineage_depth
                << " parent " << hex(checkpoint.parent) << " state "
                << checkpoint_state_name(checkpoint.state) << "\n";
        }
    }

    if (view.execution.ambiguity.valid()) {
        out << "  ambiguity                : " << hex(view.ambiguity.id) << " state "
            << ambiguity_state_name(view.ambiguity.state) << "\n";
        out << "    action                 : " << hex(view.ambiguity.action) << " generation "
            << view.ambiguity.action_generation.value() << " ordinal "
            << view.ambiguity.action_ordinal << " class "
            << side_effect_class_name(view.ambiguity.effect_class) << "\n";
        out << "    reason                 : " << view.ambiguity.reason << "\n";
        if (!view.ambiguity.resolution_note.empty()) {
            out << "    resolution note        : " << view.ambiguity.resolution_note << "\n";
        }
        out << "    resolution evidence    : "
            << evidence_label(view.ambiguity.resolution_evidence) << "\n";
    }

    if (view.execution.recovery.valid()) {
        const auto decision = static_cast<RecoveryDecision>(view.recovery.decision);
        out << "  last recovery            : " << hex(view.recovery.id) << " decision "
            << recovery_decision_name(decision) << "\n";
        out << "    explanation            : " << view.recovery.explanation << "\n";
    }

    if (!view.actions.empty()) {
        out << "  actions (newest first):\n";
        for (const ActionRecord& action : view.actions) {
            out << "    ordinal " << action.sequence << " action " << hex(action.id)
                << " generation " << action.generation.value() << " class "
                << side_effect_class_name(action.effect_class) << " status "
                << action_status_name(action.status);
            if (action.commit.valid()) {
                out << " commit " << hex(action.commit);
            }
            if (requires_request_key(action.effect_class)) {
                out << " request_key=" << action.request_key;
            }
            out << "\n";
            out << "      replay safety        : "
                << (auto_replayable(action.effect_class)
                        ? "automatic replay permitted for this class"
                        : (action.sealed() ? "physically sealed; automatic retry refused"
                                           : "automatic retry refused without a durable receipt"))
                << "\n";
            if (action.receipt.present()) {
                out << "      durable receipt      : " << evidence_label(action.receipt) << "\n";
            }
            if (action.replayed_from.valid()) {
                out << "      replay of            : " << hex(action.replayed_from) << " generation "
                    << action.replayed_from_generation.value() << " replay generation "
                    << action.replay_generation.value() << "\n";
            }
        }
    }
    out << "  durable commits          : " << view.commit_count << "\n";
    return out.str();
}

std::string render_execution_table(const std::vector<ExecutionRecord>& executions) {
    std::vector<const ExecutionRecord*> ordered;
    ordered.reserve(executions.size());
    for (const auto& execution : executions) {
        ordered.push_back(&execution);
    }
    std::sort(ordered.begin(), ordered.end(), [](const ExecutionRecord* a,
                                                 const ExecutionRecord* b) { return a->id < b->id; });
    std::ostringstream out;
    out << "execution         generation  lifecycle          progress  actions  committed  "
           "epoch  worker-boot\n";
    for (const ExecutionRecord* execution : ordered) {
        out << hex(execution->id) << "  " << execution->generation.value() << "           "
            << lifecycle_name(execution->lifecycle);
        const std::string lifecycle_text(lifecycle_name(execution->lifecycle));
        for (std::size_t i = lifecycle_text.size(); i < 17; ++i) {
            out << ' ';
        }
        out << execution->progress_generation.value() << "         " << execution->action_frontier
            << "        " << execution->committed_actions << "          "
            << execution->epoch.value() << "      " << hex(execution->boot) << "\n";
    }
    return out.str();
}

std::string explain_continuation(const ExecutionView& view, ContinuationState state,
                                 const std::string& reason) {
    std::ostringstream out;
    out << "continuation state: " << continuation_state_name(state) << "\n";
    out << "  reason: " << reason << "\n";
    if (!view.found) {
        return out.str();
    }
    out << "  execution generation : " << view.execution.generation.value() << "\n";
    out << "  incarnation          : " << hex(view.execution.incarnation) << " generation "
        << view.execution.incarnation_generation.value() << "\n";
    out << "  policy generation    : " << view.execution.policy_generation.value() << "\n";
    out << "  progress generation  : " << view.execution.progress_generation.value() << "\n";
    if (view.execution.continuation.valid()) {
        out << "  continuation held    : " << hex(view.continuation.id) << " generation "
            << view.continuation.generation.value() << " epoch "
            << view.continuation.epoch.value() << "\n";
        if (view.continuation.bindings.refs.size() != view.execution.bindings.refs.size()) {
            out << "  binding set size     : continuation "
                << view.continuation.bindings.refs.size() << " vs durable "
                << view.execution.bindings.refs.size() << "\n";
        }
        for (const BindingRef& ref : view.execution.bindings.refs) {
            const BindingRef* bound = view.continuation.bindings.find(ref.id);
            if (bound == nullptr) {
                out << "  binding not bound    : " << hex64(ref.id.value()) << " ("
                    << binding_domain_name(ref.domain) << ")\n";
            } else if (bound->generation != ref.generation) {
                out << "  binding moved        : " << hex64(ref.id.value()) << " continuation "
                    << bound->generation.value() << " -> durable " << ref.generation.value()
                    << "\n";
            }
        }
    }
    return out.str();
}

std::string explain_recovery(const RecoveryPlan& plan) {
    std::ostringstream out;
    out << "recovery decision: " << recovery_decision_name(plan.decision) << "\n";
    out << "  execution            : " << hex(plan.execution) << " generation "
        << plan.execution_generation.value() << "\n";
    out << "  coordinator epoch    : " << plan.epoch.value() << "\n";
    if (plan.action.valid()) {
        out << "  interrupted action   : " << hex(plan.action) << " generation "
            << plan.action_generation.value() << "\n";
    }
    if (plan.checkpoint.valid()) {
        out << "  continuation from    : checkpoint " << hex(plan.checkpoint) << " generation "
            << plan.checkpoint_generation.value() << "\n";
    }
    out << "  commit from receipt  : " << yes_no(plan.commit_from_receipt) << "\n";
    out << "  automatic continue   : "
        << yes_no(recovery_allows_automatic_continue(plan.decision)) << "\n";
    out << "  operator required    : " << yes_no(recovery_requires_operator(plan.decision))
        << "\n";
    out << "  why                  : " << plan.explanation << "\n";
    return out.str();
}

std::string explain_checkpoint(const CheckpointRecord& checkpoint,
                               const ExecutionRecord& execution) {
    std::ostringstream out;
    out << "checkpoint " << hex(checkpoint.id) << "\n";
    out << "  generation         : " << checkpoint.generation.value() << "\n";
    out << "  state              : " << checkpoint_state_name(checkpoint.state) << "\n";
    out << "  lineage depth      : " << checkpoint.lineage_depth << " parent "
        << hex(checkpoint.parent) << "\n";
    out << "  bound progress     : generation " << checkpoint.progress_generation.value() << "\n";
    out << "  bound policy       : generation " << checkpoint.policy_generation.value() << "\n";
    out << "  bound epoch        : " << checkpoint.epoch.value() << "\n";
    out << "  effect boundary    : ordinal " << checkpoint.effect_boundary_ordinal << "\n";
    out << "  content            : " << checkpoint.content_size << " bytes digest "
        << hex64(checkpoint.content_hash) << "\n";
    out << "  creation authority : worker " << hex(checkpoint.worker) << " boot "
        << hex(checkpoint.boot) << "\n";
    out << "  resumability       : " << checkpoint_resumability(checkpoint, execution) << "\n";
    return out.str();
}

std::string explain_audit(const AuditReport& report) { return report.render(); }

std::string explain_restart(const OpenOutcome& outcome) {
    std::ostringstream out;
    out << "coordinator restart\n";
    out << "  previous epoch            : " << outcome.previous_epoch.value() << "\n";
    out << "  current epoch             : " << outcome.epoch.value() << "\n";
    out << "  store                     : "
        << (outcome.created ? "created" : "loaded from durable state") << "\n";
    out << "  snapshot present          : " << yes_no(outcome.load.snapshot_present) << "\n";
    out << "  journal present           : " << yes_no(outcome.load.journal_present) << "\n";
    out << "  journal records applied   : " << outcome.load.records_loaded << "\n";
    out << "  journal records skipped   : " << outcome.load.records_skipped << "\n";
    out << "  torn journal tail         : " << yes_no(outcome.load.truncated_tail);
    if (outcome.load.truncated_tail) {
        out << " (bytes ignored " << outcome.load.bytes_ignored << ", offset "
            << outcome.load.corrupt_offset << ")";
    }
    out << "\n";
    out << "  leases revoked            : " << outcome.leases_revoked << "\n";
    out << "  executions reclassified   : " << outcome.executions_reclassified << "\n";
    out << "  process-local sessions    : invalidated (never persisted)\n";
    if (!outcome.detail.empty()) {
        out << "  recovery classification   : " << outcome.detail << "\n";
    }
    return out.str();
}

}  // namespace pef
