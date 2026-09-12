// Example: a continuation that is not valid, and why it is refused.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "example_support.hpp"
#include "pef/explain.hpp"

using namespace pef;
using namespace pef::example;

int main() {
    Scratch scratch("stale-continuation");
    RuntimeConfig config;
    config.store_path = scratch.path();
    config.create_if_missing = true;

    ExecutionId execution;
    BindingId binding_id{0x5150};
    {
        Runtime runtime;
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
        execution = created.execution;

        BindingSet bindings;
        bindings.refs.push_back(
            BindingRef{BindingDomain::Model, binding_id, BindingGeneration{1}, "model-a"});
        bindings.canonicalize();
        Bound bound;
        if (const Status status =
                bind_worker(runtime, caller, execution, "example-worker", bindings, bound);
            !status.ok()) {
            return fail(status);
        }
        if (const Status status = ensure_running(runtime, caller, bound); !status.ok()) {
            return fail(status);
        }
        CompleteActionResult committed;
        if (const Status status = commit_one(runtime, bound, SideEffectClass::Pure, 0, committed);
            !status.ok()) {
            return fail(status);
        }
        RegisterCheckpointRequest checkpoint;
        checkpoint.request = RequestId{100};
        checkpoint.token = bound.token;
        checkpoint.expected_checkpoint_generation = CheckpointGeneration{};
        checkpoint.content_size = 512;
        checkpoint.content_hash = fnv1a("stale-example");
        RegisterCheckpointResult registered;
        if (const Status status = runtime.register_checkpoint(checkpoint, registered);
            !status.ok()) {
            return fail(status);
        }
        ExecutionView view;
        if (const Status status = runtime.query(execution, view); !status.ok()) {
            return fail(status);
        }
        ContinuationState state = ContinuationState::Invalid;
        std::string reason;
        if (const Status status = runtime.validate_continuation(caller, execution, state, reason);
            !status.ok()) {
            return fail(status);
        }
        say("before the binding moves: " + std::string(continuation_state_name(state)) + " (" +
            reason + ")");
    }

    Runtime runtime;
    OpenOutcome opened;
    if (const Status status = runtime.open(config, opened); !status.ok()) {
        return fail(status);
    }
    CallerContext caller{SessionId{}, opened.epoch};
    ExecutionView view;
    if (const Status status = runtime.query(execution, view); !status.ok()) {
        return fail(status);
    }
    ContinuationState state = ContinuationState::Invalid;
    std::string reason;
    if (const Status status = runtime.validate_continuation(caller, execution, state, reason);
        !status.ok()) {
        return fail(status);
    }
    say("after the coordinator restarted: " + std::string(continuation_state_name(state)) +
        " (" + reason + ")");
    say(explain_continuation(view, state, reason));

    // Re-establish authority with a binding generation that has moved forward.
    BindingSet moved;
    moved.refs.push_back(
        BindingRef{BindingDomain::Model, binding_id, BindingGeneration{2}, "model-a"});
    moved.canonicalize();
    Bound bound;
    if (const Status status =
            bind_worker(runtime, caller, execution, "replacement-worker", moved, bound);
        !status.ok()) {
        return fail(status);
    }
    RecoveryOutcome recovered;
    if (const Status status = runtime.recover(caller, RequestId{200}, execution, recovered);
        !status.ok()) {
        return fail(status);
    }
    say("recovery decision: " +
        std::string(recovery_decision_name(recovered.plan.decision)) + " (" +
        recovered.plan.explanation + ")");
    if (const Status status = runtime.query(execution, view); !status.ok()) {
        return fail(status);
    }
    if (const Status status = runtime.validate_continuation(caller, execution, state, reason);
        !status.ok()) {
        return fail(status);
    }
    say("after revalidation: " + std::string(continuation_state_name(state)) + " (" + reason +
        ")");
    return 0;
}
