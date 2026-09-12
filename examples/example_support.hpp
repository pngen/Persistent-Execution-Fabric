// Persistent Execution Fabric - shared example support.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "pef/runtime.hpp"

namespace pef::example {

// Scratch store outside the source tree, removed when the example ends.
class Scratch {
public:
    explicit Scratch(const std::string& label) {
        std::error_code ec;
        auto base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            base = std::filesystem::current_path();
        }
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = base / "pef-examples" / (label + "-" + std::to_string(stamp));
        std::filesystem::create_directories(path_, ec);
    }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::remove(path_.parent_path(), ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

inline void say(const std::string& text) { std::cout << text << std::endl; }

inline int fail(const Status& status) {
    std::cerr << "example failed: " << status.to_string() << std::endl;
    return 1;
}

// Request identities are client-scoped in the fabric: reusing one returns the
// cached reply of the earlier request instead of performing the operation
// again. Examples therefore mint a fresh identity for every request.
[[nodiscard]] inline RequestId next_request() {
    static std::atomic<std::uint64_t> counter{0};
    HashBuilder builder;
    builder.u64(0x9001);
    builder.u64(counter.fetch_add(1, std::memory_order_relaxed) + 1);
    return RequestId{builder.digest()};
}

// A worker binding with a live lease, used by every example.
struct Bound {
    ExecutionId execution;
    WorkerId worker;
    WorkerBootId boot;
    LeaseToken token;
    ExecutionGeneration generation;
    PolicyGeneration policy_generation;
};

inline Status bind_worker(Runtime& runtime, const CallerContext& caller, ExecutionId execution,
                          const std::string& name, const BindingSet& bindings, Bound& out) {
    out.execution = execution;
    out.worker = derive_worker_id(name);
    out.boot = mint_worker_boot_id(out.worker);
    BindWorkerRequest request;
    request.request = next_request();
    request.execution = execution;
    request.worker = out.worker;
    request.boot = out.boot;
    request.bindings = bindings;
    BindWorkerResult bound;
    PEF_TRY(runtime.bind_worker(caller, request, bound));
    ExecutionView view;
    PEF_TRY(runtime.query(execution, view));
    out.generation = view.execution.generation;
    out.policy_generation = view.execution.policy_generation;
    out.token.execution = execution;
    out.token.execution_generation = out.generation;
    out.token.incarnation = bound.incarnation;
    out.token.incarnation_generation = bound.incarnation_generation;
    out.token.lease = bound.lease;
    out.token.lease_generation = bound.lease_generation;
    out.token.epoch = runtime.epoch();
    out.token.worker = out.worker;
    out.token.boot = out.boot;
    out.token.policy_generation = out.policy_generation;
    return ok_status();
}

inline Status refresh(Runtime& runtime, Bound& bound) {
    ExecutionView view;
    PEF_TRY(runtime.query(bound.execution, view));
    bound.generation = view.execution.generation;
    bound.policy_generation = view.execution.policy_generation;
    bound.token.execution_generation = bound.generation;
    bound.token.policy_generation = bound.policy_generation;
    bound.token.epoch = runtime.epoch();
    return ok_status();
}

inline Status ensure_running(Runtime& runtime, const CallerContext& caller, Bound& bound,
                             bool revalidate_bindings = true) {
    PEF_TRY(refresh(runtime, bound));
    ExecutionView view;
    PEF_TRY(runtime.query(bound.execution, view));
    if (view.execution.lifecycle == Lifecycle::Running) {
        return ok_status();
    }
    if (view.execution.lifecycle == Lifecycle::Ready) {
        StartRequest start;
        start.request = next_request();
        start.token = bound.token;
        return runtime.start(start);
    }
    if (view.execution.lifecycle == Lifecycle::RecoveryRequired) {
        RecoveryOutcome recovered;
        PEF_TRY(runtime.recover(caller, next_request(), bound.execution, recovered));
    }
    ResumeRequest resume;
    resume.request = next_request();
    resume.token = bound.token;
    resume.observed_bindings = view.execution.bindings;
    resume.revalidate_bindings = revalidate_bindings;
    ResumeResult resumed;
    return runtime.resume(resume, resumed);
}

inline Status commit_one(Runtime& runtime, Bound& bound, SideEffectClass cls,
                         std::uint64_t ordinal_hint, CompleteActionResult& out) {
    BeginActionRequest begin;
    begin.request = next_request();
    begin.token = bound.token;
    begin.effect_class = cls;
    begin.evidence.kind = EvidenceKind::Real;
    begin.evidence.source = "example";
    if (requires_request_key(cls)) {
        begin.request_key = "example-key-" + std::to_string(ordinal_hint);
    }
    BeginActionResult action;
    PEF_TRY(runtime.begin_action(begin, action));
    CompleteActionRequest complete;
    complete.request = next_request();
    complete.token = bound.token;
    complete.action = action.action;
    complete.action_generation = action.action_generation;
    complete.completion.effect_applied = true;
    complete.completion.evidence.kind = EvidenceKind::Real;
    complete.completion.evidence.source = "example";
    return runtime.complete_action(complete, out);
}

inline ExecutionPolicy example_policy() {
    ExecutionPolicy policy;
    policy.id = PolicyId{1};
    policy.generation = PolicyGeneration{1};
    policy.allow_older_checkpoints = true;
    return policy;
}

}  // namespace pef::example
