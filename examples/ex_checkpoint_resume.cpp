// Example: checkpoint, suspend, restart the coordinator, and resume from the
// durable continuation.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "example_support.hpp"
#include "pef/explain.hpp"

using namespace pef;
using namespace pef::example;

int main() {
    Scratch scratch("checkpoint-resume");
    RuntimeConfig config;
    config.store_path = scratch.path();
    config.create_if_missing = true;

    ExecutionId execution;
    WorkerId worker;
    WorkerBootId boot;
    {
        Runtime runtime;
        OpenOutcome opened;
        if (const Status status = runtime.open(config, opened); !status.ok()) {
            return fail(status);
        }
        CallerContext caller{SessionId{}, opened.epoch};
        CreateExecutionRequest create;
        create.request = next_request();
        create.policy = example_policy();
        CreateExecutionResult created;
        if (const Status status = runtime.create_execution(caller, create, created); !status.ok()) {
            return fail(status);
        }
        execution = created.execution;
        Bound bound;
        if (const Status status =
                bind_worker(runtime, caller, execution, "example-worker", BindingSet{}, bound);
            !status.ok()) {
            return fail(status);
        }
        worker = bound.worker;
        boot = bound.boot;
        if (const Status status = ensure_running(runtime, caller, bound); !status.ok()) {
            return fail(status);
        }
        for (std::uint64_t i = 0; i < 2; ++i) {
            CompleteActionResult committed;
            if (const Status status =
                    commit_one(runtime, bound, SideEffectClass::Pure, i, committed);
                !status.ok()) {
                return fail(status);
            }
        }
        ExecutionView view;
        if (const Status status = runtime.query(execution, view); !status.ok()) {
            return fail(status);
        }
        RegisterCheckpointRequest checkpoint;
        checkpoint.request = next_request();
        checkpoint.token = bound.token;
        checkpoint.expected_checkpoint = view.execution.checkpoint;
        checkpoint.expected_checkpoint_generation = view.execution.checkpoint_generation;
        checkpoint.content_size = 8192;
        checkpoint.content_hash = fnv1a("example-checkpoint-bytes");
        checkpoint.effect_boundary_ordinal = view.execution.action_frontier;
        RegisterCheckpointResult registered;
        if (const Status status = runtime.register_checkpoint(checkpoint, registered);
            !status.ok()) {
            return fail(status);
        }
        say("checkpoint generation=" +
            std::to_string(registered.checkpoint_generation.value()) +
            " lineage_depth=" + std::to_string(registered.lineage_depth) +
            " continuation=" + hex64(registered.continuation.value()));

        ContinuationState state = ContinuationState::Invalid;
        std::string reason;
        if (const Status status = runtime.validate_continuation(caller, execution, state, reason);
            !status.ok()) {
            return fail(status);
        }
        say("continuation state=" + std::string(continuation_state_name(state)) + " reason=" +
            reason);
        if (const Status status = runtime.shutdown(); !status.ok()) {
            return fail(status);
        }
    }

    say("--- coordinator restarted against the same store ---");
    Runtime restarted;
    OpenOutcome opened;
    if (const Status status = restarted.open(config, opened); !status.ok()) {
        return fail(status);
    }
    say(explain_restart(opened));
    CallerContext caller{SessionId{}, opened.epoch};

    ExecutionView view;
    if (const Status status = restarted.query(execution, view); !status.ok()) {
        return fail(status);
    }
    say("after restart lifecycle=" + std::string(lifecycle_name(view.execution.lifecycle)) +
        " worker_bound=" + (view.execution.worker.valid() ? "yes" : "no") +
        " lease_held=" + (view.execution.lease.valid() ? "yes" : "no"));

    Bound bound;
    if (const Status status =
            bind_worker(restarted, caller, execution, "replacement-worker", BindingSet{}, bound);
        !status.ok()) {
        return fail(status);
    }
    if (const Status status = ensure_running(restarted, caller, bound); !status.ok()) {
        return fail(status);
    }
    CompleteActionResult committed;
    if (const Status status = commit_one(restarted, bound, SideEffectClass::Pure, 10, committed);
        !status.ok()) {
        return fail(status);
    }
    say("resumed and committed ordinal=" + std::to_string(committed.ordinal) +
        " progress_generation=" + std::to_string(committed.progress_generation.value()));
    say("previous boot=" + hex64(boot.value()) + " worker=" + hex64(worker.value()));
    return 0;
}
