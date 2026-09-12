// Persistent Execution Fabric - concurrency and race tests.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "pef/audit.hpp"
#include "pef/runtime.hpp"
#include "test_framework.hpp"

using namespace pef;

namespace {

struct Fixture {
    peftest::TempStore dir;
    Runtime runtime;
    CallerContext caller;
    ExecutionId execution;
    WorkerId worker;
    WorkerBootId boot;
    LeaseToken token;

    explicit Fixture(const char* label) : dir(label) {}

    Status setup() {
        RuntimeConfig config;
        config.store_path = dir.path();
        config.create_if_missing = true;
        OpenOutcome outcome;
        PEF_TRY(runtime.open(config, outcome));
        caller.epoch = runtime.epoch();
        ExecutionPolicy policy;
        policy.id = PolicyId{1};
        policy.generation = PolicyGeneration{1};
        CreateExecutionRequest create;
        create.request = RequestId{1};
        create.policy = policy;
        CreateExecutionResult created;
        PEF_TRY(runtime.create_execution(caller, create, created));
        execution = created.execution;

        worker = derive_worker_id("concurrent-worker");
        boot = mint_worker_boot_id(worker);
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
        token.policy_generation = created.policy_generation;
        StartRequest start;
        start.request = RequestId{3};
        start.token = token;
        return runtime.start(start);
    }

    [[nodiscard]] Status clear_ambiguity() {
        ExecutionView view;
        PEF_TRY(runtime.query(execution, view));
        if (!view.execution.ambiguity.valid()) {
            return ok_status();
        }
        ResolveAmbiguityRequest request;
        request.request = RequestId{0xF0000 + view.execution.ambiguity.value()};
        request.caller = caller;
        request.execution = execution;
        request.ambiguity = view.execution.ambiguity;
        request.ambiguity_generation = view.execution.ambiguity_generation;
        request.resolution = AmbiguityState::Abandoned;
        request.note = "concurrency test cleanup";
        ResolveAmbiguityResult resolved;
        return runtime.resolve_ambiguity(request, resolved);
    }
};

}  // namespace

PEF_TEST(concurrency, two_completions_racing_for_one_action_commit_once) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("conc-race");
    PEF_REQUIRE(ctx, fixture.setup().ok());

    PEF_PHASE(ctx, ACTION);
    BeginActionRequest begin;
    begin.request = RequestId{10};
    begin.token = fixture.token;
    begin.effect_class = SideEffectClass::Idempotent;
    BeginActionResult action;
    PEF_REQUIRE(ctx, fixture.runtime.begin_action(begin, action).ok());

    PEF_PHASE(ctx, COMMIT);
    constexpr int kRacers = 8;
    std::atomic<int> committed{0};
    std::atomic<int> duplicates{0};
    std::atomic<int> refusals{0};
    std::vector<std::thread> threads;
    std::vector<CommitId> observed_ids(kRacers);
    threads.reserve(kRacers);
    for (int i = 0; i < kRacers; ++i) {
        threads.emplace_back([&, i]() {
            CompleteActionRequest complete;
            complete.request = RequestId{100 + static_cast<std::uint64_t>(i)};
            complete.token = fixture.token;
            complete.action = action.action;
            complete.action_generation = action.action_generation;
            complete.completion.effect_applied = true;
            CompleteActionResult result;
            const Status status = fixture.runtime.complete_action(complete, result);
            if (!status.ok()) {
                refusals.fetch_add(1);
                return;
            }
            observed_ids[static_cast<std::size_t>(i)] = result.commit;
            if (result.duplicate) {
                duplicates.fetch_add(1);
            } else {
                committed.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK_MSG(ctx, committed.load() == 1,
                  "exactly one racer must perform the logical commit, saw " +
                      std::to_string(committed.load()));
    for (int i = 0; i < kRacers; ++i) {
        if (observed_ids[static_cast<std::size_t>(i)].valid()) {
            PEF_CHECK_MSG(ctx, observed_ids[static_cast<std::size_t>(i)] == observed_ids[0],
                          "racers observed different commit identities");
        }
    }
    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 1);
    PEF_CHECK(ctx, view.commit_count == 1);

    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
}

PEF_TEST(concurrency, distinct_actions_commit_concurrently_without_loss) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("conc-distinct");
    PEF_REQUIRE(ctx, fixture.setup().ok());

    PEF_PHASE(ctx, ACTION);
    constexpr int kWorkers = 16;
    std::atomic<int> committed{0};
    std::vector<std::thread> threads;
    threads.reserve(kWorkers);
    for (int i = 0; i < kWorkers; ++i) {
        threads.emplace_back([&, i]() {
            BeginActionRequest begin;
            begin.request = RequestId{2000 + static_cast<std::uint64_t>(i) * 2};
            begin.token = fixture.token;
            begin.effect_class = SideEffectClass::Pure;
            BeginActionResult action;
            if (!fixture.runtime.begin_action(begin, action).ok()) {
                return;
            }
            CompleteActionRequest complete;
            complete.request = RequestId{2001 + static_cast<std::uint64_t>(i) * 2};
            complete.token = fixture.token;
            complete.action = action.action;
            complete.action_generation = action.action_generation;
            complete.completion.effect_applied = true;
            CompleteActionResult result;
            if (fixture.runtime.complete_action(complete, result).ok()) {
                committed.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, committed.load() == kWorkers);
    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == kWorkers);
    PEF_CHECK(ctx, view.execution.committed_actions == kWorkers);
    PEF_CHECK(ctx, view.commit_count == kWorkers);
    PEF_CHECK(ctx, view.execution.action_frontier == kWorkers);

    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
}

PEF_TEST(concurrency, checkpoint_and_progress_racing_leave_valid_lineage) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("conc-checkpoint");
    PEF_REQUIRE(ctx, fixture.setup().ok());

    PEF_PHASE(ctx, ACTION);
    constexpr int kSteps = 24;
    std::atomic<bool> stop{false};
    std::atomic<int> checkpoints{0};
    std::thread checkpoint_thread([&]() {
        for (int attempt = 0; attempt < 40 && !stop.load(); ++attempt) {
            ExecutionView view;
            if (!fixture.runtime.query(fixture.execution, view).ok()) {
                break;
            }
            RegisterCheckpointRequest request;
            request.request = RequestId{5000 + static_cast<std::uint64_t>(attempt)};
            request.token = fixture.token;
            request.expected_checkpoint = view.execution.checkpoint;
            request.expected_checkpoint_generation = view.execution.checkpoint_generation;
            request.content_size = 256;
            request.content_hash = 0x1000 + static_cast<std::uint64_t>(attempt);
            request.effect_boundary_ordinal = view.execution.action_frontier;
            RegisterCheckpointResult registered;
            if (fixture.runtime.register_checkpoint(request, registered).ok()) {
                checkpoints.fetch_add(1);
            }
            std::this_thread::yield();
        }
    });

    for (int i = 0; i < kSteps; ++i) {
        BeginActionRequest begin;
        begin.request = RequestId{6000 + static_cast<std::uint64_t>(i) * 2};
        begin.token = fixture.token;
        begin.effect_class = SideEffectClass::Pure;
        BeginActionResult action;
        if (!fixture.runtime.begin_action(begin, action).ok()) {
            continue;
        }
        CompleteActionRequest complete;
        complete.request = RequestId{6001 + static_cast<std::uint64_t>(i) * 2};
        complete.token = fixture.token;
        complete.action = action.action;
        complete.action_generation = action.action_generation;
        complete.completion.effect_applied = true;
        CompleteActionResult result;
        (void)fixture.runtime.complete_action(complete, result);
    }
    stop.store(true);
    checkpoint_thread.join();

    PEF_PHASE(ctx, VERIFY);
    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == kSteps);
    PEF_CHECK(ctx, view.commit_count == kSteps);
    PEF_CHECK(ctx, !view.checkpoints.empty());
    for (const auto& checkpoint : view.checkpoints) {
        if (checkpoint.parent.valid()) {
            PEF_CHECK_MSG(ctx, checkpoint.lineage_depth >= 1,
                          "a checkpoint with a parent must declare lineage depth");
        }
    }
    ContinuationState state = ContinuationState::Invalid;
    std::string reason;
    PEF_REQUIRE(ctx,
                fixture.runtime
                    .validate_continuation(fixture.caller, fixture.execution, state, reason)
                    .ok());
    PEF_CHECK_MSG(ctx, state == ContinuationState::Valid, reason);

    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
}

PEF_TEST(concurrency, fence_racing_completion_leaves_no_authority) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("conc-fence");
    PEF_REQUIRE(ctx, fixture.setup().ok());

    PEF_PHASE(ctx, ACTION);
    constexpr int kRounds = 30;
    for (int round = 0; round < kRounds; ++round) {
        // Re-establish authority for each round: fence, rebind, resume.
        if (round > 0) {
            // The previous round may already have fenced the execution; a
            // second fence of an already-fenced execution is not a legal
            // transition, and that refusal is itself part of the contract.
            (void)fixture.runtime.fence(fixture.caller,
                                        RequestId{static_cast<std::uint64_t>(7000 + round * 10)},
                                        fixture.execution, "race");
            BindWorkerRequest bind;
            bind.request = RequestId{static_cast<std::uint64_t>(7001 + round * 10)};
            bind.execution = fixture.execution;
            bind.worker = fixture.worker;
            bind.boot = fixture.boot;
            BindWorkerResult bound;
            PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());
            fixture.token.incarnation = bound.incarnation;
            fixture.token.incarnation_generation = bound.incarnation_generation;
            fixture.token.lease = bound.lease;
            fixture.token.lease_generation = bound.lease_generation;
            fixture.token.epoch = fixture.runtime.epoch();
            ExecutionView view;
            PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
            fixture.token.execution_generation = view.execution.generation;
            fixture.token.policy_generation = view.execution.policy_generation;
            RecoveryOutcome recovered;
            PEF_REQUIRE(ctx, fixture.runtime
                                 .recover(fixture.caller, RequestId{static_cast<std::uint64_t>(7002 + round * 10)},
                                          fixture.execution, recovered)
                                 .ok());
            ResumeRequest resume;
            resume.request = RequestId{static_cast<std::uint64_t>(7003 + round * 10)};
            resume.token = fixture.token;
            resume.revalidate_bindings = true;
            ResumeResult resumed;
            PEF_REQUIRE(ctx, fixture.runtime.resume(resume, resumed).ok());
        }

        BeginActionRequest begin;
        begin.request = RequestId{8000 + static_cast<std::uint64_t>(round) * 2};
        begin.token = fixture.token;
        begin.effect_class = SideEffectClass::Idempotent;
        BeginActionResult action;
        PEF_REQUIRE(ctx, fixture.runtime.begin_action(begin, action).ok());

        // The fence and the completion are submitted concurrently.
        std::atomic<bool> completion_ok{false};
        std::thread completer([&]() {
            CompleteActionRequest complete;
            complete.request = RequestId{8001 + static_cast<std::uint64_t>(round) * 2};
            complete.token = fixture.token;
            complete.action = action.action;
            complete.action_generation = action.action_generation;
            complete.completion.effect_applied = true;
            CompleteActionResult result;
            completion_ok.store(fixture.runtime.complete_action(complete, result).ok());
        });
        std::thread fencer([&]() {
            (void)fixture.runtime.fence(fixture.caller,
                                        RequestId{9000 + static_cast<std::uint64_t>(round)},
                                        fixture.execution, "race");
        });
        completer.join();
        fencer.join();

        ExecutionView view;
        PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
        PEF_CHECK_MSG(ctx, !view.execution.lease.valid() ||
                               view.lease.state == LeaseState::Active,
                      "an execution references a lease that is not active");
        PEF_CHECK(ctx, view.execution.lifecycle != Lifecycle::Running ||
                           view.execution.lease.valid());
    }

    PEF_PHASE(ctx, VERIFY);
    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
}

PEF_TEST(concurrency, snapshot_under_mutation_is_consistent) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("conc-snapshot");
    PEF_REQUIRE(ctx, fixture.setup().ok());

    PEF_PHASE(ctx, ACTION);
    std::atomic<bool> stop{false};
    std::atomic<int> snapshots{0};
    std::thread snapshotter([&]() {
        for (int i = 0; i < 12 && !stop.load(); ++i) {
            if (fixture.runtime.take_snapshot(false).ok()) {
                snapshots.fetch_add(1);
            }
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 64; ++i) {
        BeginActionRequest begin;
        begin.request = RequestId{10000 + static_cast<std::uint64_t>(i) * 2};
        begin.token = fixture.token;
        begin.effect_class = SideEffectClass::Pure;
        BeginActionResult action;
        if (!fixture.runtime.begin_action(begin, action).ok()) {
            continue;
        }
        CompleteActionRequest complete;
        complete.request = RequestId{10001 + static_cast<std::uint64_t>(i) * 2};
        complete.token = fixture.token;
        complete.action = action.action;
        complete.action_generation = action.action_generation;
        complete.completion.effect_applied = true;
        CompleteActionResult result;
        (void)fixture.runtime.complete_action(complete, result);
    }
    stop.store(true);
    snapshotter.join();

    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, snapshots.load() > 0);
    PEF_REQUIRE(ctx, fixture.runtime.flush().ok());

    // The snapshot written mid-stream must still load into a consistent state.
    Runtime reloaded;
    RuntimeConfig config;
    config.store_path = fixture.dir.path();
    OpenOutcome outcome;
    PEF_REQUIRE(ctx, reloaded.open(config, outcome).ok());
    ExecutionView view;
    PEF_REQUIRE(ctx, reloaded.query(fixture.execution, view).ok());
    PEF_CHECK(ctx, view.execution.id == fixture.execution);
    AuditReport audit;
    PEF_REQUIRE(ctx, reloaded.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, reloaded.shutdown().ok());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
}

PEF_TEST(concurrency, repeated_recovery_converges) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("conc-recovery");
    PEF_REQUIRE(ctx, fixture.setup().ok());

    PEF_PHASE(ctx, ACTION);
    constexpr int kThreads = 6;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            for (int round = 0; round < 8; ++round) {
                RecoveryOutcome outcome;
                const Status status = fixture.runtime.recover(
                    fixture.caller, RequestId{20000 + static_cast<std::uint64_t>(i) * 100 +
                                              static_cast<std::uint64_t>(round)},
                    fixture.execution, outcome);
                (void)status;
                RecoveryPlan plan;
                (void)fixture.runtime.classify(fixture.caller, fixture.execution, plan);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    PEF_PHASE(ctx, VERIFY);
    PEF_REQUIRE(ctx, fixture.clear_ambiguity().ok());
    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(fixture.execution, view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 0);
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
}

PEF_TEST(concurrency, concurrent_creation_never_reuses_an_identity) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("conc-create");
    PEF_REQUIRE(ctx, fixture.setup().ok());

    PEF_PHASE(ctx, CREATE);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    std::mutex collected_mutex;
    std::vector<ExecutionId> collected;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < kPerThread; ++j) {
                CreateExecutionRequest create;
                create.request = RequestId{30000 + static_cast<std::uint64_t>(i) * 1000 +
                                           static_cast<std::uint64_t>(j)};
                create.policy.id = PolicyId{1};
                create.policy.generation = PolicyGeneration{1};
                CreateExecutionResult created;
                if (!fixture.runtime.create_execution(fixture.caller, create, created).ok()) {
                    continue;
                }
                std::lock_guard<std::mutex> guard(collected_mutex);
                collected.push_back(created.execution);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, collected.size() == static_cast<std::size_t>(kThreads * kPerThread));
    std::sort(collected.begin(), collected.end());
    PEF_CHECK_MSG(ctx, std::adjacent_find(collected.begin(), collected.end()) == collected.end(),
                  "two executions received the same identity");

    static std::mutex unused;
    (void)unused;
    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
}

PEF_TEST(concurrency, shutdown_racing_operations_is_idempotent) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("conc-shutdown");
    PEF_REQUIRE(ctx, fixture.setup().ok());

    PEF_PHASE(ctx, ACTION);
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i]() {
            while (!stop.load()) {
                BeginActionRequest begin;
                begin.request = RequestId{40000 + static_cast<std::uint64_t>(i) * 1000 +
                                          fixture.runtime.journal_sequence()};
                begin.token = fixture.token;
                begin.effect_class = SideEffectClass::Pure;
                BeginActionResult action;
                if (!fixture.runtime.begin_action(begin, action).ok()) {
                    continue;
                }
                CompleteActionRequest complete;
                complete.request = RequestId{begin.request.value() ^ 0x5555ULL};
                complete.token = fixture.token;
                complete.action = action.action;
                complete.action_generation = action.action_generation;
                complete.completion.effect_applied = true;
                CompleteActionResult result;
                (void)fixture.runtime.complete_action(complete, result);
            }
        });
    }

    PEF_PHASE(ctx, SHUTDOWN);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
    stop.store(true);
    for (auto& thread : threads) {
        thread.join();
    }

    PEF_PHASE(ctx, VERIFY);
    Runtime reloaded;
    RuntimeConfig config;
    config.store_path = fixture.dir.path();
    OpenOutcome outcome;
    PEF_REQUIRE(ctx, reloaded.open(config, outcome).ok());
    AuditReport audit;
    PEF_REQUIRE(ctx, reloaded.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, reloaded.shutdown().ok());
}
