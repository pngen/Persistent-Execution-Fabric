// Example: a coordinator restart, what changes, and what does not.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "example_support.hpp"
#include "pef/audit.hpp"
#include "pef/explain.hpp"

using namespace pef;
using namespace pef::example;

int main() {
    Scratch scratch("coordinator-restart");
    RuntimeConfig config;
    config.store_path = scratch.path();
    config.create_if_missing = true;

    ExecutionId execution;
    CoordinatorEpoch first_epoch;
    LeaseToken old_token;
    {
        Runtime runtime;
        OpenOutcome opened;
        if (const Status status = runtime.open(config, opened); !status.ok()) {
            return fail(status);
        }
        first_epoch = opened.epoch;
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
                bind_worker(runtime, caller, execution, "worker-before-restart", BindingSet{},
                            bound);
            !status.ok()) {
            return fail(status);
        }
        old_token = bound.token;
        if (const Status status = ensure_running(runtime, caller, bound); !status.ok()) {
            return fail(status);
        }
        for (std::uint64_t i = 0; i < 3; ++i) {
            CompleteActionResult committed;
            if (const Status status =
                    commit_one(runtime, bound, SideEffectClass::Pure, i, committed);
                !status.ok()) {
                return fail(status);
            }
        }
        say("before restart: epoch=" + std::to_string(first_epoch.value()) +
            " lifecycle=" + std::string(lifecycle_name(Lifecycle::Running)) +
            " progress=" + std::to_string(runtime.journal_sequence()) + " journal records");
        if (const Status status = runtime.shutdown(); !status.ok()) {
            return fail(status);
        }
    }

    Runtime restarted;
    OpenOutcome opened;
    if (const Status status = restarted.open(config, opened); !status.ok()) {
        return fail(status);
    }
    say(explain_restart(opened));
    CallerContext caller{SessionId{}, opened.epoch};

    // Pre-restart authority is refused.
    BeginActionRequest stale;
    stale.request = next_request();
    stale.token = old_token;
    stale.effect_class = SideEffectClass::Pure;
    BeginActionResult stale_action;
    say("pre-restart token: " + restarted.begin_action(stale, stale_action).to_string());

    ExecutionView view;
    if (const Status status = restarted.query(execution, view); !status.ok()) {
        return fail(status);
    }
    say("execution identity survived: " + hex64(view.execution.id.value()) +
        " progress_generation=" + std::to_string(view.execution.progress_generation.value()) +
        " lifecycle=" + std::string(lifecycle_name(view.execution.lifecycle)));
    say("process-local authority restored: worker=" +
        std::string(view.execution.worker.valid() ? "yes" : "no") +
        " lease=" + std::string(view.execution.lease.valid() ? "yes" : "no"));

    RecoveryPlan plan;
    if (const Status status = restarted.classify(caller, execution, plan); !status.ok()) {
        return fail(status);
    }
    say(explain_recovery(plan));

    AuditReport audit;
    if (const Status status = restarted.audit(audit); !status.ok()) {
        return fail(status);
    }
    say("audit violations=" + std::to_string(audit.violations()));
    return audit.clean() ? 0 : 1;
}
