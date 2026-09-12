// Example: the shape an independent downstream consumer uses.
//
// This file consumes only the installed headers and the exported target. The
// repository's own consumer project (created outside the source tree during
// release validation) compiles this source against an installed prefix.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include <pef/audit.hpp>
#include <pef/explain.hpp>
#include <pef/runtime.hpp>
#include <pef/version.hpp>

int main() {
    using namespace pef;
    std::cout << "consuming " << build_info_string() << std::endl;

    std::error_code ec;
    const auto directory =
        std::filesystem::temp_directory_path(ec) / "pef-consumer-store";
    std::filesystem::remove_all(directory, ec);

    Runtime runtime;
    RuntimeConfig config;
    config.store_path = directory;
    config.create_if_missing = true;
    OpenOutcome opened;
    if (const Status status = runtime.open(config, opened); !status.ok()) {
        std::cerr << status.to_string() << std::endl;
        return 1;
    }
    CallerContext caller{SessionId{}, opened.epoch};

    ExecutionPolicy policy;
    policy.id = PolicyId{1};
    policy.generation = PolicyGeneration{1};
    CreateExecutionRequest create;
    create.request = RequestId{1};
    create.policy = policy;
    CreateExecutionResult created;
    if (const Status status = runtime.create_execution(caller, create, created); !status.ok()) {
        std::cerr << status.to_string() << std::endl;
        return 1;
    }

    const WorkerId worker = derive_worker_id("consumer-worker");
    const WorkerBootId boot = mint_worker_boot_id(worker);
    BindWorkerRequest bind;
    bind.request = RequestId{2};
    bind.execution = created.execution;
    bind.worker = worker;
    bind.boot = boot;
    BindWorkerResult bound;
    if (const Status status = runtime.bind_worker(caller, bind, bound); !status.ok()) {
        std::cerr << status.to_string() << std::endl;
        return 1;
    }
    LeaseToken token;
    token.execution = created.execution;
    token.execution_generation = created.generation;
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = runtime.epoch();
    token.worker = worker;
    token.boot = boot;
    token.policy_generation = created.policy_generation;

    StartRequest start;
    start.request = RequestId{3};
    start.token = token;
    if (const Status status = runtime.start(start); !status.ok()) {
        std::cerr << status.to_string() << std::endl;
        return 1;
    }

    BeginActionRequest begin;
    begin.request = RequestId{4};
    begin.token = token;
    begin.effect_class = SideEffectClass::Idempotent;
    BeginActionResult action;
    if (const Status status = runtime.begin_action(begin, action); !status.ok()) {
        std::cerr << status.to_string() << std::endl;
        return 1;
    }
    CompleteActionRequest complete;
    complete.request = RequestId{5};
    complete.token = token;
    complete.action = action.action;
    complete.action_generation = action.action_generation;
    complete.completion.effect_applied = true;
    CompleteActionResult committed;
    if (const Status status = runtime.complete_action(complete, committed); !status.ok()) {
        std::cerr << status.to_string() << std::endl;
        return 1;
    }
    std::cout << "committed ordinal=" << committed.ordinal
              << " progress_generation=" << committed.progress_generation.value() << std::endl;

    ContinuationState state = ContinuationState::Invalid;
    std::string reason;
    if (const Status status = runtime.validate_continuation(caller, created.execution, state, reason);
        !status.ok()) {
        std::cerr << status.to_string() << std::endl;
        return 1;
    }
    std::cout << "continuation " << continuation_state_name(state) << ": " << reason << std::endl;

    AuditReport audit;
    if (const Status status = runtime.audit(audit); !status.ok()) {
        std::cerr << status.to_string() << std::endl;
        return 1;
    }
    std::cout << "audit violations=" << audit.violations() << std::endl;
    if (const Status status = runtime.shutdown(); !status.ok()) {
        std::cerr << status.to_string() << std::endl;
        return 1;
    }
    std::filesystem::remove_all(directory, ec);
    return audit.clean() ? 0 : 1;
}
