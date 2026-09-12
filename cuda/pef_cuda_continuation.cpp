// Persistent Execution Fabric - real hardware-bound continuation proof.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// What is proven here is durable execution semantics AROUND a hardware-bound
// action, not migration of device state: the fabric never claims that GPU
// memory or a device context was checkpointed. The device buffer is recomputed,
// and the proof shows that a stale device binding generation cannot resume.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "cuda_driver.hpp"
#include "pef/audit.hpp"
#include "pef/explain.hpp"
#include "pef/runtime.hpp"

namespace {

using namespace pef;

constexpr std::uint32_t kElementCount = 1u << 16;

void say(const std::string& text) { std::cout << text << std::endl; }

int fail(const Status& status) {
    std::cerr << "cuda proof failed: " << status.to_string() << std::endl;
    return 1;
}

// The CPU reference for the kernel: out[i] = in[i] * 2 + 1.
std::vector<float> cpu_reference(const std::vector<float>& input) {
    std::vector<float> output(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        output[i] = input[i] * 2.0F + 1.0F;
    }
    return output;
}

std::uint64_t digest_of(const std::vector<float>& values) {
    HashBuilder builder;
    builder.u64(values.size());
    for (float value : values) {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "float must be 32 bits");
        std::memcpy(&bits, &value, sizeof(bits));
        builder.u32(bits);
    }
    return builder.digest();
}

struct BoundWorker {
    ExecutionId execution;
    WorkerId worker;
    WorkerBootId boot;
    LeaseToken token;
};

Status bind_worker(Runtime& runtime, const CallerContext& caller, ExecutionId execution,
                   const BindingSet& bindings, std::uint64_t request_id, BoundWorker& out) {
    out.execution = execution;
    out.worker = derive_worker_id("cuda-worker");
    out.boot = mint_worker_boot_id(out.worker);
    BindWorkerRequest request;
    // Request identities are distinct per attempt: reusing one would return the
    // cached reply of the earlier bind instead of binding again.
    request.request = RequestId{request_id};
    request.execution = execution;
    request.worker = out.worker;
    request.boot = out.boot;
    request.bindings = bindings;
    BindWorkerResult bound;
    PEF_TRY(runtime.bind_worker(caller, request, bound));
    ExecutionView view;
    PEF_TRY(runtime.query(execution, view));
    out.token.execution = execution;
    out.token.execution_generation = view.execution.generation;
    out.token.incarnation = bound.incarnation;
    out.token.incarnation_generation = bound.incarnation_generation;
    out.token.lease = bound.lease;
    out.token.lease_generation = bound.lease_generation;
    out.token.epoch = runtime.epoch();
    out.token.worker = out.worker;
    out.token.boot = out.boot;
    out.token.policy_generation = view.execution.policy_generation;
    return ok_status();
}

}  // namespace

int main() {
    say("=== Persistent Execution Fabric hardware-bound continuation proof ===");

    // ---- device discovery -------------------------------------------------
    std::string reason;
    if (!cuda::driver_available(reason)) {
        say("UNSUPPORTED: " + reason);
        say("This environment cannot produce real hardware evidence, so no claim is made.");
        return 0;
    }
    const cuda::DeviceInfo device = cuda::describe_device(0);
    if (device.availability != cuda::Availability::Real) {
        say("UNSUPPORTED: " + device.detail);
        return 0;
    }
    say("REAL device ordinal=" + std::to_string(device.ordinal) + " name=" + device.name);
    say("REAL device uuid=" + device.uuid + " compute=" + std::to_string(device.compute_major) +
        "." + std::to_string(device.compute_minor) + " memory_bytes=" +
        std::to_string(device.total_memory));

    // The hardware binding generation is derived from the device identity the
    // fabric observed. A different device yields a different binding identity.
    const std::uint64_t device_identity = fnv1a("cuda:" + device.uuid);
    const BindingId hardware_binding{device_identity};

    std::error_code ec;
    const auto store_path = std::filesystem::temp_directory_path(ec) / "pef-cuda-proof-store";
    std::filesystem::remove_all(store_path, ec);

    Runtime runtime;
    RuntimeConfig config;
    config.store_path = store_path;
    config.create_if_missing = true;
    OpenOutcome opened;
    if (const Status status = runtime.open(config, opened); !status.ok()) {
        return fail(status);
    }
    CallerContext caller{SessionId{}, opened.epoch};

    ExecutionPolicy policy;
    policy.id = PolicyId{1};
    policy.generation = PolicyGeneration{1};
    policy.allow_recompute = true;
    CreateExecutionRequest create;
    create.request = RequestId{1};
    create.policy = policy;
    CreateExecutionResult created;
    if (const Status status = runtime.create_execution(caller, create, created); !status.ok()) {
        return fail(status);
    }

    BindingSet generation_one;
    generation_one.refs.push_back(BindingRef{BindingDomain::Hardware, hardware_binding,
                                             BindingGeneration{1}, device.name});
    generation_one.canonicalize();

    BoundWorker first;
    if (const Status status =
            bind_worker(runtime, caller, created.execution, generation_one, 100, first);
        !status.ok()) {
        return fail(status);
    }
    StartRequest start;
    start.request = RequestId{2};
    start.token = first.token;
    if (const Status status = runtime.start(start); !status.ok()) {
        return fail(status);
    }
    say("REAL execution created and started under hardware binding generation 1");

    // ---- the hardware-bound action ----------------------------------------
    std::vector<float> input(kElementCount);
    for (std::uint32_t i = 0; i < kElementCount; ++i) {
        input[i] = static_cast<float>(i % 97) * 0.5F;
    }
    const std::vector<float> expected = cpu_reference(input);

    BeginActionRequest begin;
    begin.request = RequestId{3};
    begin.token = first.token;
    begin.effect_class = SideEffectClass::Pure;
    begin.evidence.kind = EvidenceKind::Real;
    begin.evidence.source = "cuda-device";
    BeginActionResult action;
    if (const Status status = runtime.begin_action(begin, action); !status.ok()) {
        return fail(status);
    }

    cuda::DeviceBuffer device_in;
    cuda::DeviceBuffer device_out;
    std::string error;
    if (!device_in.allocate(kElementCount * sizeof(float), error)) {
        say("UNSUPPORTED: " + error);
        return 0;
    }
    if (!device_out.allocate(kElementCount * sizeof(float), error)) {
        say("UNSUPPORTED: " + error);
        return 0;
    }
    if (!device_in.copy_in(input.data(), kElementCount * sizeof(float), error)) {
        say("FAILED: " + error);
        return 1;
    }
    if (!cuda::launch_scale_kernel(device_out, device_in, kElementCount, error)) {
        say("FAILED: " + error);
        return 1;
    }
    std::vector<float> actual(kElementCount);
    if (!device_out.copy_out(actual.data(), kElementCount * sizeof(float), error)) {
        say("FAILED: " + error);
        return 1;
    }
    bool parity = actual == expected;
    say(std::string("REAL kernel executed on ") + device.name +
        "; CPU parity: " + (parity ? "exact" : "MISMATCH"));
    if (!parity) {
        return 1;
    }
    const std::uint64_t output_digest = digest_of(actual);

    // ---- checkpoint of verified output metadata ---------------------------
    ExecutionView view;
    if (const Status status = runtime.query(created.execution, view); !status.ok()) {
        return fail(status);
    }
    RegisterCheckpointRequest checkpoint;
    checkpoint.request = RequestId{4};
    checkpoint.token = first.token;
    checkpoint.expected_checkpoint = view.execution.checkpoint;
    checkpoint.expected_checkpoint_generation = view.execution.checkpoint_generation;
    checkpoint.content_size = kElementCount * sizeof(float);
    checkpoint.content_hash = output_digest;
    checkpoint.effect_boundary_ordinal = view.execution.action_frontier;
    RegisterCheckpointResult registered;
    if (const Status status = runtime.register_checkpoint(checkpoint, registered); !status.ok()) {
        return fail(status);
    }
    say("REAL verified output digest=" + hex64(output_digest) +
        " registered as checkpoint generation " +
        std::to_string(registered.checkpoint_generation.value()));
    say("NOTE the fabric stores the checkpoint description; the device buffer itself is not "
        "checkpointed and no device state is migrated.");

    CompleteActionRequest complete;
    complete.request = RequestId{5};
    complete.token = first.token;
    complete.action = action.action;
    complete.action_generation = action.action_generation;
    complete.completion.effect_applied = true;
    complete.completion.evidence.kind = EvidenceKind::Real;
    complete.completion.evidence.source = "cuda-device";
    CompleteActionResult committed;
    if (const Status status = runtime.complete_action(complete, committed); !status.ok()) {
        return fail(status);
    }
    say("REAL commit ordinal=" + std::to_string(committed.ordinal) +
        " progress_generation=" + std::to_string(committed.progress_generation.value()));
    device_in.release();
    device_out.release();

    // ---- worker loss ------------------------------------------------------
    if (const Status status = runtime.fence(caller, RequestId{6}, created.execution,
                                            "simulated worker loss after the kernel ran");
        !status.ok()) {
        return fail(status);
    }
    say("FENCE old worker boot " + hex64(first.boot.value()) + " lost authority");

    // ---- a stale device binding generation must not resume ----------------
    BindingSet generation_two;
    generation_two.refs.push_back(BindingRef{BindingDomain::Hardware, hardware_binding,
                                             BindingGeneration{2}, device.name});
    generation_two.canonicalize();

    // Classification is applied first, so the only remaining obstacle to the
    // stale attempt is the hardware binding generation itself.
    RecoveryOutcome classified;
    if (const Status status = runtime.recover(caller, RequestId{90}, created.execution, classified);
        !status.ok()) {
        return fail(status);
    }
    say("RECOVERY decision=" +
        std::string(recovery_decision_name(classified.plan.decision)));

    BoundWorker replacement;
    if (const Status status =
            bind_worker(runtime, caller, created.execution, generation_two, 101, replacement);
        !status.ok()) {
        return fail(status);
    }

    // The continuation issued before the device binding moved is revalidated
    // against the generations the execution now requires.
    ContinuationState continuation_state = ContinuationState::Invalid;
    std::string continuation_reason;
    if (const Status status = runtime.validate_continuation(caller, created.execution,
                                                            continuation_state,
                                                            continuation_reason);
        !status.ok()) {
        return fail(status);
    }
    say("STALE CONTINUATION: " + std::string(continuation_state_name(continuation_state)) +
        " (" + continuation_reason + ")");

    // A worker that still presents the previous device binding generation must
    // not be allowed to resume.
    ResumeRequest stale_resume;
    stale_resume.request = RequestId{7};
    stale_resume.token = replacement.token;
    stale_resume.observed_bindings = generation_one;
    stale_resume.revalidate_bindings = false;
    ResumeResult refused;
    const Status stale_status = runtime.resume(stale_resume, refused);
    say("STALE DEVICE BINDING: " + stale_status.to_string());
    if (stale_status.ok() || stale_status.code() != Code::StaleBindingGeneration) {
        say("FAILED: a stale hardware binding generation resumed execution");
        return 1;
    }
    if (continuation_state == ContinuationState::Valid) {
        say("FAILED: a continuation bound to the previous device generation stayed VALID");
        return 1;
    }

    // ---- revalidate and resume -------------------------------------------
    ResumeRequest resume;
    resume.request = RequestId{10};
    resume.token = replacement.token;
    resume.observed_bindings = generation_two;
    resume.revalidate_bindings = true;
    ResumeResult resumed;
    if (const Status status = runtime.resume(resume, resumed); !status.ok()) {
        return fail(status);
    }
    say("RESUMED under hardware binding generation 2; continuation=" +
        std::string(continuation_state_name(resumed.continuation_state)));

    // ---- safely recompute the PURE action ---------------------------------
    BeginActionRequest replay_begin;
    replay_begin.request = RequestId{11};
    replay_begin.token = replacement.token;
    replay_begin.effect_class = SideEffectClass::Pure;
    replay_begin.evidence.kind = EvidenceKind::Real;
    replay_begin.evidence.source = "cuda-device";
    BeginActionResult replay_action;
    if (const Status status = runtime.begin_action(replay_begin, replay_action); !status.ok()) {
        return fail(status);
    }
    cuda::DeviceBuffer replay_in;
    cuda::DeviceBuffer replay_out;
    if (!replay_in.allocate(kElementCount * sizeof(float), error) ||
        !replay_out.allocate(kElementCount * sizeof(float), error) ||
        !replay_in.copy_in(input.data(), kElementCount * sizeof(float), error) ||
        !cuda::launch_scale_kernel(replay_out, replay_in, kElementCount, error)) {
        say("FAILED during recompute: " + error);
        return 1;
    }
    std::vector<float> recomputed(kElementCount);
    if (!replay_out.copy_out(recomputed.data(), kElementCount * sizeof(float), error)) {
        say("FAILED: " + error);
        return 1;
    }
    const bool replay_parity = recomputed == expected;
    say(std::string("REAL recompute on ") + device.name +
        "; CPU parity: " + (replay_parity ? "exact" : "MISMATCH"));
    if (!replay_parity) {
        return 1;
    }
    const std::uint64_t replay_digest = digest_of(recomputed);
    say("REAL recomputed digest matches the checkpoint digest: " +
        std::string(replay_digest == output_digest ? "yes" : "NO"));

    CompleteActionRequest replay_complete;
    replay_complete.request = RequestId{12};
    replay_complete.token = replacement.token;
    replay_complete.action = replay_action.action;
    replay_complete.action_generation = replay_action.action_generation;
    replay_complete.completion.effect_applied = true;
    replay_complete.completion.evidence.kind = EvidenceKind::Real;
    replay_complete.completion.evidence.source = "cuda-device";
    CompleteActionResult replay_committed;
    if (const Status status = runtime.complete_action(replay_complete, replay_committed);
        !status.ok()) {
        return fail(status);
    }
    say("REAL commit ordinal=" + std::to_string(replay_committed.ordinal) +
        " progress_generation=" + std::to_string(replay_committed.progress_generation.value()));

    // ---- cleanup and audit ------------------------------------------------
    replay_in.release();
    replay_out.release();
    AuditReport audit;
    if (const Status status = runtime.audit(audit); !status.ok()) {
        return fail(status);
    }
    say("AUDIT violations=" + std::to_string(audit.violations()));
    ExecutionView final_view;
    if (const Status status = runtime.query(created.execution, final_view); !status.ok()) {
        return fail(status);
    }
    say("FINAL progress_generation=" +
        std::to_string(final_view.execution.progress_generation.value()) +
        " commits=" + std::to_string(final_view.commit_count));
    if (const Status status = runtime.shutdown(); !status.ok()) {
        return fail(status);
    }
    std::filesystem::remove_all(store_path, ec);
    say("RESOURCES released: device buffers freed, store removed");
    return audit.clean() ? 0 : 1;
}
