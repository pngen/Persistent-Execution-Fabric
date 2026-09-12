// Persistent Execution Fabric - worker executable.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// A worker executes bound work. It holds no durable authority of its own: every
// step is admitted by the coordinator, and the worker can be killed at any
// point without leaving the durable state inconsistent.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "app_support.hpp"
#include "pef/client.hpp"
#include "pef/explain.hpp"
#include "pef/version.hpp"

namespace {

void print_usage() {
    pef::app::print_line(
        "usage: pef_worker --port N [--host H] [--name NAME] [--boot HEX16]\n"
        "                  (--execution HEX16 | --create) --scenario NAME [options]\n"
        "\n"
        "scenarios:\n"
        "  progress            run --steps actions and checkpoint every --checkpoint-every\n"
        "  crash-in-flight     begin a non-repeatable action and die before any receipt\n"
        "  crash-after-receipt begin a non-repeatable action, record a receipt, then die\n"
        "  crash-before-ack    complete the effect, record a receipt, die before the commit ack\n"
        "  idle                bind and wait until killed\n"
        "\n"
        "options:\n"
        "  --steps N               number of actions for the progress scenario\n"
        "  --checkpoint-every N    checkpoint cadence, 0 disables automatic checkpoints\n"
        "  --effect-class CLS      PURE | IDEMPOTENT | REPEATABLE_WITH_KEY | COMPENSATABLE |\n"
        "                          NON_REPEATABLE | UNKNOWN\n"
        "  --request-key KEY       idempotency key for the action\n"
        "  --receipt applied|not-applied|none\n"
        "  --hold-ms N             idle time before the scenario finishes\n"
        "  --fail-actions N        fail this many actions before succeeding\n");
}

[[nodiscard]] pef::SideEffectClass default_class(const std::string& name) {
    const auto parsed = pef::parse_side_effect_class(name);
    return parsed.has_value() ? *parsed : pef::SideEffectClass::Pure;
}

// Ends the process immediately, without unwinding or flushing. This is the
// abrupt death the fabric must survive.
[[noreturn]] void die_now(int code) {
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(code);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace pef;
    app::Args args(argc, argv);
    if (args.has("--help") || args.has("-h")) {
        print_usage();
        return 0;
    }
    if (args.has("--version")) {
        app::print_line(build_info_string());
        return 0;
    }

    ClientConfig client_config;
    client_config.host = args.value_or("--host", "127.0.0.1");
    client_config.port = static_cast<std::uint16_t>(args.u64_or("--port", 0));
    client_config.name = args.value_or("--name", "pef-worker");
    client_config.version = version_string();
    const std::string scenario = args.value_or("--scenario", "progress");

    const WorkerId worker = derive_worker_id(client_config.name);
    WorkerBootId boot;
    if (const auto explicit_boot = args.value("--boot"); explicit_boot.has_value()) {
        const auto parsed = parse_id<WorkerBootIdTag>(*explicit_boot, true);
        if (!parsed.has_value()) {
            app::print_error("pef_worker: --boot is not a valid identity");
            return 2;
        }
        boot = *parsed;
    } else {
        boot = mint_worker_boot_id(worker);
    }

    CoordinatorClient client;
    const Status connected = client.connect(client_config);
    if (!connected.ok()) {
        app::print_error("pef_worker: " + connected.to_string());
        return 1;
    }
    HelloReply hello;
    const Status greeted = client.hello_as_worker(worker, boot, hello);
    if (!greeted.ok()) {
        app::print_error("pef_worker: " + greeted.to_string());
        return 1;
    }
    app::print_line("WORKER READY name=" + client_config.name + " worker=" +
                    hex64(worker.value()) + " boot=" + hex64(boot.value()) +
                    " epoch=" + std::to_string(hello.epoch.value()) +
                    " session=" + std::to_string(hello.session.value()));

    ExecutionId execution;
    if (args.has("--create")) {
        ExecutionPolicy policy;
        policy.id = PolicyId{1};
        policy.generation = PolicyGeneration{1};
        policy.checkpoint_interval_actions = 0;
        if (args.has("--allow-older-checkpoints")) {
            policy.allow_older_checkpoints = true;
        }
        if (args.has("--policy-recovery-manual")) {
            policy.recovery_preference = RecoveryPreference::RequireManualSelection;
        }
        if (args.has("--policy-compensate")) {
            policy.ambiguity_handling = AmbiguityHandling::RequireCompensation;
        }
        if (args.has("--policy-manual-ambiguity")) {
            policy.ambiguity_handling = AmbiguityHandling::ManualResolutionRequired;
        }
        CreateExecutionResult created;
        const Status status = client.create_execution(policy, created);
        if (!status.ok()) {
            app::print_error("pef_worker: create failed: " + status.to_string());
            return 1;
        }
        execution = created.execution;
        app::print_line("WORKER CREATED execution=" + hex64(execution.value()));
    } else {
        const auto explicit_execution = args.value("--execution");
        if (!explicit_execution.has_value()) {
            app::print_error("pef_worker: --execution or --create is required");
            print_usage();
            return 2;
        }
        const auto parsed = parse_id<ExecutionIdTag>(*explicit_execution, true);
        if (!parsed.has_value()) {
            app::print_error("pef_worker: --execution is not a valid identity");
            return 2;
        }
        execution = *parsed;
    }

    // Bindings the worker can currently offer.
    BindingSet bindings;
    for (const auto& value : args.positional()) {
        (void)value;
    }
    // --binding domain:id:generation[:label] may be repeated; Args::value returns
    // the first, so raw scanning is used for repeats.
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        if (args.at(i) == "--binding") {
            const auto ref = app::parse_binding(args.at(i + 1));
            if (!ref.has_value()) {
                app::print_error("pef_worker: --binding must be domain:id:generation[:label]");
                return 2;
            }
            bindings.refs.push_back(*ref);
        }
    }
    bindings.canonicalize();

    BindWorkerRequest bind;
    bind.request = client.next_request();
    bind.execution = execution;
    bind.worker = worker;
    bind.boot = boot;
    bind.bindings = bindings;
    BindWorkerResult bound;
    Status status = client.bind_worker(bind, bound);
    if (!status.ok()) {
        app::print_error("pef_worker: bind failed: " + status.to_string());
        return 1;
    }
    app::print_line("WORKER BOUND execution=" + hex64(execution.value()) + " incarnation=" +
                    hex64(bound.incarnation.value()) + " incarnation_generation=" +
                    std::to_string(bound.incarnation_generation.value()) + " lease=" +
                    hex64(bound.lease.value()) + " lease_generation=" +
                    std::to_string(bound.lease_generation.value()) + " lifecycle=" +
                    std::string(lifecycle_name(bound.lifecycle)));

    LeaseToken token;
    token.execution = execution;
    token.execution_generation = bound.lifecycle == Lifecycle::Ready
                                     ? ExecutionGeneration{1}
                                     : ExecutionGeneration{1};
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = client.epoch();
    token.worker = worker;
    token.boot = boot;

    {
        ExecutionView view;
        if (client.query(execution, view).ok()) {
            token.execution_generation = view.execution.generation;
            token.policy_generation = view.execution.policy_generation;
        }
    }

    if (scenario == "idle") {
        const int hold_ms = args.int_or("--hold-ms", 0);
        if (hold_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        } else {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                PingReply pong;
                if (!client.ping(pong).ok()) {
                    break;
                }
            }
        }
        return 0;
    }

    // Recovery runs before resume: classification is explicit and deterministic.
    {
        ExecutionView view;
        if (client.query(execution, view).ok() &&
            view.execution.lifecycle != Lifecycle::Running) {
            RecoveryOutcome recovered;
            const Status recovery_status = client.recover(execution, recovered);
            if (!recovery_status.ok()) {
                app::print_line("WORKER RECOVERY_REFUSED " + recovery_status.to_string());
            } else {
                app::print_line(std::string("WORKER RECOVERED decision=") +
                                std::string(recovery_decision_name(recovered.plan.decision)) +
                                " committed_from_receipt=" +
                                (recovered.committed ? "yes" : "no"));
            }
        }
    }

    {
        ExecutionView view;
        if (!client.query(execution, view).ok()) {
            app::print_error("pef_worker: query failed");
            return 1;
        }
        if (view.execution.lifecycle == Lifecycle::Ready) {
            StartRequest start;
            start.request = client.next_request();
            start.token = token;
            status = client.start(start);
            if (!status.ok()) {
                app::print_error("pef_worker: start failed: " + status.to_string());
                return 1;
            }
            app::print_line("WORKER STARTED");
        } else if (view.execution.lifecycle != Lifecycle::Running) {
            ResumeRequest resume;
            resume.request = client.next_request();
            resume.token = token;
            resume.observed_bindings = bindings;
            resume.revalidate_bindings = true;
            ResumeResult resumed;
            status = client.resume(resume, resumed);
            if (!status.ok()) {
                app::print_error("pef_worker: resume failed: " + status.to_string());
                return 1;
            }
            app::print_line(std::string("WORKER RESUMED lifecycle=") +
                            std::string(lifecycle_name(resumed.lifecycle)) +
                            " continuation=" +
                            std::string(continuation_state_name(resumed.continuation_state)));
        }
    }

    const SideEffectClass effect_class =
        default_class(args.value_or("--effect-class", "PURE"));
    const std::string request_key = args.value_or("--request-key", "key-" + client_config.name);
    const std::string receipt_mode = args.value_or("--receipt", "none");
    const std::uint64_t steps = args.u64_or("--steps", 3);
    const std::uint64_t checkpoint_every = args.u64_or("--checkpoint-every", 0);
    const std::uint64_t fail_actions = args.u64_or("--fail-actions", 0);

    if (scenario == "crash-in-flight" || scenario == "crash-after-receipt" ||
        scenario == "crash-before-ack") {
        BeginActionRequest begin;
        begin.request = client.next_request();
        begin.token = token;
        begin.effect_class = effect_class == SideEffectClass::Pure
                                 ? SideEffectClass::NonRepeatable
                                 : effect_class;
        begin.request_key = request_key;
        begin.evidence.kind = EvidenceKind::Real;
        begin.evidence.source = "worker";
        BeginActionResult action;
        status = client.begin_action(begin, action);
        if (!status.ok()) {
            app::print_error("pef_worker: begin_action failed: " + status.to_string());
            return 1;
        }
        app::print_line("WORKER ACTION ordinal=" + std::to_string(action.ordinal) +
                        " action=" + hex64(action.action.value()) + " class=" +
                        std::string(side_effect_class_name(begin.effect_class)));
        if (scenario != "crash-in-flight") {
            // The physical effect has happened. Record it durably before dying.
            ReportSideEffectRequest receipt;
            receipt.request = client.next_request();
            receipt.token = token;
            receipt.action = action.action;
            receipt.action_generation = action.action_generation;
            receipt.applied = receipt_mode != "not-applied";
            receipt.evidence.kind = EvidenceKind::Real;
            receipt.evidence.source = "external-receipt";
            status = client.report_side_effect(receipt);
            if (!status.ok()) {
                app::print_error("pef_worker: report_side_effect failed: " + status.to_string());
                return 1;
            }
            app::print_line("WORKER RECEIPT applied=" +
                            std::string(receipt.applied ? "yes" : "no"));
        }
        app::print_line("WORKER CRASHING scenario=" + scenario);
        die_now(3);
    }

    for (std::uint64_t step = 0; step < steps; ++step) {
        BeginActionRequest begin;
        begin.request = client.next_request();
        begin.token = token;
        begin.effect_class = effect_class;
        begin.request_key = requires_request_key(effect_class) ? request_key : std::string{};
        begin.evidence.kind = EvidenceKind::Real;
        begin.evidence.source = "worker";
        BeginActionResult action;
        status = client.begin_action(begin, action);
        if (!status.ok()) {
            app::print_error("pef_worker: begin_action failed: " + status.to_string());
            return 1;
        }
        if (step < fail_actions) {
            FailActionRequest failure;
            failure.request = client.next_request();
            failure.token = token;
            failure.action = action.action;
            failure.action_generation = action.action_generation;
            failure.reason = "scripted failure";
            failure.evidence.kind = EvidenceKind::Real;
            failure.evidence.source = "worker";
            status = client.fail_action(failure);
            if (!status.ok()) {
                app::print_error("pef_worker: fail_action failed: " + status.to_string());
                return 1;
            }
            app::print_line("WORKER ACTION_FAILED ordinal=" + std::to_string(action.ordinal));
            continue;
        }
        CompleteActionRequest complete;
        complete.request = client.next_request();
        complete.token = token;
        complete.action = action.action;
        complete.action_generation = action.action_generation;
        complete.completion.effect_applied = true;
        complete.completion.evidence.kind = EvidenceKind::Real;
        complete.completion.evidence.source = "worker";
        CompleteActionResult committed;
        status = client.complete_action(complete, committed);
        if (!status.ok()) {
            app::print_error("pef_worker: complete_action failed: " + status.to_string());
            return 1;
        }
        app::print_line("WORKER STEP ordinal=" + std::to_string(committed.ordinal) +
                        " progress_generation=" +
                        std::to_string(committed.progress_generation.value()) + " commit=" +
                        hex64(committed.commit.value()) +
                        " duplicate=" + (committed.duplicate ? "yes" : "no"));

        if (checkpoint_every > 0 && ((step + 1) % checkpoint_every) == 0) {
            ExecutionView view;
            if (client.query(execution, view).ok()) {
                RegisterCheckpointRequest checkpoint;
                checkpoint.request = client.next_request();
                checkpoint.token = token;
                checkpoint.expected_checkpoint = view.execution.checkpoint;
                checkpoint.expected_checkpoint_generation = view.execution.checkpoint_generation;
                checkpoint.content_size = 1024 * (step + 1);
                checkpoint.content_hash = fnv1a(request_key) ^ (step + 1);
                checkpoint.effect_boundary_ordinal = committed.ordinal;
                RegisterCheckpointResult registered;
                status = client.register_checkpoint(checkpoint, registered);
                if (!status.ok()) {
                    app::print_error("pef_worker: checkpoint failed: " + status.to_string());
                    return 1;
                }
                app::print_line("WORKER CHECKPOINT generation=" +
                                std::to_string(registered.checkpoint_generation.value()) +
                                " depth=" + std::to_string(registered.lineage_depth) +
                                " continuation=" +
                                std::string(continuation_state_name(ContinuationState::Valid)));
            }
        }
    }

    const int hold_ms = args.int_or("--hold-ms", 0);
    if (hold_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    }
    app::print_line("WORKER DONE steps=" + std::to_string(steps));
    ExecutionView final_view;
    if (client.query(execution, final_view).ok()) {
        app::print_line("WORKER FINAL progress_generation=" +
                        std::to_string(final_view.execution.progress_generation.value()) +
                        " committed=" + std::to_string(final_view.execution.committed_actions) +
                        " lifecycle=" +
                        std::string(lifecycle_name(final_view.execution.lifecycle)));
    }
    client.close();
    return 0;
}
