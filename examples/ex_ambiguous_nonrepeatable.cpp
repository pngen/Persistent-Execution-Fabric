// Example: an interrupted non-repeatable action becomes explicit ambiguity, is
// resolved by evidence, and is re-driven under a new generation.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "example_support.hpp"
#include "pef/explain.hpp"

using namespace pef;
using namespace pef::example;

int main() {
    Scratch scratch("ambiguous-nonrepeatable");
    Runtime runtime;
    RuntimeConfig config;
    config.store_path = scratch.path();
    config.create_if_missing = true;
    OpenOutcome opened;
    if (const Status status = runtime.open(config, opened); !status.ok()) {
        return fail(status);
    }
    CallerContext caller{SessionId{}, opened.epoch};
    CreateExecutionRequest create;
    create.request = RequestId{1};
    create.policy = example_policy();
    CreateExecutionResult created;
    if (const Status status = runtime.create_execution(caller, create, created); !status.ok()) {
        return fail(status);
    }
    Bound bound;
    if (const Status status = bind_worker(runtime, caller, created.execution, "worker-risky",
                                          BindingSet{}, bound);
        !status.ok()) {
        return fail(status);
    }
    if (const Status status = ensure_running(runtime, caller, bound); !status.ok()) {
        return fail(status);
    }

    BeginActionRequest begin;
    begin.request = RequestId{10};
    begin.token = bound.token;
    begin.effect_class = SideEffectClass::NonRepeatable;
    begin.evidence.kind = EvidenceKind::Real;
    begin.evidence.source = "example";
    BeginActionResult action;
    if (const Status status = runtime.begin_action(begin, action); !status.ok()) {
        return fail(status);
    }
    say("dispatched NON_REPEATABLE action ordinal " + std::to_string(action.ordinal));

    // The worker dies with no durable acknowledgement.
    if (const Status status = runtime.fence(caller, RequestId{11}, created.execution,
                                            "simulated process death");
        !status.ok()) {
        return fail(status);
    }
    RecoveryOutcome recovered;
    if (const Status status = runtime.recover(caller, RequestId{12}, created.execution, recovered);
        !status.ok()) {
        return fail(status);
    }
    say("recovery decision: " +
        std::string(recovery_decision_name(recovered.plan.decision)));
    say("  " + recovered.plan.explanation);

    ExecutionView view;
    if (const Status status = runtime.query(created.execution, view); !status.ok()) {
        return fail(status);
    }
    say(explain_execution(view));

    // Attempting to complete the ambiguous action must be refused.
    CompleteActionRequest complete;
    complete.request = RequestId{13};
    complete.token = bound.token;
    complete.action = action.action;
    complete.action_generation = action.action_generation;
    CompleteActionResult outcome;
    say("completion attempt: " + runtime.complete_action(complete, outcome).to_string());

    // An operator establishes, from an external system, that no effect applied.
    ResolveAmbiguityRequest resolve;
    resolve.request = RequestId{14};
    resolve.caller = caller;
    resolve.execution = created.execution;
    resolve.ambiguity = view.execution.ambiguity;
    resolve.ambiguity_generation = view.execution.ambiguity_generation;
    resolve.resolution = AmbiguityState::ResolvedNotApplied;
    resolve.note = "external system confirmed no effect";
    resolve.evidence.kind = EvidenceKind::Synthetic;
    resolve.evidence.source = "example-reconciler";
    ResolveAmbiguityResult resolved;
    if (const Status status = runtime.resolve_ambiguity(resolve, resolved); !status.ok()) {
        return fail(status);
    }
    say("resolved as " + std::string(ambiguity_state_name(resolved.state)) +
        "; replay admitted=" + (resolved.replay_admitted ? "yes" : "no"));

    // Re-drive the same logical action under a new generation.
    Bound replacement;
    if (const Status status = bind_worker(runtime, caller, created.execution,
                                          "replacement-worker", BindingSet{}, replacement);
        !status.ok()) {
        return fail(status);
    }
    if (const Status status = ensure_running(runtime, caller, replacement); !status.ok()) {
        return fail(status);
    }
    BeginActionRequest replay_begin = begin;
    replay_begin.request = RequestId{15};
    replay_begin.token = replacement.token;
    replay_begin.request_key = "example-replay-key";
    BeginActionResult replayed;
    if (const Status status = runtime.begin_action(replay_begin, replayed); !status.ok()) {
        return fail(status);
    }
    say("re-drove logical action ordinal " + std::to_string(replayed.ordinal) +
        " generation " + std::to_string(replayed.action_generation.value()));

    CompleteActionRequest replay_complete;
    replay_complete.request = RequestId{16};
    replay_complete.token = replacement.token;
    replay_complete.action = replayed.action;
    replay_complete.action_generation = replayed.action_generation;
    replay_complete.completion.effect_applied = true;
    replay_complete.completion.evidence.kind = EvidenceKind::Real;
    CompleteActionResult committed;
    if (const Status status = runtime.complete_action(replay_complete, committed); !status.ok()) {
        return fail(status);
    }
    say("committed progress_generation " +
        std::to_string(committed.progress_generation.value()));

    AuditReport audit;
    if (const Status status = runtime.audit(audit); !status.ok()) {
        return fail(status);
    }
    say("audit violations=" + std::to_string(audit.violations()));
    return audit.clean() ? 0 : 1;
}
