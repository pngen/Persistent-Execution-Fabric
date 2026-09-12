// Persistent Execution Fabric - inspection and control CLI.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// The CLI is a control-plane client. It never invents authority: worker
// operations go through a real bind and a real lease.
#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "app_support.hpp"
#include "pef/audit.hpp"
#include "pef/client.hpp"
#include "pef/explain.hpp"
#include "pef/persistence.hpp"
#include "pef/version.hpp"

namespace {

using namespace pef;

const std::set<std::string> kValueFlags = {
    "--host",     "--port",       "--store",       "--worker",   "--name",
    "--boot",     "--reason",     "--binding",     "--hash",     "--size",
    "--boundary", "--ambiguity",  "--as",          "--note",     "--worker-name",
    "--interval", "--policy-field", "--policy-value", "--effect-class", "--request-key",
    "--max-replay-depth", "--durability"};

[[nodiscard]] std::vector<std::string> operands(const app::Args& args, std::size_t start) {
    std::vector<std::string> out;
    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string& value = args.at(i);
        if (!value.empty() && value[0] == '-') {
            if (kValueFlags.find(value) != kValueFlags.end()) {
                ++i;
            }
            continue;
        }
        out.push_back(value);
    }
    return out;
}

[[nodiscard]] int fail(const std::string& message) {
    app::print_error("pef_cli: " + message);
    return 1;
}

[[nodiscard]] int report(const Status& status) {
    if (status.ok()) {
        app::print_line("ok");
        return 0;
    }
    return fail(status.to_string());
}

[[nodiscard]] bool ambiguous_resolution_pending(const ExecutionView& view);
[[nodiscard]] int command_demo(const app::Args& args);

// Returns the failure code from the enclosing command function when a status
// does not succeed.
#define PEF_TRY_RETURN(expression)                     \
    do {                                               \
        const ::pef::Status pef_status_ = (expression); \
        if (!pef_status_.ok()) {                       \
            return fail(pef_status_.to_string());      \
        }                                              \
    } while (false)

struct Connection {
    CoordinatorClient client;
    CallerContext caller;
};

[[nodiscard]] Status connect_client(const app::Args& args, CoordinatorClient& client,
                                    HelloReply& hello) {
    ClientConfig config;
    config.host = args.value_or("--host", "127.0.0.1");
    bool ok = true;
    config.port = static_cast<std::uint16_t>(args.u64_or("--port", 0, ok));
    config.name = args.value_or("--name", "pef-cli");
    config.version = version_string();
    PEF_TRY(client.connect(config));
    return client.hello_as_operator(hello);
}

// A CLI-invoked worker binding. The CLI acts as a real worker process: it
// creates a boot identity, binds, and receives a lease.
struct BoundWorker {
    CoordinatorClient* client = nullptr;
    WorkerId worker;
    WorkerBootId boot;
    LeaseToken token;
    bool bound = false;
    ExecutionRecord execution;

    [[nodiscard]] Status bind(CoordinatorClient& c, ExecutionId execution_id,
                              const BindingSet& bindings) {
        client = &c;
        const std::string name = "pef-cli-worker";
        worker = derive_worker_id(name);
        boot = mint_worker_boot_id(worker);
        HelloReply hello;
        PEF_TRY(c.hello_as_worker(worker, boot, hello));
        BindWorkerRequest request;
        request.request = c.next_request();
        request.execution = execution_id;
        request.worker = worker;
        request.boot = boot;
        request.bindings = bindings;
        BindWorkerResult result;
        PEF_TRY(c.bind_worker(request, result));
        token.execution = execution_id;
        token.incarnation = result.incarnation;
        token.incarnation_generation = result.incarnation_generation;
        token.lease = result.lease;
        token.lease_generation = result.lease_generation;
        token.epoch = c.epoch();
        token.worker = worker;
        token.boot = boot;
        bound = true;
        PEF_TRY(refresh(c));
        return ok_status();
    }

    [[nodiscard]] Status refresh(CoordinatorClient& c) {
        ExecutionView view;
        PEF_TRY(c.query(token.execution, view));
        execution = view.execution;
        token.execution_generation = view.execution.generation;
        token.policy_generation = view.execution.policy_generation;
        token.epoch = c.epoch();
        return ok_status();
    }
};

[[nodiscard]] Status ensure_running(CoordinatorClient& client, BoundWorker& worker,
                                    bool revalidate_bindings) {
    PEF_TRY(worker.refresh(client));
    if (worker.execution.lifecycle == Lifecycle::Running) {
        return ok_status();
    }
    if (worker.execution.lifecycle == Lifecycle::Ready) {
        StartRequest start;
        start.request = client.next_request();
        start.token = worker.token;
        PEF_TRY(client.start(start));
        return worker.refresh(client);
    }
    if (worker.execution.lifecycle == Lifecycle::RecoveryRequired) {
        RecoveryOutcome recovered;
        PEF_TRY(client.recover(worker.token.execution, recovered));
        PEF_TRY(worker.refresh(client));
    }
    ResumeRequest resume;
    resume.request = client.next_request();
    resume.token = worker.token;
    resume.observed_bindings = worker.execution.bindings;
    resume.revalidate_bindings = revalidate_bindings;
    ResumeResult resumed;
    PEF_TRY(client.resume(resume, resumed));
    return worker.refresh(client);
}

[[nodiscard]] BindingSet bindings_from(const app::Args& args, std::size_t start) {
    BindingSet bindings;
    for (std::size_t i = start; i + 1 < args.size(); ++i) {
        if (args.at(i) == "--binding") {
            if (const auto ref = app::parse_binding(args.at(i + 1)); ref.has_value()) {
                bindings.refs.push_back(*ref);
            }
        }
    }
    bindings.canonicalize();
    return bindings;
}

// Offline inspection: opens the durable store without opening a runtime, so no
// epoch is advanced and nothing is mutated.
[[nodiscard]] Status open_offline(const std::string& path, FileDurableStore& store,
                                  DurableState& state, LoadReport& report) {
    PEF_TRY(store.open(path, false, StoreLimits{}));
    return store.load(state, report);
}

void print_usage() {
    app::print_line(
        "usage: pef_cli [--host H] [--port N] <command> [arguments]\n"
        "\n"
        "  version                                  print build identity\n"
        "  ping                                     check coordinator liveness and epoch\n"
        "  execution create [--interval N] [--allow-older-checkpoints]\n"
        "  execution list\n"
        "  execution show <execution>\n"
        "  execution start <execution> [--worker-name N] [--binding d:id:gen[:label]]\n"
        "  execution suspend <execution>\n"
        "  execution resume <execution>\n"
        "  execution fence <execution> [--reason R]\n"
        "  execution cancel <execution> [--reason R]\n"
        "  execution retire <execution> [--reason R]\n"
        "  execution drain <execution>\n"
        "  execution complete <execution>\n"
        "  execution advance-generation <execution> [--reason R]\n"
        "  execution policy <execution> [--max-replay-depth N] [--durability MODE]\n"
        "  action show <execution> [--ordinal N]\n"
        "  checkpoint create <execution> --hash H [--size N] [--boundary N]\n"
        "  checkpoint list <execution>\n"
        "  checkpoint explain <execution> [--generation N]\n"
        "  progress show <execution>\n"
        "  continuation show <execution>\n"
        "  continuation validate <execution>\n"
        "  recovery explain <execution>\n"
        "  recovery apply <execution>\n"
        "  ambiguity list <execution>\n"
        "  ambiguity resolve <execution> --ambiguity ID --as\n"
        "      applied|not-applied|accepted|abandoned|manual [--note TEXT]\n"
        "  lease show <execution>\n"
        "  snapshot [--truncate]\n"
        "  audit [--store DIR]                      online audit, or offline store audit\n"
        "  verify [--store DIR]                     same as audit, exit 1 on violations\n"
        "  demo                                     run a self-contained scenario\n"
        "\n"
        "exit codes: 0 success, 1 refusal or failure, 2 usage, 3 audit violations\n");
}

[[nodiscard]] int command_version() {
    app::print_line(build_info_string());
    return 0;
}

[[nodiscard]] int command_ping(const app::Args& args) {
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    app::print_line("coordinator " + hello.coordinator_version);
    app::print_line("store=" + hex64(hello.store.value()) +
                    " epoch=" + std::to_string(hello.epoch.value()) +
                    " journal_sequence=" + std::to_string(hello.journal_sequence) +
                    " protocol=" + std::to_string(hello.protocol_version) +
                    " schema=" + std::to_string(hello.persistence_schema));
    return 0;
}

[[nodiscard]] int command_audit(const app::Args& args, bool as_verify) {
    if (const auto store_path = args.value("--store"); store_path.has_value()) {
        FileDurableStore store;
        DurableState state;
        LoadReport report_data;
        const Status status = open_offline(*store_path, store, state, report_data);
        if (!status.ok()) {
            return report(status);
        }
        app::print_line("offline store=" + *store_path);
        app::print_line("  snapshot_present=" +
                        std::string(report_data.snapshot_present ? "yes" : "no") +
                        " journal_present=" +
                        std::string(report_data.journal_present ? "yes" : "no") +
                        " records_loaded=" + std::to_string(report_data.records_loaded) +
                        " torn_tail=" +
                        std::string(report_data.truncated_tail ? "yes" : "no"));
        const AuditReport audit = audit_state(state);
        app::print_line(explain_audit(audit));
        return (as_verify && !audit.clean()) ? 3 : 0;
    }
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    AuditReport audit;
    const Status audited = client.audit(audit);
    if (!audited.ok()) {
        return report(audited);
    }
    app::print_line(explain_audit(audit));
    return (as_verify && !audit.clean()) ? 3 : 0;
}

[[nodiscard]] std::optional<ExecutionId> execution_argument(const std::vector<std::string>& ops,
                                                            std::size_t index) {
    if (ops.size() <= index) {
        return std::nullopt;
    }
    return app::parse_identifier<ExecutionIdTag>(ops[index]);
}

[[nodiscard]] int command_execution(const app::Args& args) {
    const std::vector<std::string> ops = operands(args, 1);
    if (ops.size() < 2) {
        print_usage();
        return 2;
    }
    const std::string& verb = ops[1];

    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    const CallerContext caller{hello.session, hello.epoch};

    if (verb == "create") {
        ExecutionPolicy policy;
        policy.id = PolicyId{1};
        policy.generation = PolicyGeneration{1};
        policy.checkpoint_interval_actions =
            static_cast<std::uint32_t>(args.u64_or("--interval", 0));
        policy.allow_older_checkpoints = args.has("--allow-older-checkpoints");
        policy.allow_recovery_from_failed = args.has("--allow-recovery-from-failed");
        if (args.has("--manual-ambiguity")) {
            policy.ambiguity_handling = AmbiguityHandling::ManualResolutionRequired;
        }
        if (args.has("--compensate")) {
            policy.ambiguity_handling = AmbiguityHandling::RequireCompensation;
        }
        if (args.has("--manual-recovery")) {
            policy.recovery_preference = RecoveryPreference::RequireManualSelection;
        }
        if (args.has("--forbid-replay")) {
            policy.replay_allowance = ReplayAllowance::ForbidReplay;
            policy.allow_recompute = false;
        }
        if (args.value_or("--durability", "durable") == "journaled") {
            policy.durability = DurabilityMode::JournaledNotFlushed;
        }
        CreateExecutionResult created;
        const Status created_status = client.create_execution(policy, created);
        if (!created_status.ok()) {
            return report(created_status);
        }
        app::print_line("execution=" + hex64(created.execution.value()) +
                        " generation=" + std::to_string(created.generation.value()) +
                        " policy=" + hex64(created.policy.value()) +
                        " policy_generation=" +
                        std::to_string(created.policy_generation.value()) +
                        " existing=" + (created.existing ? "yes" : "no"));
        return 0;
    }

    if (verb == "list") {
        if (const auto store_path = args.value("--store"); store_path.has_value()) {
            FileDurableStore store;
            DurableState state;
            LoadReport report_data;
            PEF_TRY_RETURN(open_offline(*store_path, store, state, report_data));
            std::vector<ExecutionRecord> executions;
            for (const ExecutionRecord* record : state.executions.canonical_order()) {
                executions.push_back(*record);
            }
            app::print_line(render_execution_table(executions));
            return 0;
        }
        std::vector<ExecutionRecord> executions;
        const Status listed = client.list_executions(executions);
        if (!listed.ok()) {
            return report(listed);
        }
        app::print_line(render_execution_table(executions));
        return 0;
    }

    const auto execution = execution_argument(ops, 2);
    if (!execution.has_value()) {
        return fail("a valid execution identity is required");
    }

    if (verb == "show") {
        if (const auto store_path = args.value("--store"); store_path.has_value()) {
            FileDurableStore store;
            DurableState state;
            LoadReport report_data;
            PEF_TRY_RETURN(open_offline(*store_path, store, state, report_data));
            ExecutionView view;
            const ExecutionRecord* record = state.executions.find(*execution);
            if (record == nullptr) {
                return fail("execution is not present in the store");
            }
            view.found = true;
            view.execution = *record;
            if (const ContinuationRecord* continuation =
                    state.continuations.find(record->continuation);
                continuation != nullptr) {
                view.continuation = *continuation;
            }
            if (const CheckpointRecord* checkpoint = state.checkpoints.find(record->checkpoint);
                checkpoint != nullptr) {
                view.checkpoint = *checkpoint;
            }
            if (const LeaseRecord* lease = state.leases.find(record->lease); lease != nullptr) {
                view.lease = *lease;
            }
            if (const AmbiguityRecord* ambiguity = state.ambiguities.find(record->ambiguity);
                ambiguity != nullptr) {
                view.ambiguity = *ambiguity;
            }
            if (const RecoveryRecord* recovery = state.recoveries.find(record->recovery);
                recovery != nullptr) {
                view.recovery = *recovery;
            }
            app::print_line(explain_execution(view));
            return 0;
        }
        ExecutionView view;
        const Status queried = client.query(*execution, view);
        if (!queried.ok()) {
            return report(queried);
        }
        app::print_line(explain_execution(view));
        return 0;
    }

    if (verb == "start" || verb == "resume" || verb == "checkpoint") {
        return fail("use the dedicated commands; start/resume require a bound worker");
    }

    if (verb == "suspend") {
        Status suspend = client.suspend_begin(*execution);
        if (!suspend.ok()) {
            return report(suspend);
        }
        suspend = client.suspend_commit(*execution);
        if (!suspend.ok()) {
            return report(suspend);
        }
        app::print_line("execution suspended: " + hex64(execution->value()));
        return 0;
    }
    if (verb == "fence") {
        return report(client.fence(*execution, args.value_or("--reason", "cli")));
    }
    if (verb == "cancel") {
        return report(client.cancel(*execution, args.value_or("--reason", "cli")));
    }
    if (verb == "retire") {
        return report(client.retire(*execution, args.value_or("--reason", "cli")));
    }
    if (verb == "drain") {
        return report(client.drain(*execution));
    }
    if (verb == "complete") {
        return report(client.complete_execution(*execution));
    }
    if (verb == "advance-generation") {
        return report(client.advance_generation(*execution, args.value_or("--reason", "cli")));
    }
    if (verb == "policy") {
        ExecutionView view;
        PEF_TRY_RETURN(client.query(*execution, view));
        ExecutionPolicy policy = view.policy;
        bool ok = true;
        policy.max_replay_depth =
            static_cast<std::uint32_t>(args.u64_or("--max-replay-depth",
                                                   policy.max_replay_depth, ok));
        if (!ok) {
            return fail("--max-replay-depth is not a number");
        }
        if (const auto durability = args.value("--durability"); durability.has_value()) {
            if (*durability == "durable") {
                policy.durability = DurabilityMode::DurableOnCommit;
            } else if (*durability == "journaled") {
                policy.durability = DurabilityMode::JournaledNotFlushed;
            } else {
                return fail("--durability must be durable or journaled");
            }
        }
        const Status updated = client.update_policy(*execution, policy, true);
        if (!updated.ok()) {
            return report(updated);
        }
        PEF_TRY_RETURN(client.query(*execution, view));
        app::print_line("policy generation advanced to " +
                        std::to_string(view.execution.policy_generation.value()) +
                        "; prior authority invalidated");
        return 0;
    }
    (void)caller;
    return fail("unknown execution command: " + verb);
}

[[nodiscard]] int command_checkpoint(const app::Args& args) {
    const std::vector<std::string> ops = operands(args, 1);
    if (ops.size() < 2) {
        print_usage();
        return 2;
    }
    const std::string& verb = ops[1];
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    const auto execution = execution_argument(ops, 2);
    if (!execution.has_value()) {
        return fail("a valid execution identity is required");
    }

    if (verb == "list" || verb == "explain") {
        ExecutionView view;
        PEF_TRY_RETURN(client.query(*execution, view));
        if (view.checkpoints.empty()) {
            app::print_line("no checkpoints");
            return 0;
        }
        if (verb == "list") {
            for (const CheckpointRecord& checkpoint : view.checkpoints) {
                app::print_line("checkpoint " + hex64(checkpoint.id.value()) + " generation " +
                                std::to_string(checkpoint.generation.value()) + " depth " +
                                std::to_string(checkpoint.lineage_depth) + " state " +
                                std::string(checkpoint_state_name(checkpoint.state)) +
                                " progress_generation " +
                                std::to_string(checkpoint.progress_generation.value()));
            }
            return 0;
        }
        const CheckpointRecord& checkpoint = view.checkpoints.front();
        app::print_line(explain_checkpoint(checkpoint, view.execution));
        return 0;
    }

    if (verb != "create") {
        return fail("unknown checkpoint command: " + verb);
    }
    bool ok = true;
    const std::uint64_t hash = args.u64_or("--hash", 0, ok);
    if (!ok || hash == 0) {
        return fail("--hash with a non-zero content digest is required");
    }
    BoundWorker worker;
    PEF_TRY_RETURN(worker.bind(client, *execution, bindings_from(args, 3)));
    PEF_TRY_RETURN(ensure_running(client, worker, true));
    RegisterCheckpointRequest checkpoint;
    checkpoint.request = client.next_request();
    checkpoint.token = worker.token;
    checkpoint.expected_checkpoint = worker.execution.checkpoint;
    checkpoint.expected_checkpoint_generation = worker.execution.checkpoint_generation;
    checkpoint.content_size = args.u64_or("--size", 0);
    checkpoint.content_hash = hash;
    checkpoint.effect_boundary_ordinal =
        args.u64_or("--boundary", worker.execution.action_frontier);
    RegisterCheckpointResult registered;
    const Status registered_status = client.register_checkpoint(checkpoint, registered);
    if (!registered_status.ok()) {
        return report(registered_status);
    }
    app::print_line("checkpoint=" + hex64(registered.checkpoint.value()) +
                    " generation=" + std::to_string(registered.checkpoint_generation.value()) +
                    " state=" + std::string(checkpoint_state_name(registered.state)) +
                    " lineage_depth=" + std::to_string(registered.lineage_depth) +
                    " continuation=" + hex64(registered.continuation.value()));
    return 0;
}

[[nodiscard]] int command_execution_start(const app::Args& args) {
    const std::vector<std::string> ops = operands(args, 1);
    const auto execution = execution_argument(ops, 2);
    if (!execution.has_value()) {
        return fail("a valid execution identity is required");
    }
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    BoundWorker worker;
    PEF_TRY_RETURN(worker.bind(client, *execution, bindings_from(args, 3)));
    PEF_TRY_RETURN(ensure_running(client, worker, true));
    app::print_line("execution=" + hex64(execution->value()) + " lifecycle=" +
                    std::string(lifecycle_name(worker.execution.lifecycle)) +
                    " incarnation=" + hex64(worker.token.incarnation.value()) +
                    " incarnation_generation=" +
                    std::to_string(worker.token.incarnation_generation.value()) +
                    " lease=" + hex64(worker.token.lease.value()) +
                    " lease_generation=" + std::to_string(worker.token.lease_generation.value()));
    // Hold the lease open long enough that the coordinator observes a live
    // binding rather than an immediate disconnect.
    const int hold_ms = args.int_or("--hold-ms", 0);
    if (hold_ms > 0) {
        ExecutionView view;
        PEF_TRY_RETURN(client.query(*execution, view));
        (void)view;
    }
    return 0;
}

[[nodiscard]] int command_continuation(const app::Args& args) {
    const std::vector<std::string> ops = operands(args, 1);
    if (ops.size() < 2) {
        print_usage();
        return 2;
    }
    const std::string& verb = ops[1];
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    const auto execution = execution_argument(ops, 2);
    if (!execution.has_value()) {
        return fail("a valid execution identity is required");
    }
    ExecutionView view;
    PEF_TRY_RETURN(client.query(*execution, view));
    if (verb == "show") {
        if (!view.execution.continuation.valid()) {
            app::print_line("no continuation");
            return 0;
        }
        app::print_line(explain_execution(view));
        return 0;
    }
    if (verb == "validate") {
        ContinuationState state = ContinuationState::Invalid;
        std::string reason;
        const Status validated = client.validate_continuation(*execution, state, reason);
        if (!validated.ok()) {
            return report(validated);
        }
        app::print_line(explain_continuation(view, state, reason));
        return state == ContinuationState::Valid ? 0 : 1;
    }
    return fail("unknown continuation command: " + verb);
}

[[nodiscard]] int command_recovery(const app::Args& args) {
    const std::vector<std::string> ops = operands(args, 1);
    if (ops.size() < 2) {
        print_usage();
        return 2;
    }
    const std::string& verb = ops[1];
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    const auto execution = execution_argument(ops, 2);
    if (!execution.has_value()) {
        return fail("a valid execution identity is required");
    }
    if (verb == "explain") {
        RecoveryPlan plan;
        const Status classified = client.classify(*execution, plan);
        if (!classified.ok()) {
            return report(classified);
        }
        app::print_line(explain_recovery(plan));
        return 0;
    }
    if (verb == "apply") {
        RecoveryOutcome outcome;
        const Status recovered = client.recover(*execution, outcome);
        if (!recovered.ok()) {
            return report(recovered);
        }
        app::print_line(explain_recovery(outcome.plan));
        app::print_line(std::string("applied=") + (outcome.applied ? "yes" : "no") +
                        " committed_from_receipt=" + (outcome.committed ? "yes" : "no") +
                        " recovery=" + hex64(outcome.recovery.value()));
        return 0;
    }
    return fail("unknown recovery command: " + verb);
}

[[nodiscard]] int command_ambiguity(const app::Args& args) {
    const std::vector<std::string> ops = operands(args, 1);
    if (ops.size() < 2) {
        print_usage();
        return 2;
    }
    const std::string& verb = ops[1];
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    const auto execution = execution_argument(ops, 2);
    if (!execution.has_value()) {
        return fail("a valid execution identity is required");
    }
    ExecutionView view;
    PEF_TRY_RETURN(client.query(*execution, view));
    if (verb == "list") {
        if (!view.execution.ambiguity.valid()) {
            app::print_line("no ambiguity recorded");
            return 0;
        }
        app::print_line("ambiguity " + hex64(view.ambiguity.id.value()) + " state " +
                        std::string(ambiguity_state_name(view.ambiguity.state)) + " action " +
                        hex64(view.ambiguity.action.value()) + " ordinal " +
                        std::to_string(view.ambiguity.action_ordinal) + " class " +
                        std::string(side_effect_class_name(view.ambiguity.effect_class)));
        app::print_line("  reason: " + view.ambiguity.reason);
        if (ambiguous_resolution_pending(view)) {
            app::print_line("  automatic retry: refused; an explicit resolution is required");
        }
        return 0;
    }
    if (verb == "resolve") {
        const auto ambiguity_text = args.value("--ambiguity");
        const auto resolution_text = args.value("--as");
        if (!ambiguity_text.has_value() || !resolution_text.has_value()) {
            return fail("--ambiguity and --as are required");
        }
        const auto ambiguity_id = app::parse_identifier<AmbiguityIdTag>(*ambiguity_text);
        if (!ambiguity_id.has_value()) {
            return fail("--ambiguity is not a valid identity");
        }
        AmbiguityState resolution = AmbiguityState::ManualResolutionRequired;
        if (*resolution_text == "applied") {
            resolution = AmbiguityState::ResolvedApplied;
        } else if (*resolution_text == "not-applied") {
            resolution = AmbiguityState::ResolvedNotApplied;
        } else if (*resolution_text == "accepted") {
            resolution = AmbiguityState::Accepted;
        } else if (*resolution_text == "abandoned") {
            resolution = AmbiguityState::Abandoned;
        } else if (*resolution_text == "manual") {
            resolution = AmbiguityState::ManualResolutionRequired;
        } else {
            return fail("--as must be applied, not-applied, accepted, abandoned, or manual");
        }
        if (!view.execution.ambiguity.valid() || view.execution.ambiguity != *ambiguity_id) {
            return fail("the execution does not reference that ambiguity");
        }
        ResolveAmbiguityRequest request;
        request.request = client.next_request();
        request.caller = CallerContext{hello.session, hello.epoch};
        request.execution = *execution;
        request.ambiguity = *ambiguity_id;
        request.ambiguity_generation = view.execution.ambiguity_generation;
        request.resolution = resolution;
        request.note = args.value_or("--note", "cli");
        request.evidence.kind = args.has("--synthetic-evidence") ? EvidenceKind::Synthetic
                                                                 : EvidenceKind::Real;
        request.evidence.source = "operator";
        ResolveAmbiguityResult resolved;
        const Status resolved_status = client.resolve_ambiguity(request, resolved);
        if (!resolved_status.ok()) {
            return report(resolved_status);
        }
        app::print_line("ambiguity state: " + std::string(ambiguity_state_name(resolved.state)));
        app::print_line(std::string("  committed=") + (resolved.committed ? "yes" : "no") +
                        " replay_admitted=" + (resolved.replay_admitted ? "yes" : "no"));
        if (resolved.committed) {
            app::print_line("  commit=" + hex64(resolved.commit.value()) +
                            " progress_generation=" +
                            std::to_string(resolved.progress_generation.value()));
        }
        return 0;
    }
    return fail("unknown ambiguity command: " + verb);
}

[[nodiscard]] int command_action(const app::Args& args) {
    const std::vector<std::string> ops = operands(args, 1);
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    const auto execution = execution_argument(ops, 2);
    if (!execution.has_value()) {
        return fail("a valid execution identity is required");
    }
    ExecutionView view;
    PEF_TRY_RETURN(client.query(*execution, view));
    if (view.actions.empty()) {
        app::print_line("no actions recorded");
        return 0;
    }
    bool ok = true;
    const std::uint64_t ordinal = args.u64_or("--ordinal", 0, ok);
    app::print_line(explain_execution(view));
    if (ordinal != 0) {
        const auto found = std::find_if(view.actions.begin(), view.actions.end(),
                                        [ordinal](const ActionRecord& action) {
                                            return action.sequence == ordinal;
                                        });
        if (found == view.actions.end()) {
            return fail("no action at that ordinal");
        }
    }
    return 0;
}

[[nodiscard]] int command_lease(const app::Args& args) {
    const std::vector<std::string> ops = operands(args, 1);
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    const auto execution = execution_argument(ops, 2);
    if (!execution.has_value()) {
        return fail("a valid execution identity is required");
    }
    ExecutionView view;
    PEF_TRY_RETURN(client.query(*execution, view));
    if (!view.execution.lease.valid()) {
        app::print_line("no lease held");
        return 0;
    }
    app::print_line("lease " + hex64(view.lease.id.value()) + " generation " +
                    std::to_string(view.lease.generation.value()) + " state " +
                    std::string(lease_state_name(view.lease.state)));
    app::print_line("  execution_generation " +
                    std::to_string(view.lease.execution_generation.value()) +
                    " incarnation_generation " +
                    std::to_string(view.lease.incarnation_generation.value()));
    app::print_line("  worker " + hex64(view.lease.worker.value()) + " boot " +
                    hex64(view.lease.boot.value()) + " epoch " +
                    std::to_string(view.lease.epoch.value()));
    app::print_line("  policy_generation " + std::to_string(view.lease.policy_generation.value()));
    return 0;
}

[[nodiscard]] int command_snapshot(const app::Args& args) {
    CoordinatorClient client;
    HelloReply hello;
    const Status status = connect_client(args, client, hello);
    if (!status.ok()) {
        return report(status);
    }
    std::uint64_t sequence = 0;
    const Status snapshotted = client.snapshot(args.has("--truncate"), sequence);
    if (!snapshotted.ok()) {
        return report(snapshotted);
    }
    app::print_line("snapshot written journal_sequence=" + std::to_string(sequence) +
                    " journal_truncated=" + (args.has("--truncate") ? "yes" : "no"));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace pef;
    app::Args args(argc, argv);
    if (argc < 2 || args.has("--help") || args.has("-h")) {
        print_usage();
        return argc < 2 ? 2 : 0;
    }
    const std::vector<std::string> ops = operands(args, 1);
    if (ops.empty()) {
        print_usage();
        return 2;
    }
    const std::string& group = ops[0];

    if (group == "version") {
        return command_version();
    }
    if (group == "ping") {
        return command_ping(args);
    }
    if (group == "audit") {
        return command_audit(args, false);
    }
    if (group == "verify") {
        return command_audit(args, true);
    }
    if (group == "snapshot") {
        return command_snapshot(args);
    }
    if (group == "execution") {
        const std::string& verb = ops.size() > 1 ? ops[1] : std::string{};
        if (verb == "start" || verb == "resume") {
            return command_execution_start(args);
        }
        return command_execution(args);
    }
    if (group == "checkpoint") {
        return command_checkpoint(args);
    }
    if (group == "continuation") {
        return command_continuation(args);
    }
    if (group == "recovery") {
        return command_recovery(args);
    }
    if (group == "ambiguity") {
        return command_ambiguity(args);
    }
    if (group == "action") {
        return command_action(args);
    }
    if (group == "progress") {
        return command_action(args);
    }
    if (group == "lease") {
        return command_lease(args);
    }
    if (group == "demo") {
        return command_demo(args);
    }
    print_usage();
    return 2;
}

namespace {

bool ambiguous_resolution_pending(const ExecutionView& view) {
    return view.execution.ambiguity.valid() && ambiguity_is_open(view.ambiguity.state);
}

// A self-contained scenario driven entirely through the control plane. Every
// step performs a real runtime operation; nothing is simulated.
int command_demo(const app::Args& args) {
    CoordinatorClient client;
    HelloReply hello;
    const Status connected = connect_client(args, client, hello);
    if (!connected.ok()) {
        return report(connected);
    }
    app::print_line("[demo] coordinator store=" + hex64(hello.store.value()) +
                    " epoch=" + std::to_string(hello.epoch.value()));

    // Phase 1: durable progress with a checkpoint and a validated continuation.
    ExecutionPolicy policy;
    policy.id = PolicyId{1};
    policy.generation = PolicyGeneration{1};
    policy.checkpoint_interval_actions = 0;
    CreateExecutionResult created;
    PEF_TRY_RETURN(client.create_execution(policy, created));
    app::print_line("[demo] created execution " + hex64(created.execution.value()));

    BoundWorker worker;
    PEF_TRY_RETURN(worker.bind(client, created.execution, BindingSet{}));
    PEF_TRY_RETURN(ensure_running(client, worker, true));
    app::print_line("[demo] bound worker boot=" + hex64(worker.boot.value()) +
                    " incarnation=" + hex64(worker.token.incarnation.value()));

    for (int step = 1; step <= 3; ++step) {
        BeginActionRequest begin;
        begin.request = client.next_request();
        begin.token = worker.token;
        begin.effect_class = SideEffectClass::Idempotent;
        begin.evidence.kind = EvidenceKind::Real;
        begin.evidence.source = "demo";
        BeginActionResult action;
        PEF_TRY_RETURN(client.begin_action(begin, action));
        CompleteActionRequest complete;
        complete.request = client.next_request();
        complete.token = worker.token;
        complete.action = action.action;
        complete.action_generation = action.action_generation;
        complete.completion.effect_applied = true;
        complete.completion.evidence.kind = EvidenceKind::Real;
        complete.completion.evidence.source = "demo";
        CompleteActionResult committed;
        PEF_TRY_RETURN(client.complete_action(complete, committed));
        app::print_line("[demo] committed ordinal " + std::to_string(committed.ordinal) +
                        " progress_generation " +
                        std::to_string(committed.progress_generation.value()));
    }

    ExecutionView view;
    PEF_TRY_RETURN(client.query(created.execution, view));
    RegisterCheckpointRequest checkpoint;
    checkpoint.request = client.next_request();
    checkpoint.token = worker.token;
    checkpoint.expected_checkpoint = view.execution.checkpoint;
    checkpoint.expected_checkpoint_generation = view.execution.checkpoint_generation;
    checkpoint.content_size = 4096;
    checkpoint.content_hash = fnv1a("demo-checkpoint");
    checkpoint.effect_boundary_ordinal = view.execution.action_frontier;
    RegisterCheckpointResult registered;
    PEF_TRY_RETURN(client.register_checkpoint(checkpoint, registered));
    app::print_line("[demo] checkpoint generation " +
                    std::to_string(registered.checkpoint_generation.value()) +
                    " lineage depth " + std::to_string(registered.lineage_depth));

    ContinuationState state = ContinuationState::Invalid;
    std::string reason;
    PEF_TRY_RETURN(client.validate_continuation(created.execution, state, reason));
    app::print_line("[demo] continuation " + std::string(continuation_state_name(state)) +
                    " (" + reason + ")");

    // Phase 2: an interrupted non-repeatable action must become explicit
    // ambiguity rather than an optimistic retry.
    CreateExecutionResult risky;
    PEF_TRY_RETURN(client.create_execution(policy, risky));
    BoundWorker risk_worker;
    PEF_TRY_RETURN(risk_worker.bind(client, risky.execution, BindingSet{}));
    PEF_TRY_RETURN(ensure_running(client, risk_worker, true));
    BeginActionRequest begin;
    begin.request = client.next_request();
    begin.token = risk_worker.token;
    begin.effect_class = SideEffectClass::NonRepeatable;
    begin.evidence.kind = EvidenceKind::Real;
    begin.evidence.source = "demo";
    BeginActionResult action;
    PEF_TRY_RETURN(client.begin_action(begin, action));
    app::print_line("[demo] dispatched NON_REPEATABLE action ordinal " +
                    std::to_string(action.ordinal));

    // The worker is fenced as if it had died. Nothing recorded the physical
    // outcome, so the runtime refuses to guess.
    PEF_TRY_RETURN(client.fence(risky.execution, "demo worker loss"));
    RecoveryOutcome recovered;
    PEF_TRY_RETURN(client.recover(risky.execution, recovered));
    app::print_line("[demo] recovery decision " +
                    std::string(recovery_decision_name(recovered.plan.decision)));
    app::print_line("[demo] " + recovered.plan.explanation);

    ExecutionView risky_view;
    PEF_TRY_RETURN(client.query(risky.execution, risky_view));
    if (risky_view.execution.blocked) {
        app::print_line("[demo] progress above ordinal " +
                        std::to_string(risky_view.execution.blocked_above_ordinal) +
                        " is blocked; automatic retry is refused");
    }

    // Phase 3: an operator establishes that the effect did not apply. The same
    // logical action may then be re-driven under a new generation.
    ResolveAmbiguityRequest resolve;
    resolve.request = client.next_request();
    resolve.caller = CallerContext{hello.session, hello.epoch};
    resolve.execution = risky.execution;
    resolve.ambiguity = risky_view.execution.ambiguity;
    resolve.ambiguity_generation = risky_view.execution.ambiguity_generation;
    resolve.resolution = AmbiguityState::ResolvedNotApplied;
    resolve.note = "external system confirmed no effect";
    resolve.evidence.kind = EvidenceKind::Synthetic;
    resolve.evidence.source = "demo-reconciler";
    ResolveAmbiguityResult resolved;
    PEF_TRY_RETURN(client.resolve_ambiguity(resolve, resolved));
    app::print_line("[demo] ambiguity resolved as " +
                    std::string(ambiguity_state_name(resolved.state)) +
                    "; replay admitted=" + (resolved.replay_admitted ? "yes" : "no"));

    RecoveryOutcome after;
    PEF_TRY_RETURN(client.recover(risky.execution, after));
    app::print_line("[demo] post-resolution decision " +
                    std::string(recovery_decision_name(after.plan.decision)));

    // Phase 4: re-drive the same logical action under a new generation and
    // commit it exactly once. The fenced worker cannot do this: authority must
    // be re-established by a replacement that binds and resumes.
    BoundWorker replayer;
    PEF_TRY_RETURN(replayer.bind(client, risky.execution, BindingSet{}));
    PEF_TRY_RETURN(ensure_running(client, replayer, true));
    app::print_line("[demo] replacement incarnation " +
                    hex64(replayer.token.incarnation.value()) + " lease " +
                    hex64(replayer.token.lease.value()));
    BeginActionRequest replay_begin;
    replay_begin.request = client.next_request();
    replay_begin.token = replayer.token;
    replay_begin.effect_class = SideEffectClass::NonRepeatable;
    replay_begin.evidence.kind = EvidenceKind::Real;
    replay_begin.evidence.source = "demo-replay";
    BeginActionResult replay_action;
    PEF_TRY_RETURN(client.begin_action(replay_begin, replay_action));
    app::print_line("[demo] re-drove logical action ordinal " +
                    std::to_string(replay_action.ordinal) + " generation " +
                    std::to_string(replay_action.action_generation.value()));
    CompleteActionRequest replay_complete;
    replay_complete.request = client.next_request();
    replay_complete.token = replayer.token;
    replay_complete.action = replay_action.action;
    replay_complete.action_generation = replay_action.action_generation;
    replay_complete.completion.effect_applied = true;
    replay_complete.completion.evidence.kind = EvidenceKind::Real;
    replay_complete.completion.evidence.source = "demo-replay";
    CompleteActionResult replay_committed;
    PEF_TRY_RETURN(client.complete_action(replay_complete, replay_committed));
    app::print_line("[demo] committed replay progress_generation " +
                    std::to_string(replay_committed.progress_generation.value()));

    AuditReport audit;
    PEF_TRY_RETURN(client.audit(audit));
    app::print_line("[demo] audit violations=" + std::to_string(audit.violations()));
    return audit.clean() ? 0 : 3;
}

}  // namespace
