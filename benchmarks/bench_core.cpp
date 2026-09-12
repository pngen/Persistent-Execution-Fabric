// Persistent Execution Fabric - completed-work benchmarks.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Every number here measures work that has already been acknowledged as
// durable, not work that was merely submitted. The durability mode in force is
// printed with each measurement, because the honest cost of a durable commit
// depends on it.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "pef/audit.hpp"
#include "pef/runtime.hpp"

namespace {

using namespace pef;

class Timer {
public:
    void start() { begin_ = std::chrono::steady_clock::now(); }
    [[nodiscard]] std::uint64_t stop_micros() const {
        const auto end = std::chrono::steady_clock::now();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin_).count());
    }

private:
    std::chrono::steady_clock::time_point begin_{};
};

void emit(const char* name, std::uint64_t operations, std::uint64_t micros,
          const char* mode = "") {
    const double per_op =
        operations == 0 ? 0.0 : static_cast<double>(micros) / static_cast<double>(operations);
    const double per_second = micros == 0 ? 0.0
                                          : static_cast<double>(operations) * 1000000.0 /
                                                static_cast<double>(micros);
    char line[256];
    if (mode[0] == '\0') {
        std::snprintf(line, sizeof(line),
                      "BENCH %-28s n=%-8llu micros=%-10llu per_op_us=%-10.3f ops_per_second=%.0f",
                      name, static_cast<unsigned long long>(operations),
                      static_cast<unsigned long long>(micros), per_op, per_second);
    } else {
        std::snprintf(line, sizeof(line),
                      "BENCH %-28s n=%-8llu micros=%-10llu per_op_us=%-10.3f "
                      "ops_per_second=%-14.0f mode=%s",
                      name, static_cast<unsigned long long>(operations),
                      static_cast<unsigned long long>(micros), per_op, per_second, mode);
    }
    std::cout << line << std::endl;
}

struct Session {
    std::filesystem::path dir;
    Runtime runtime;
    CallerContext caller;
    ExecutionId execution;
    LeaseToken token;

    ~Session() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    [[nodiscard]] Status setup(const char* label, DurabilityMode durability,
                               std::uint64_t max_actions = 10000000) {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec) / (std::string("pef-bench-") + label);
        std::filesystem::remove_all(dir, ec);
        RuntimeConfig config;
        config.store_path = dir;
        config.create_if_missing = true;
        OpenOutcome opened;
        PEF_TRY(runtime.open(config, opened));
        caller.epoch = opened.epoch;

        ExecutionPolicy policy;
        policy.id = PolicyId{1};
        policy.generation = PolicyGeneration{1};
        policy.durability = durability;
        policy.max_action_history = static_cast<std::uint32_t>(
            max_actions > 4000000000ULL ? 4000000000ULL : max_actions);
        CreateExecutionRequest create;
        create.request = RequestId{1};
        create.policy = policy;
        CreateExecutionResult created;
        PEF_TRY(runtime.create_execution(caller, create, created));
        execution = created.execution;

        const WorkerId worker = derive_worker_id("bench-worker");
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
        token.policy_generation = created.policy_generation;
        StartRequest start;
        start.request = RequestId{3};
        start.token = token;
        PEF_TRY(runtime.start(start));
        return ok_status();
    }

    [[nodiscard]] Status commit_batch(std::uint64_t first, std::uint64_t count) {
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t index = first + i;
            BeginActionRequest begin;
            begin.request = RequestId{1000 + index * 2};
            begin.token = token;
            begin.effect_class = SideEffectClass::Pure;
            BeginActionResult action;
            PEF_TRY(runtime.begin_action(begin, action));
            CompleteActionRequest complete;
            complete.request = RequestId{1001 + index * 2};
            complete.token = token;
            complete.action = action.action;
            complete.action_generation = action.action_generation;
            complete.completion.effect_applied = true;
            CompleteActionResult result;
            PEF_TRY(runtime.complete_action(complete, result));
        }
        return ok_status();
    }

    [[nodiscard]] Status run_to(std::uint64_t target, std::uint64_t& completed) {
        while (completed < target) {
            const std::uint64_t batch = std::min<std::uint64_t>(target - completed, 1000);
            PEF_TRY(commit_batch(completed, batch));
            completed += batch;
        }
        return ok_status();
    }
};

void bench_execution_creation(std::uint64_t count) {
    Session session;
    if (!session.setup("create", DurabilityMode::JournaledNotFlushed).ok()) {
        return;
    }
    std::vector<ExecutionId> ids;
    ids.reserve(count);
    Timer timer;
    timer.start();
    for (std::uint64_t i = 0; i < count; ++i) {
        CreateExecutionRequest create;
        create.request = RequestId{100000 + i};
        create.policy.id = PolicyId{1};
        create.policy.generation = PolicyGeneration{1};
        CreateExecutionResult created;
        if (!session.runtime.create_execution(session.caller, create, created).ok()) {
            return;
        }
        ids.push_back(created.execution);
    }
    emit("execution_create", count, timer.stop_micros(), "journaled");
    (void)session.runtime.shutdown();
}

void bench_progress_commit(std::uint64_t count, DurabilityMode durability, const char* mode) {
    Session session;
    if (!session.setup("commit", durability, count + 16).ok()) {
        return;
    }
    std::uint64_t completed = 0;
    Timer timer;
    timer.start();
    if (!session.run_to(200, completed).ok()) {
        return;
    }
    const std::uint64_t warmup = timer.stop_micros();
    (void)warmup;
    Timer steady;
    steady.start();
    const std::uint64_t before = completed;
    if (!session.run_to(count, completed).ok()) {
        return;
    }
    emit("progress_commit", completed - before, steady.stop_micros(), mode);

    Timer lookup;
    lookup.start();
    for (int i = 0; i < 1000; ++i) {
        ExecutionView view;
        if (!session.runtime.query(session.execution, view).ok()) {
            return;
        }
    }
    emit("execution_query", 1000, lookup.stop_micros());

    Timer continuity;
    continuity.start();
    for (int i = 0; i < 1000; ++i) {
        ContinuationState state = ContinuationState::Invalid;
        std::string reason;
        if (!session.runtime
                 .validate_continuation(session.caller, session.execution, state, reason)
                 .ok()) {
            return;
        }
    }
    emit("continuation_validate", 1000, continuity.stop_micros());

    Timer checkpoint_timer;
    checkpoint_timer.start();
    constexpr std::uint64_t kCheckpoints = 64;
    for (std::uint64_t i = 0; i < kCheckpoints; ++i) {
        ExecutionView view;
        if (!session.runtime.query(session.execution, view).ok()) {
            return;
        }
        RegisterCheckpointRequest checkpoint;
        checkpoint.request = RequestId{900000 + i};
        checkpoint.token = session.token;
        checkpoint.expected_checkpoint = view.execution.checkpoint;
        checkpoint.expected_checkpoint_generation = view.execution.checkpoint_generation;
        checkpoint.content_size = 4096;
        checkpoint.content_hash = 0x1000 + i;
        checkpoint.effect_boundary_ordinal = view.execution.action_frontier;
        RegisterCheckpointResult registered;
        if (!session.runtime.register_checkpoint(checkpoint, registered).ok()) {
            return;
        }
    }
    emit("checkpoint_register", kCheckpoints, checkpoint_timer.stop_micros());

    Timer classify_timer;
    classify_timer.start();
    for (int i = 0; i < 1000; ++i) {
        RecoveryPlan plan;
        if (!session.runtime.classify(session.caller, session.execution, plan).ok()) {
            return;
        }
    }
    emit("recovery_classify", 1000, classify_timer.stop_micros());

    Timer audit_timer;
    audit_timer.start();
    AuditReport audit;
    if (!session.runtime.audit(audit).ok()) {
        return;
    }
    emit("invariant_audit", 1, audit_timer.stop_micros());
    std::cout << "  actions=" << audit.counts.actions << " progress=" << audit.counts.progress
              << " commits=" << audit.counts.commits
              << " checkpoints=" << audit.counts.checkpoints
              << " continuations=" << audit.counts.continuations
              << " violations=" << audit.violations() << std::endl;

    Timer snapshot_timer;
    snapshot_timer.start();
    if (!session.runtime.take_snapshot(true).ok()) {
        return;
    }
    emit("snapshot_compact", 1, snapshot_timer.stop_micros());

    if (!session.runtime.shutdown().ok()) {
        return;
    }
    Runtime reloaded;
    RuntimeConfig config;
    config.store_path = session.dir;
    OpenOutcome opened;
    Timer load_timer;
    load_timer.start();
    if (!reloaded.open(config, opened).ok()) {
        return;
    }
    emit("snapshot_load", 1, load_timer.stop_micros());
    (void)reloaded.shutdown();
}

}  // namespace

int main() {
    std::cout << "Persistent Execution Fabric completed-work benchmarks" << std::endl;
    const std::uint64_t sizes[] = {10, 100, 1000, 10000, 100000};
    for (std::uint64_t size : sizes) {
        std::cout << "-- journaled (appended, flushed at barriers) n=" << size << std::endl;
        bench_execution_creation(size);
    }
    for (std::uint64_t size : sizes) {
        std::cout << "-- journaled progress n=" << size << std::endl;
        bench_progress_commit(size, DurabilityMode::JournaledNotFlushed, "journaled");
    }
    for (std::uint64_t size : {10ULL, 100ULL, 1000ULL}) {
        std::cout << "-- durable-on-commit progress n=" << size << std::endl;
        bench_progress_commit(size, DurabilityMode::DurableOnCommit, "durable");
    }
    return 0;
}
