// Example: an interrupted replay-safe action, and one that is not.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "example_support.hpp"
#include "pef/explain.hpp"

using namespace pef;
using namespace pef::example;

namespace {

// Runs an execution up to a dispatched action with the given side-effect class
// and then fences the worker, exactly as a process death would.
Status prepare_interrupted(Runtime& runtime, const CallerContext& caller,
                           SideEffectClass cls, ExecutionId& execution, ActionId& action_id,
                           ActionGeneration& action_generation) {
    CreateExecutionRequest create;
    create.request = RequestId{1};
    create.policy = example_policy();
    CreateExecutionResult created;
    PEF_TRY(runtime.create_execution(caller, create, created));
    execution = created.execution;

    Bound bound;
    PEF_TRY(bind_worker(runtime, caller, execution, "worker-interrupted", BindingSet{}, bound));
    PEF_TRY(ensure_running(runtime, caller, bound));

    BeginActionRequest begin;
    begin.request = RequestId{500};
    begin.token = bound.token;
    begin.effect_class = cls;
    begin.evidence.kind = EvidenceKind::Synthetic;
    begin.evidence.source = "example-fault-injection";
    if (requires_request_key(cls)) {
        begin.request_key = "example-replay-key";
    }
    BeginActionResult action;
    PEF_TRY(runtime.begin_action(begin, action));
    action_id = action.action;
    action_generation = action.action_generation;

    // No durable receipt is recorded: the physical outcome is unknown.
    RecoveryOutcome recovered;
    PEF_TRY(runtime.fence(caller, RequestId{501}, execution, "simulated process death"));
    return runtime.recover(caller, RequestId{502}, execution, recovered);
}

}  // namespace

int main() {
    Scratch scratch("replay-safe");
    Runtime runtime;
    RuntimeConfig config;
    config.store_path = scratch.path();
    config.create_if_missing = true;
    OpenOutcome opened;
    if (const Status status = runtime.open(config, opened); !status.ok()) {
        return fail(status);
    }
    CallerContext caller{SessionId{}, opened.epoch};

    for (SideEffectClass cls : {SideEffectClass::Pure, SideEffectClass::Idempotent,
                                SideEffectClass::RepeatableWithKey,
                                SideEffectClass::NonRepeatable, SideEffectClass::Unknown}) {
        ExecutionId execution;
        ActionId action;
        ActionGeneration generation;
        if (const Status status =
                prepare_interrupted(runtime, caller, cls, execution, action, generation);
            !status.ok()) {
            return fail(status);
        }
        RecoveryPlan plan;
        if (const Status status = runtime.classify(caller, execution, plan); !status.ok()) {
            return fail(status);
        }
        say(std::string(side_effect_class_name(cls)) + ": " +
            std::string(recovery_decision_name(plan.decision)) +
            " automatic=" + (recovery_allows_automatic_continue(plan.decision) ? "yes" : "no"));
        say("  " + plan.explanation);
        if (plan.decision == RecoveryDecision::AmbiguousCompletion) {
            ExecutionView view;
            if (const Status status = runtime.query(execution, view); !status.ok()) {
                return fail(status);
            }
            say("  blocked above ordinal " +
                std::to_string(view.execution.blocked_above_ordinal) +
                "; automatic retry refused");
        }
    }
    return 0;
}
