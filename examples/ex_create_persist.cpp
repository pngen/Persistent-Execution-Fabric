// Example: create a persistent execution and commit durable progress.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "example_support.hpp"
#include "pef/audit.hpp"
#include "pef/explain.hpp"

using namespace pef;
using namespace pef::example;

int main() {
    Scratch scratch("create-persist");
    Runtime runtime;
    RuntimeConfig config;
    config.store_path = scratch.path();
    config.create_if_missing = true;
    OpenOutcome opened;
    if (const Status status = runtime.open(config, opened); !status.ok()) {
        return fail(status);
    }
    say("store epoch=" + std::to_string(opened.epoch.value()) +
        " state=" + (opened.created ? "created" : "recovered"));

    CallerContext caller{SessionId{}, opened.epoch};
    CreateExecutionRequest create;
    create.request = next_request();
    create.policy = example_policy();
    CreateExecutionResult created;
    if (const Status status = runtime.create_execution(caller, create, created); !status.ok()) {
        return fail(status);
    }
    say("created execution=" + hex64(created.execution.value()) +
        " generation=" + std::to_string(created.generation.value()));

    Bound bound;
    if (const Status status = bind_worker(runtime, caller, created.execution, "example-worker",
                                          BindingSet{}, bound);
        !status.ok()) {
        return fail(status);
    }
    if (const Status status = ensure_running(runtime, caller, bound); !status.ok()) {
        return fail(status);
    }

    for (std::uint64_t i = 0; i < 3; ++i) {
        CompleteActionResult committed;
        if (const Status status = commit_one(runtime, bound, SideEffectClass::Idempotent, i,
                                             committed);
            !status.ok()) {
            return fail(status);
        }
        say("commit ordinal=" + std::to_string(committed.ordinal) +
            " progress_generation=" + std::to_string(committed.progress_generation.value()) +
            " commit=" + hex64(committed.commit.value()));
    }

    ExecutionView view;
    if (const Status status = runtime.query(created.execution, view); !status.ok()) {
        return fail(status);
    }
    say(explain_execution(view));

    AuditReport audit;
    if (const Status status = runtime.audit(audit); !status.ok()) {
        return fail(status);
    }
    say("audit violations=" + std::to_string(audit.violations()));
    return audit.clean() ? 0 : 1;
}
