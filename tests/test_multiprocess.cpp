// Persistent Execution Fabric - real OS-process proof.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Every step in this file starts, observes, kills, or waits on a real process
// and drives the real control plane over real loopback TCP. No thread stands in
// for a process, and no step infers success from the absence of an error.
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "pef/audit.hpp"
#include "pef/client.hpp"
#include "pef/explain.hpp"
#include "process_helper.hpp"
#include "test_framework.hpp"

using namespace pef;

namespace {

constexpr int kReadyTimeoutMs = 30000;
constexpr int kStateTimeoutMs = 15000;

struct Cluster {
    peftest::TempStore dir;
    peftest::ChildProcess coordinator;
    std::vector<peftest::ChildProcess*> children;
    std::uint16_t port = 0;
    std::string coordinator_stdout;
    bool started = false;

    explicit Cluster(const char* label) : dir(label) {}

    [[nodiscard]] std::filesystem::path coordinator_exe() const {
        return std::filesystem::path(std::string(PEF_COORDINATOR_EXE));
    }
    [[nodiscard]] std::filesystem::path worker_exe() const {
        return std::filesystem::path(std::string(PEF_WORKER_EXE));
    }

    bool start_coordinator(peftest::CaseContext& ctx, bool create, const std::string& label) {
        std::vector<std::string> arguments = {"--store", dir.path_string(),
                                              "--port", "0",
                                              "--request-cache", "4096"};
        if (create) {
            arguments.push_back("--create");
        }
        std::string error;
        if (!coordinator.spawn(coordinator_exe(), arguments, dir.path(), label, error)) {
            ctx.fail("coordinator spawn failed: " + error);
            return false;
        }
        children.push_back(&coordinator);
        std::string observed;
        if (!coordinator.wait_for_marker("PEF COORDINATOR READY", kReadyTimeoutMs, observed)) {
            ctx.fail("coordinator did not report readiness; output: " + observed +
                     " stderr: " + coordinator.read_stderr());
            return false;
        }
        coordinator_stdout = observed;
        const auto port_text = peftest::field_value(observed, "port");
        if (!port_text.has_value()) {
            ctx.fail("coordinator readiness line carries no port");
            return false;
        }
        port = static_cast<std::uint16_t>(std::stoi(*port_text));
        started = true;
        if (!peftest::wait_for_port("127.0.0.1", port, kReadyTimeoutMs)) {
            ctx.fail("coordinator port never accepted a connection");
            return false;
        }
        return true;
    }

    bool stop_coordinator(peftest::CaseContext& ctx, std::uint32_t expected_exit,
                          const std::string& reason) {
        CoordinatorClient client;
        ClientConfig config;
        config.host = "127.0.0.1";
        config.port = port;
        config.name = "harness";
        if (client.connect(config).ok()) {
            HelloReply hello;
            if (client.hello_as_operator(hello).ok()) {
                (void)client.request_shutdown();
            }
        }
        const auto code = coordinator.wait_for_exit(kStateTimeoutMs);
        if (!code.has_value()) {
            ctx.fail("coordinator did not exit after " + reason);
            coordinator.kill();
            (void)coordinator.wait_for_exit(5000);
            return false;
        }
        if (*code != expected_exit) {
            ctx.fail("coordinator exit code " + std::to_string(*code) + " after " + reason +
                     "; stderr: " + coordinator.read_stderr());
            return false;
        }
        started = false;
        return true;
    }

    [[nodiscard]] bool connect(CoordinatorClient& client, const std::string& name) const {
        ClientConfig config;
        config.host = "127.0.0.1";
        config.port = port;
        config.name = name;
        return client.connect(config).ok();
    }

    [[nodiscard]] bool connect_operator(CoordinatorClient& client, HelloReply& hello,
                                        const std::string& name) const {
        if (!connect(client, name)) {
            return false;
        }
        return client.hello_as_operator(hello).ok();
    }

    bool spawn_worker(peftest::CaseContext& ctx, const std::string& label,
                      const std::vector<std::string>& extra, peftest::ChildProcess& out) {
        std::vector<std::string> arguments = {"--host", "127.0.0.1",
                                              "--port", std::to_string(port)};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        std::string error;
        if (!out.spawn(worker_exe(), arguments, dir.path(), label, error)) {
            ctx.fail("worker spawn failed: " + error);
            return false;
        }
        children.push_back(&out);
        return true;
    }

    void stop_all() {
        for (auto* child : children) {
            if (child->alive()) {
                child->kill();
                (void)child->wait_for_exit(5000);
            }
        }
    }
};

[[nodiscard]] std::string hex_of(ExecutionId id) { return hex64(id.value()); }

// Waits until the execution satisfies a predicate, polling the real
// coordinator. The bound exists so a hang is reported rather than hanging the
// suite; the condition itself is a real state observation.
template <class Predicate>
bool wait_for_state(CoordinatorClient& client, ExecutionId execution, int timeout_ms,
                    Predicate predicate, ExecutionView& out) {
    for (int elapsed = 0; elapsed <= timeout_ms; elapsed += 20) {
        ExecutionView view;
        if (client.query(execution, view).ok() && predicate(view)) {
            out = view;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    (void)client.query(execution, out);
    return false;
}

struct TokenBuilder {
    CoordinatorClient* client = nullptr;
    WorkerId worker;
    WorkerBootId boot;

    [[nodiscard]] bool bind(ExecutionId execution, BindingSet bindings, LeaseToken& token,
                            BindWorkerResult& bound) {
        BindWorkerRequest request;
        request.request = client->next_request();
        request.execution = execution;
        request.worker = worker;
        request.boot = boot;
        request.bindings = bindings;
        if (!client->bind_worker(request, bound).ok()) {
            return false;
        }
        ExecutionView view;
        if (!client->query(execution, view).ok()) {
            return false;
        }
        token.execution = execution;
        token.execution_generation = view.execution.generation;
        token.incarnation = bound.incarnation;
        token.incarnation_generation = bound.incarnation_generation;
        token.lease = bound.lease;
        token.lease_generation = bound.lease_generation;
        token.epoch = client->epoch();
        token.worker = worker;
        token.boot = boot;
        token.policy_generation = view.execution.policy_generation;
        return true;
    }
};

// A commit count that disagrees with the committed action counter would mean a
// logical commit was recorded twice for one logical action.
[[nodiscard]] bool msg_duplicate_free(const ExecutionView& view) {
    return view.commit_count == view.execution.committed_actions;
}

[[nodiscard]] ExecutionPolicy proof_policy() {
    ExecutionPolicy policy;
    policy.id = PolicyId{1};
    policy.generation = PolicyGeneration{1};
    policy.checkpoint_interval_actions = 0;
    return policy;
}

}  // namespace

// ---------------------------------------------------------------------------
// The full durable lifecycle proof.
// ---------------------------------------------------------------------------
PEF_TEST(multiprocess, durable_lifecycle_proof) {
    Cluster cluster("mp-proof");

    PEF_PHASE(ctx, SETUP);
    PEF_REQUIRE(ctx, cluster.start_coordinator(ctx, true, "coordinator-1"));
    CoordinatorClient client;
    HelloReply hello;
    PEF_REQUIRE(ctx, cluster.connect_operator(client, hello, "harness"));
    const CoordinatorEpoch first_epoch = hello.epoch;
    PEF_CHECK(ctx, first_epoch.value() >= 1);

    PEF_PHASE(ctx, CREATE);
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, client.create_execution(proof_policy(), created).ok());
    PEF_CHECK(ctx, created.execution.valid());

    PEF_PHASE(ctx, BIND);
    const std::string worker_a = "worker-a";
    std::vector<std::string> worker_a_args = {
        "--name",     worker_a,
        "--execution", hex_of(created.execution),
        "--scenario", "progress",
        "--steps",    "3",
        "--checkpoint-every", "3",
        "--hold-ms",  "-1"};
    peftest::ChildProcess process_a;
    PEF_REQUIRE(ctx, cluster.spawn_worker(ctx, "worker-a", worker_a_args, process_a));
    std::string observed;
    PEF_REQUIRE(ctx, process_a.wait_for_marker("WORKER HOLDING", kReadyTimeoutMs, observed));
    PEF_CHECK_MSG(ctx, observed.find("WORKER BOUND") != std::string::npos,
                  "worker did not report a bind: " + observed);
    PEF_CHECK_MSG(ctx, observed.find("WORKER CHECKPOINT") != std::string::npos,
                  "worker did not report a checkpoint: " + observed);

    PEF_PHASE(ctx, CHECKPOINT);
    ExecutionView view;
    PEF_REQUIRE(ctx, wait_for_state(
                         client, created.execution, kStateTimeoutMs,
                         [](const ExecutionView& v) {
                             return v.execution.progress_generation.value() == 3 &&
                                    v.execution.checkpoint.valid();
                         },
                         view));
    PEF_CHECK(ctx, view.execution.lifecycle == Lifecycle::Running);
    PEF_CHECK(ctx, view.execution.committed_actions == 3);
    PEF_CHECK(ctx, view.execution.action_frontier == 3);
    PEF_CHECK(ctx, view.execution.worker.valid());
    PEF_CHECK(ctx, view.checkpoint.state == CheckpointState::Verified ||
                       view.checkpoint.state == CheckpointState::Current);
    ContinuationState continuation_state = ContinuationState::Invalid;
    std::string continuation_reason;
    PEF_REQUIRE(ctx, client
                         .validate_continuation(created.execution, continuation_state,
                                                continuation_reason)
                         .ok());
    PEF_CHECK_MSG(ctx, continuation_state == ContinuationState::Valid, continuation_reason);

    PEF_PHASE(ctx, KILL);
    const WorkerBootId boot_a = view.execution.boot;
    process_a.kill();
    const auto exit_a = process_a.wait_for_exit(kStateTimeoutMs);
    PEF_REQUIRE(ctx, exit_a.has_value());
    PEF_CHECK_MSG(ctx, *exit_a != 0, "a killed worker must not report a clean exit");

    PEF_PHASE(ctx, FENCE);
    PEF_REQUIRE(ctx, wait_for_state(
                         client, created.execution, kStateTimeoutMs,
                         [](const ExecutionView& v) {
                             return !v.execution.lease.valid() &&
                                    v.execution.lifecycle != Lifecycle::Running;
                         },
                         view));
    PEF_CHECK(ctx, view.execution.lifecycle == Lifecycle::Fenced);
    PEF_CHECK(ctx, !view.execution.worker.valid());
    PEF_CHECK(ctx, !view.execution.lease.valid());
    PEF_CHECK_MSG(ctx, view.execution.progress_generation.value() == 3,
                  "fencing must not change authoritative progress");
    // Exactly-once logical progress: three actions produced exactly three
    // durable commits and no more.
    PEF_CHECK(ctx, view.commit_count == 3);

    PEF_PHASE(ctx, RECOVER);
    RecoveryOutcome recovered;
    PEF_REQUIRE(ctx, client.recover(created.execution, recovered).ok());
    PEF_CHECK_MSG(ctx, recovery_allows_automatic_continue(recovered.plan.decision),
                  std::string("recovery refused to continue: ") +
                      std::string(recovery_decision_name(recovered.plan.decision)) + " " +
                      recovered.plan.explanation);

    PEF_PHASE(ctx, RESUME);
    std::vector<std::string> worker_a_prime_args = {
        "--name",      "worker-a-prime",
        "--execution", hex_of(created.execution),
        "--scenario",  "progress",
        "--steps",     "2",
        "--checkpoint-every", "0"};
    peftest::ChildProcess process_a_prime;
    PEF_REQUIRE(ctx, cluster.spawn_worker(ctx, "worker-a-prime", worker_a_prime_args,
                                          process_a_prime));
    PEF_REQUIRE(ctx, process_a_prime.wait_for_marker("WORKER DONE", kReadyTimeoutMs, observed));
    PEF_CHECK_MSG(ctx, observed.find("WORKER BOUND") != std::string::npos,
                  "replacement worker did not bind: " + observed);
    const auto ended = process_a_prime.wait_for_exit(kStateTimeoutMs);
    PEF_REQUIRE(ctx, ended.has_value());
    PEF_CHECK(ctx, *ended == 0);

    PEF_PHASE(ctx, VERIFY);
    PEF_REQUIRE(ctx, wait_for_state(
                         client, created.execution, kStateTimeoutMs,
                         [](const ExecutionView& v) {
                             return v.execution.progress_generation.value() == 5;
                         },
                         view));
    PEF_CHECK_MSG(ctx, view.execution.progress_generation.value() == 5,
                  "progress did not continue exactly to 5 after replacement");
    PEF_CHECK_MSG(ctx, view.commit_count == 5, "duplicate logical commits were recorded");
    PEF_CHECK_MSG(ctx, view.execution.incarnation_generation.value() >= 2,
                  "the replacement worker did not advance the execution incarnation");
    PEF_CHECK_MSG(ctx, view.execution.boot != boot_a,
                  "the replacement worker reused the previous boot identity");

    // Old worker traffic must be rejected even though the execution is live.
    PEF_PHASE(ctx, FENCE);
    BindWorkerResult stale_bound;
    LeaseToken stale_token;
    TokenBuilder stale_builder{&client, derive_worker_id(worker_a), boot_a};
    PEF_CHECK_MSG(ctx, stale_builder.bind(created.execution, BindingSet{}, stale_token,
                                          stale_bound),
                  "the coordinator refused a bind that the proof expects to succeed");
    // Re-binding the old boot is legal, but the old *lease* is not. Reconstruct
    // the pre-kill token and confirm the coordinator refuses it.
    LeaseToken revoked = stale_token;
    revoked.lease_generation = LeaseGeneration{revoked.lease_generation.value() - 1};
    BeginActionRequest stale_begin;
    stale_begin.request = client.next_request();
    stale_begin.token = revoked;
    stale_begin.effect_class = SideEffectClass::Pure;
    BeginActionResult stale_action;
    const Status stale_status = client.begin_action(stale_begin, stale_action);
    PEF_CHECK_MSG(ctx, !stale_status.ok(),
                  "a superseded lease generation was accepted for new work");

    PEF_PHASE(ctx, AUDIT);
    AuditReport audit;
    PEF_REQUIRE(ctx, client.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());

    PEF_PHASE(ctx, RESTART);
    PEF_REQUIRE(ctx, cluster.stop_coordinator(ctx, 0, "clean shutdown"));
    PEF_REQUIRE(ctx, cluster.start_coordinator(ctx, false, "coordinator-2"));
    CoordinatorClient restarted;
    HelloReply restarted_hello;
    PEF_REQUIRE(ctx, cluster.connect_operator(restarted, restarted_hello, "harness-2"));
    PEF_CHECK_MSG(ctx, restarted_hello.epoch.value() > first_epoch.value(),
                  "coordinator restart did not advance the epoch");
    PEF_CHECK_MSG(ctx, restarted_hello.store == hello.store,
                  "execution identity domain changed across restart");

    PEF_PHASE(ctx, RECOVER);
    ExecutionView recovered_view;
    PEF_REQUIRE(ctx, restarted.query(created.execution, recovered_view).ok());
    PEF_CHECK_MSG(ctx, recovered_view.execution.id == created.execution,
                  "the execution identity did not survive the restart");
    PEF_CHECK_MSG(ctx, recovered_view.execution.progress_generation.value() == 5,
                  "authoritative progress did not survive the restart");
    PEF_CHECK_MSG(ctx, !recovered_view.execution.worker.valid(),
                  "process-local worker authority was restored from durable state");
    PEF_CHECK_MSG(ctx, !recovered_view.execution.lease.valid(),
                  "a pre-restart lease was restored");

    // A token built before the restart must be refused on the new epoch.
    LeaseToken old_token = stale_token;
    old_token.epoch = first_epoch;
    BeginActionRequest old_epoch_begin;
    old_epoch_begin.request = restarted.next_request();
    old_epoch_begin.token = old_token;
    old_epoch_begin.effect_class = SideEffectClass::Pure;
    BeginActionResult old_epoch_action;
    const Status old_epoch_status = restarted.begin_action(old_epoch_begin, old_epoch_action);
    PEF_CHECK_MSG(ctx, !old_epoch_status.ok(), "a pre-restart token was accepted");
    PEF_CHECK(ctx, old_epoch_status.code() == Code::StaleEpoch ||
                       old_epoch_status.code() == Code::StaleLease ||
                       old_epoch_status.code() == Code::AuthorityRevoked ||
                       old_epoch_status.code() == Code::StaleWorkerBoot);

    PEF_PHASE(ctx, RESUME);
    RecoveryOutcome post_restart;
    PEF_REQUIRE(ctx, restarted.recover(created.execution, post_restart).ok());
    PEF_CHECK(ctx, post_restart.applied);
    std::vector<std::string> final_args = {"--name",      "worker-final",
                                           "--execution", hex_of(created.execution),
                                           "--scenario",  "progress",
                                           "--steps",     "1",
                                           "--checkpoint-every", "0"};
    peftest::ChildProcess final_worker;
    PEF_REQUIRE(ctx, cluster.spawn_worker(ctx, "worker-final", final_args, final_worker));
    PEF_REQUIRE(ctx, final_worker.wait_for_marker("WORKER DONE", kReadyTimeoutMs, observed));
    PEF_REQUIRE(ctx, final_worker.wait_for_exit(kStateTimeoutMs).has_value());
    ExecutionView final_view;
    PEF_REQUIRE(ctx, wait_for_state(
                         restarted, created.execution, kStateTimeoutMs,
                         [](const ExecutionView& v) {
                             return v.execution.progress_generation.value() == 6;
                         },
                         final_view));
    PEF_CHECK(ctx, final_view.commit_count == 6);

    PEF_PHASE(ctx, AUDIT);
    AuditReport final_audit;
    PEF_REQUIRE(ctx, restarted.audit(final_audit).ok());
    PEF_CHECK_MSG(ctx, final_audit.clean(), final_audit.render());

    PEF_PHASE(ctx, SHUTDOWN);
    PEF_REQUIRE(ctx, cluster.stop_coordinator(ctx, 0, "final shutdown"));
    for (auto* child : cluster.children) {
        PEF_CHECK_MSG(ctx, !child->alive(), "a child process leaked past the proof");
    }
    cluster.stop_all();
}
// ---------------------------------------------------------------------------
// An interrupted non-repeatable action becomes explicit ambiguity, and no
// progress above it may commit until it is resolved.
// ---------------------------------------------------------------------------
PEF_TEST(multiprocess, non_repeatable_interruption_is_ambiguous_and_blocked) {
    Cluster cluster("mp-ambiguity");
    PEF_PHASE(ctx, SETUP);
    PEF_REQUIRE(ctx, cluster.start_coordinator(ctx, true, "coordinator"));
    CoordinatorClient client;
    HelloReply hello;
    PEF_REQUIRE(ctx, cluster.connect_operator(client, hello, "harness"));

    PEF_PHASE(ctx, CREATE);
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, client.create_execution(proof_policy(), created).ok());

    PEF_PHASE(ctx, ACTION);
    std::vector<std::string> arguments = {"--name",      "worker-nonrepeatable",
                                          "--execution", hex_of(created.execution),
                                          "--scenario",  "crash-in-flight",
                                          "--effect-class", "NON_REPEATABLE"};
    peftest::ChildProcess worker;
    PEF_REQUIRE(ctx, cluster.spawn_worker(ctx, "worker-nonrepeatable", arguments, worker));
    std::string observed;
    PEF_REQUIRE(ctx, worker.wait_for_marker("WORKER CRASHING", kReadyTimeoutMs, observed));
    const auto code = worker.wait_for_exit(kStateTimeoutMs);
    PEF_REQUIRE(ctx, code.has_value());
    PEF_CHECK(ctx, *code == 3);

    PEF_PHASE(ctx, KILL);
    ExecutionView view;
    PEF_REQUIRE(ctx, wait_for_state(
                         client, created.execution, kStateTimeoutMs,
                         [](const ExecutionView& v) { return !v.execution.lease.valid(); }, view));

    PEF_PHASE(ctx, AMBIGUITY);
    RecoveryOutcome recovered;
    PEF_REQUIRE(ctx, client.recover(created.execution, recovered).ok());
    PEF_CHECK_MSG(ctx, recovered.plan.decision == RecoveryDecision::AmbiguousCompletion,
                  std::string("unexpected decision ") +
                      std::string(recovery_decision_name(recovered.plan.decision)));
    PEF_REQUIRE(ctx, client.query(created.execution, view).ok());
    PEF_CHECK(ctx, view.execution.blocked);
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 0);
    PEF_CHECK(ctx, view.ambiguity.state == AmbiguityState::Open);
    PEF_CHECK_MSG(ctx,
                  view.actions.size() == 1 &&
                      view.actions[0].status == ActionStatus::Ambiguous,
                  "the interrupted action was not sealed as ambiguous");

    PEF_PHASE(ctx, COMMIT);
    RecoveryOutcome again;
    PEF_REQUIRE(ctx, client.recover(created.execution, again).ok());
    PEF_REQUIRE(ctx, client.query(created.execution, view).ok());
    PEF_CHECK_MSG(ctx, view.execution.progress_generation.value() == 0,
                  "an unresolved ambiguity allowed progress to advance");

    PEF_PHASE(ctx, VERIFY);
    AuditReport audit;
    PEF_REQUIRE(ctx, client.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());

    PEF_PHASE(ctx, SHUTDOWN);
    PEF_REQUIRE(ctx, cluster.stop_coordinator(ctx, 0, "shutdown"));
    PEF_CHECK(ctx, !worker.alive());
    cluster.stop_all();
}

// ---------------------------------------------------------------------------
// A durable external receipt that states the effect applied removes the
// ambiguity: the coordinator commits logical progress with no worker and no
// replay.
// ---------------------------------------------------------------------------
PEF_TEST(multiprocess, durable_receipt_commits_without_replay) {
    Cluster cluster("mp-receipt");
    PEF_PHASE(ctx, SETUP);
    PEF_REQUIRE(ctx, cluster.start_coordinator(ctx, true, "coordinator"));
    CoordinatorClient client;
    HelloReply hello;
    PEF_REQUIRE(ctx, cluster.connect_operator(client, hello, "harness"));

    CreateExecutionResult created;
    PEF_REQUIRE(ctx, client.create_execution(proof_policy(), created).ok());

    PEF_PHASE(ctx, ACTION);
    std::vector<std::string> arguments = {"--name",      "worker-receipt",
                                          "--execution", hex_of(created.execution),
                                          "--scenario",  "crash-after-receipt",
                                          "--effect-class", "NON_REPEATABLE",
                                          "--receipt",   "applied"};
    peftest::ChildProcess worker;
    PEF_REQUIRE(ctx, cluster.spawn_worker(ctx, "worker-receipt", arguments, worker));
    std::string observed;
    PEF_REQUIRE(ctx, worker.wait_for_marker("WORKER CRASHING", kReadyTimeoutMs, observed));
    PEF_REQUIRE(ctx, worker.wait_for_exit(kStateTimeoutMs).has_value());

    PEF_PHASE(ctx, AMBIGUITY);
    ExecutionView view;
    PEF_REQUIRE(ctx, wait_for_state(
                         client, created.execution, kStateTimeoutMs,
                         [](const ExecutionView& v) { return !v.execution.lease.valid(); }, view));
    RecoveryOutcome recovered;
    PEF_REQUIRE(ctx, client.recover(created.execution, recovered).ok());
    PEF_CHECK_MSG(ctx, recovered.plan.commit_from_receipt,
                  "a durable applied receipt did not permit a commit");
    PEF_CHECK_MSG(ctx, recovered.committed, "the receipt-driven commit did not happen");

    PEF_PHASE(ctx, COMMIT);
    PEF_REQUIRE(ctx, client.query(created.execution, view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 1);
    PEF_CHECK(ctx, view.execution.committed_actions == 1);
    PEF_CHECK_MSG(ctx, !view.execution.blocked,
                  "a receipt-driven commit left the execution blocked");
    PEF_CHECK_MSG(ctx, msg_duplicate_free(view),
                  "the receipt-driven commit was recorded twice");

    PEF_PHASE(ctx, VERIFY);
    AuditReport audit;
    PEF_REQUIRE(ctx, client.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());

    PEF_PHASE(ctx, SHUTDOWN);
    PEF_REQUIRE(ctx, cluster.stop_coordinator(ctx, 0, "shutdown"));
    cluster.stop_all();
}

// ---------------------------------------------------------------------------
// A receipt that states the effect did not apply makes re-driving the same
// logical action safe.
// ---------------------------------------------------------------------------
PEF_TEST(multiprocess, receipt_stating_no_effect_permits_replay) {
    Cluster cluster("mp-no-effect");
    PEF_PHASE(ctx, SETUP);
    PEF_REQUIRE(ctx, cluster.start_coordinator(ctx, true, "coordinator"));
    CoordinatorClient client;
    HelloReply hello;
    PEF_REQUIRE(ctx, cluster.connect_operator(client, hello, "harness"));

    CreateExecutionResult created;
    PEF_REQUIRE(ctx, client.create_execution(proof_policy(), created).ok());

    PEF_PHASE(ctx, ACTION);
    std::vector<std::string> arguments = {"--name",      "worker-no-effect",
                                          "--execution", hex_of(created.execution),
                                          "--scenario",  "crash-after-receipt",
                                          "--effect-class", "NON_REPEATABLE",
                                          "--receipt",   "not-applied"};
    peftest::ChildProcess worker;
    PEF_REQUIRE(ctx, cluster.spawn_worker(ctx, "worker-no-effect", arguments, worker));
    std::string observed;
    PEF_REQUIRE(ctx, worker.wait_for_marker("WORKER CRASHING", kReadyTimeoutMs, observed));
    PEF_REQUIRE(ctx, worker.wait_for_exit(kStateTimeoutMs).has_value());

    ExecutionView view;
    PEF_REQUIRE(ctx, wait_for_state(
                         client, created.execution, kStateTimeoutMs,
                         [](const ExecutionView& v) { return !v.execution.lease.valid(); }, view));

    PEF_PHASE(ctx, RECOVER);
    RecoveryOutcome recovered;
    PEF_REQUIRE(ctx, client.recover(created.execution, recovered).ok());
    PEF_CHECK_MSG(ctx, recovered.plan.decision == RecoveryDecision::ReplaySafeAction,
                  std::string("unexpected decision ") +
                      std::string(recovery_decision_name(recovered.plan.decision)));
    PEF_CHECK_MSG(ctx, !recovered.committed,
                  "a receipt stating no effect must not commit progress");

    PEF_PHASE(ctx, REPLAY);
    std::vector<std::string> replay_args = {"--name",      "worker-replay",
                                            "--execution", hex_of(created.execution),
                                            "--scenario",  "progress",
                                            "--steps",     "1",
                                            "--effect-class", "NON_REPEATABLE",
                                            "--checkpoint-every", "0"};
    peftest::ChildProcess replayer;
    PEF_REQUIRE(ctx, cluster.spawn_worker(ctx, "worker-replay", replay_args, replayer));
    PEF_REQUIRE(ctx, replayer.wait_for_marker("WORKER DONE", kReadyTimeoutMs, observed));
    PEF_REQUIRE(ctx, replayer.wait_for_exit(kStateTimeoutMs).has_value());

    PEF_PHASE(ctx, VERIFY);
    PEF_REQUIRE(ctx, wait_for_state(
                         client, created.execution, kStateTimeoutMs,
                         [](const ExecutionView& v) {
                             return v.execution.progress_generation.value() == 1;
                         },
                         view));
    PEF_CHECK(ctx, view.commit_count == 1);
    PEF_CHECK_MSG(ctx, view.execution.replay_count >= 1,
                  "the re-driven action was not recorded as a replay");
    PEF_CHECK_MSG(ctx, !view.actions.empty() && view.actions.front().generation.value() >= 2,
                  "the re-driven action did not advance its generation");

    AuditReport audit;
    PEF_REQUIRE(ctx, client.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());

    PEF_PHASE(ctx, SHUTDOWN);
    PEF_REQUIRE(ctx, cluster.stop_coordinator(ctx, 0, "shutdown"));
    cluster.stop_all();
}

// ---------------------------------------------------------------------------
// Process-death matrix: where the worker dies decides what may continue.
// ---------------------------------------------------------------------------
PEF_TEST(multiprocess, process_death_matrix) {
    Cluster cluster("mp-death");
    PEF_PHASE(ctx, SETUP);
    PEF_REQUIRE(ctx, cluster.start_coordinator(ctx, true, "coordinator"));
    CoordinatorClient client;
    HelloReply hello;
    PEF_REQUIRE(ctx, cluster.connect_operator(client, hello, "harness"));

    struct DeathCase {
        const char* label;
        const char* scenario;
        const char* effect_class;
        const char* receipt;
        std::uint64_t steps;
        std::uint64_t checkpoint_every;
        bool hold;
        RecoveryDecision expected;
        std::uint64_t expected_progress;
        bool expected_checkpoint;
    };
    const DeathCase cases[] = {
        {"idle-hold", "progress", "PURE", "none", 2, 0, true,
         RecoveryDecision::ResumeFromCurrent, 2, false},
        {"hold-after-checkpoint", "progress", "PURE", "none", 2, 2, true,
         RecoveryDecision::ResumeFromCurrent, 2, true},
        {"in-flight-pure", "crash-in-flight", "PURE", "none", 0, 0, false,
         RecoveryDecision::ReplaySafeAction, 0, false},
        {"in-flight-idempotent", "crash-in-flight", "IDEMPOTENT", "none", 0, 0, false,
         RecoveryDecision::ReplaySafeAction, 0, false},
        {"in-flight-nonrepeatable", "crash-in-flight", "NON_REPEATABLE", "none", 0, 0, false,
         RecoveryDecision::AmbiguousCompletion, 0, false},
        {"in-flight-unknown", "crash-in-flight", "UNKNOWN", "none", 0, 0, false,
         RecoveryDecision::AmbiguousCompletion, 0, false},
        {"after-effect-nonrepeatable", "crash-after-receipt", "NON_REPEATABLE", "applied", 0, 0,
         false, RecoveryDecision::ResumeFromCurrent, 1, false},
        {"after-effect-idempotent", "crash-after-receipt", "IDEMPOTENT", "applied", 0, 0, false,
         RecoveryDecision::ResumeFromCurrent, 1, false},
        {"after-no-effect", "crash-after-receipt", "NON_REPEATABLE", "not-applied", 0, 0, false,
         RecoveryDecision::ReplaySafeAction, 0, false},
    };

    for (const DeathCase& death : cases) {
        PEF_PHASE(ctx, CREATE);
        CreateExecutionResult created;
        PEF_REQUIRE(ctx, client.create_execution(proof_policy(), created).ok());

        std::vector<std::string> arguments = {"--name", std::string("worker-") + death.label,
                                              "--execution", hex_of(created.execution),
                                              "--scenario", death.scenario,
                                              "--effect-class", death.effect_class,
                                              "--receipt", death.receipt,
                                              "--steps", std::to_string(death.steps),
                                              "--checkpoint-every",
                                              std::to_string(death.checkpoint_every)};
        if (death.hold) {
            arguments.push_back("--hold-ms");
            arguments.push_back("-1");
        }
        peftest::ChildProcess worker;
        PEF_REQUIRE(ctx, cluster.spawn_worker(ctx, std::string("death-") + death.label,
                                              arguments, worker));
        std::string observed;
        const char* marker = death.hold ? "WORKER HOLDING" : "WORKER CRASHING";
        PEF_REQUIRE(ctx, worker.wait_for_marker(marker, kReadyTimeoutMs, observed));

        PEF_PHASE(ctx, KILL);
        worker.kill();
        PEF_REQUIRE(ctx, worker.wait_for_exit(kStateTimeoutMs).has_value());

        ExecutionView view;
        PEF_REQUIRE(ctx,
                    wait_for_state(
                        client, created.execution, kStateTimeoutMs,
                        [](const ExecutionView& v) { return !v.execution.lease.valid(); }, view));

        PEF_PHASE(ctx, RECOVER);
        RecoveryOutcome recovered;
        PEF_REQUIRE(ctx, client.recover(created.execution, recovered).ok());
        PEF_CHECK_MSG(ctx, recovered.plan.decision == death.expected,
                      std::string(death.label) + ": expected " +
                          std::string(recovery_decision_name(death.expected)) + " but got " +
                          std::string(recovery_decision_name(recovered.plan.decision)) + " (" +
                          recovered.plan.explanation + ")");

        PEF_PHASE(ctx, VERIFY);
        PEF_REQUIRE(ctx, client.query(created.execution, view).ok());
        PEF_CHECK_MSG(ctx, view.execution.progress_generation.value() == death.expected_progress,
                      std::string(death.label) + ": unexpected authoritative progress");
        PEF_CHECK_MSG(ctx, view.execution.checkpoint.valid() == death.expected_checkpoint,
                      std::string(death.label) + ": unexpected checkpoint presence");
    }

    PEF_PHASE(ctx, AUDIT);
    AuditReport audit;
    PEF_REQUIRE(ctx, client.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());

    PEF_PHASE(ctx, SHUTDOWN);
    PEF_REQUIRE(ctx, cluster.stop_coordinator(ctx, 0, "shutdown"));
    cluster.stop_all();
}

// ---------------------------------------------------------------------------
// Shutdown while work is pending leaves durable state valid and leaks nothing.
// ---------------------------------------------------------------------------
PEF_TEST(multiprocess, shutdown_while_work_is_pending) {
    Cluster cluster("mp-shutdown");
    PEF_PHASE(ctx, SETUP);
    PEF_REQUIRE(ctx, cluster.start_coordinator(ctx, true, "coordinator"));
    CoordinatorClient client;
    HelloReply hello;
    PEF_REQUIRE(ctx, cluster.connect_operator(client, hello, "harness"));

    CreateExecutionResult created;
    PEF_REQUIRE(ctx, client.create_execution(proof_policy(), created).ok());

    PEF_PHASE(ctx, START);
    std::vector<std::string> arguments = {"--name",      "worker-pending",
                                          "--execution", hex_of(created.execution),
                                          "--scenario",  "progress",
                                          "--steps",     "2",
                                          "--checkpoint-every", "0",
                                          "--hold-ms",   "-1"};
    peftest::ChildProcess worker;
    PEF_REQUIRE(ctx, cluster.spawn_worker(ctx, "worker-pending", arguments, worker));
    std::string observed;
    PEF_REQUIRE(ctx, worker.wait_for_marker("WORKER HOLDING", kReadyTimeoutMs, observed));

    PEF_PHASE(ctx, SHUTDOWN);
    PEF_REQUIRE(ctx, cluster.stop_coordinator(ctx, 0, "shutdown with a live worker"));
    PEF_CHECK_MSG(ctx, worker.alive(),
                  "the worker should still be running when the coordinator stops");
    worker.kill();
    PEF_REQUIRE(ctx, worker.wait_for_exit(kStateTimeoutMs).has_value());

    PEF_PHASE(ctx, RESTART);
    PEF_REQUIRE(ctx, cluster.start_coordinator(ctx, false, "coordinator-again"));
    CoordinatorClient restarted;
    HelloReply restarted_hello;
    PEF_REQUIRE(ctx, cluster.connect_operator(restarted, restarted_hello, "harness-2"));

    PEF_PHASE(ctx, VERIFY);
    ExecutionView view;
    PEF_REQUIRE(ctx, restarted.query(created.execution, view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 2);
    PEF_CHECK(ctx, !view.execution.lease.valid());
    PEF_CHECK(ctx, !view.execution.worker.valid());
    AuditReport audit;
    PEF_REQUIRE(ctx, restarted.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());

    PEF_PHASE(ctx, SHUTDOWN);
    PEF_REQUIRE(ctx, cluster.stop_coordinator(ctx, 0, "final shutdown"));
    for (auto* child : cluster.children) {
        PEF_CHECK_MSG(ctx, !child->alive(), "a child process leaked");
    }
    cluster.stop_all();
}

