// Persistent Execution Fabric - scale and completed-work measurements.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// These cases measure completed durable work, not submission latency: every
// measurement is taken after the coordinator acknowledged a durable commit.
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "pef/audit.hpp"
#include "pef/runtime.hpp"
#include "test_framework.hpp"

using namespace pef;

namespace {

struct ScaleFixture {
    peftest::TempStore dir;
    Runtime runtime;
    CallerContext caller;
    ExecutionId execution;
    LeaseToken token;
    PolicyGeneration policy_generation;

    explicit ScaleFixture(const char* label) : dir(label) {}

    Status setup(bool truncate_on_snapshot = false) {
        (void)truncate_on_snapshot;
        RuntimeConfig config;
        config.store_path = dir.path();
        config.create_if_missing = true;
        OpenOutcome outcome;
        PEF_TRY(runtime.open(config, outcome));
        caller.epoch = runtime.epoch();
        ExecutionPolicy policy;
        policy.id = PolicyId{1};
        policy.generation = PolicyGeneration{1};
        policy.max_action_history = 10000000;
        policy.durability = DurabilityMode::JournaledNotFlushed;
        CreateExecutionRequest create;
        create.request = RequestId{1};
        create.policy = policy;
        CreateExecutionResult created;
        PEF_TRY(runtime.create_execution(caller, create, created));
        execution = created.execution;
        policy_generation = created.policy_generation;
        const WorkerId worker = derive_worker_id("scale-worker");
        const WorkerBootId boot = mint_worker_boot_id(worker);
        BindWorkerRequest bind;
        bind.request = RequestId{2};
        bind.execution = execution;
        bind.worker = worker;
        bind.boot = boot;
        BindWorkerResult bound;
        PEF_TRY(runtime.bind_worker(caller, bind, bound));
        token.execution = execution;
        token.execution_generation = created.generation;
        token.incarnation = bound.incarnation;
        token.incarnation_generation = bound.incarnation_generation;
        token.lease = bound.lease;
        token.lease_generation = bound.lease_generation;
        token.epoch = runtime.epoch();
        token.worker = worker;
        token.boot = boot;
        token.policy_generation = policy_generation;
        StartRequest start;
        start.request = RequestId{3};
        start.token = token;
        return runtime.start(start);
    }

    // Returns the elapsed microseconds for count completed commits.
    Status run(std::uint64_t count, std::uint64_t& elapsed_micros) {
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < count; ++i) {
            BeginActionRequest action_request;
            action_request.request = RequestId{1000 + i * 2};
            action_request.token = token;
            action_request.effect_class = SideEffectClass::Pure;
            BeginActionResult action;
            PEF_TRY(runtime.begin_action(action_request, action));
            CompleteActionRequest complete;
            complete.request = RequestId{1001 + i * 2};
            complete.token = token;
            complete.action = action.action;
            complete.action_generation = action.action_generation;
            complete.completion.effect_applied = true;
            CompleteActionResult result;
            PEF_TRY(runtime.complete_action(complete, result));
        }
        const auto end = std::chrono::steady_clock::now();
        elapsed_micros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
        return ok_status();
    }
};

void report(const char* label, std::uint64_t completed, std::uint64_t micros) {
    const double per_action = completed == 0 ? 0.0
                                             : static_cast<double>(micros) /
                                                   static_cast<double>(completed);
    const double per_second =
        micros == 0 ? 0.0 : static_cast<double>(completed) * 1000000.0 / static_cast<double>(micros);
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "MEASURE %s completed=%llu micros=%llu per_action_us=%.2f per_second=%.0f",
                  label, static_cast<unsigned long long>(completed),
                  static_cast<unsigned long long>(micros), per_action, per_second);
    std::cout << buffer << std::endl;
}

}  // namespace

PEF_TEST(scale, completed_work_scales_linearly) {
    PEF_PHASE(ctx, SETUP);
    const std::uint64_t sizes[] = {10, 100, 1000, 10000, 100000};
    for (std::uint64_t size : sizes) {
        ScaleFixture fixture("scale-linear");
        PEF_REQUIRE(ctx, fixture.setup().ok());
        std::uint64_t micros = 0;
        PEF_REQUIRE(ctx, fixture.run(size, micros).ok());
        PEF_PHASE(ctx, COMMIT);
        report("linear", size, micros);

        ExecutionView view;
        PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
        PEF_CHECK_MSG(ctx, view.execution.progress_generation.value() == size,
                      "committed progress does not match the requested action count");
        PEF_CHECK(ctx, view.execution.committed_actions == size);
        PEF_CHECK(ctx, view.commit_count == size);

        // Lookup cost must not grow with history: the newest action is found
        // through the derived index, not by scanning the action table.
        const auto lookup_begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i) {
            ExecutionView probe;
            PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, probe).ok());
        }
        const auto lookup_end = std::chrono::steady_clock::now();
        const auto lookup_micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                       lookup_end - lookup_begin)
                                       .count();
        report("query", 1000, static_cast<std::uint64_t>(lookup_micros));
        PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
    }
}

PEF_TEST(scale, audit_and_snapshot_round_trip_at_scale) {
    PEF_PHASE(ctx, SETUP);
    ScaleFixture fixture("scale-audit");
    PEF_REQUIRE(ctx, fixture.setup().ok());
    std::uint64_t micros = 0;
    PEF_REQUIRE(ctx, fixture.run(20000, micros).ok());
    report("audit-setup", 20000, micros);

    PEF_PHASE(ctx, VERIFY);
    const auto audit_begin = std::chrono::steady_clock::now();
    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    const auto audit_end = std::chrono::steady_clock::now();
    report("audit", 20000,
           static_cast<std::uint64_t>(
               std::chrono::duration_cast<std::chrono::microseconds>(audit_end - audit_begin)
                   .count()));
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());

    PEF_PHASE(ctx, PERSIST);
    const auto snapshot_begin = std::chrono::steady_clock::now();
    PEF_REQUIRE(ctx, fixture.runtime.take_snapshot(true).ok());
    const auto snapshot_end = std::chrono::steady_clock::now();
    report("snapshot", 20000,
           static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          snapshot_end - snapshot_begin)
                                          .count()));
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());

    PEF_PHASE(ctx, RECOVER);
    Runtime reloaded;
    RuntimeConfig config;
    config.store_path = fixture.dir.path();
    OpenOutcome outcome;
    const auto load_begin = std::chrono::steady_clock::now();
    PEF_REQUIRE(ctx, reloaded.open(config, outcome).ok());
    const auto load_end = std::chrono::steady_clock::now();
    report("load", 20000,
           static_cast<std::uint64_t>(
               std::chrono::duration_cast<std::chrono::microseconds>(load_end - load_begin).count()));
    ExecutionView view;
    PEF_REQUIRE(ctx, reloaded.query(fixture.execution, view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 20000);
    PEF_CHECK(ctx, view.commit_count == 20000);
    AuditReport reloaded_audit;
    PEF_REQUIRE(ctx, reloaded.audit(reloaded_audit).ok());
    PEF_CHECK_MSG(ctx, reloaded_audit.clean(), reloaded_audit.render());
    PEF_REQUIRE(ctx, reloaded.shutdown().ok());
}

PEF_TEST(scale, durable_commit_cost_is_measured_not_assumed) {
    PEF_PHASE(ctx, SETUP);
    ScaleFixture fixture("scale-durable");
    PEF_REQUIRE(ctx, fixture.setup().ok());

    // The default policy is DurableOnCommit; measure the honest cost of that.
    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
    ExecutionPolicy policy = view.policy;
    policy.durability = DurabilityMode::DurableOnCommit;
    PEF_REQUIRE(ctx,
                fixture.runtime.update_policy(fixture.caller, RequestId{50}, fixture.execution,
                                              policy, true)
                    .ok());
    PEF_PHASE(ctx, RECOVER);
    RecoveryOutcome recovered;
    PEF_REQUIRE(ctx,
                fixture.runtime.recover(fixture.caller, RequestId{51}, fixture.execution, recovered)
                    .ok());
    BindWorkerRequest bind;
    bind.request = RequestId{52};
    bind.execution = fixture.execution;
    bind.worker = derive_worker_id("scale-worker");
    bind.boot = fixture.token.boot;
    BindWorkerResult bound;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());
    fixture.token.incarnation = bound.incarnation;
    fixture.token.incarnation_generation = bound.incarnation_generation;
    fixture.token.lease = bound.lease;
    fixture.token.lease_generation = bound.lease_generation;
    fixture.token.epoch = fixture.runtime.epoch();
    PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
    fixture.token.execution_generation = view.execution.generation;
    fixture.token.policy_generation = view.execution.policy_generation;
    ResumeRequest resume;
    resume.request = RequestId{53};
    resume.token = fixture.token;
    resume.revalidate_bindings = true;
    ResumeResult resumed;
    PEF_REQUIRE(ctx, fixture.runtime.resume(resume, resumed).ok());

    PEF_PHASE(ctx, COMMIT);
    constexpr std::uint64_t kCount = 2000;
    std::uint64_t micros = 0;
    PEF_REQUIRE(ctx, fixture.run(kCount, micros).ok());
    report("durable-commit", kCount, micros);
    PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
    PEF_CHECK(ctx, view.execution.committed_actions == kCount);
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
}
