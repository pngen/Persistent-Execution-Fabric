// Example: a worker process dies, its boot is fenced, and a replacement takes
// over without inheriting the old authority.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "example_support.hpp"
#include "pef/explain.hpp"

using namespace pef;
using namespace pef::example;

int main() {
    Scratch scratch("worker-reincarnation");
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

    Bound first;
    if (const Status status = bind_worker(runtime, caller, created.execution, "worker-a",
                                          BindingSet{}, first);
        !status.ok()) {
        return fail(status);
    }
    if (const Status status = ensure_running(runtime, caller, first); !status.ok()) {
        return fail(status);
    }
    CompleteActionResult committed;
    if (const Status status = commit_one(runtime, first, SideEffectClass::Pure, 0, committed);
        !status.ok()) {
        return fail(status);
    }
    say("worker boot " + hex64(first.boot.value()) + " committed ordinal " +
        std::to_string(committed.ordinal));

    // The worker's session ends, which is exactly how the coordinator observes
    // process death over the control plane.
    SessionId session;
    CallerContext worker_caller;
    if (const Status status = runtime.begin_session(first.worker, first.boot, worker_caller,
                                                    session);
        !status.ok()) {
        return fail(status);
    }
    if (const Status status = runtime.end_session(session); !status.ok()) {
        return fail(status);
    }
    ExecutionView fenced;
    if (const Status status = runtime.query(created.execution, fenced); !status.ok()) {
        return fail(status);
    }
    say("after worker loss lifecycle=" +
        std::string(lifecycle_name(fenced.execution.lifecycle)) +
        " lease=" + (fenced.execution.lease.valid() ? "held" : "revoked"));

    // The old boot may not start new work.
    BeginActionRequest stale;
    stale.request = RequestId{50};
    stale.token = first.token;
    stale.effect_class = SideEffectClass::Pure;
    BeginActionResult stale_action;
    const Status stale_status = runtime.begin_action(stale, stale_action);
    say("old boot attempt: " + stale_status.to_string());

    // A replacement process assumes execution with a fresh boot identity.
    Bound second;
    if (const Status status = bind_worker(runtime, caller, created.execution, "worker-a-prime",
                                          BindingSet{}, second);
        !status.ok()) {
        return fail(status);
    }
    say("replacement incarnation=" + hex64(second.token.incarnation.value()) +
        " generation=" + std::to_string(second.token.incarnation_generation.value()) +
        " (advanced=" +
        (second.token.incarnation != first.token.incarnation ? "yes" : "no") + ")");
    if (const Status status = ensure_running(runtime, caller, second); !status.ok()) {
        return fail(status);
    }
    CompleteActionResult resumed;
    if (const Status status = commit_one(runtime, second, SideEffectClass::Pure, 1, resumed);
        !status.ok()) {
        return fail(status);
    }
    say("replacement committed ordinal " + std::to_string(resumed.ordinal) +
        " progress_generation " + std::to_string(resumed.progress_generation.value()));

    ExecutionView view;
    if (const Status status = runtime.query(created.execution, view); !status.ok()) {
        return fail(status);
    }
    say(explain_execution(view));
    return 0;
}
