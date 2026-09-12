// Persistent Execution Fabric - runtime authority tests.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <string>
#include <vector>

#include "pef/audit.hpp"
#include "pef/runtime.hpp"
#include "test_framework.hpp"

using namespace pef;

namespace {

struct Fixture {
    peftest::TempStore store;
    Runtime runtime;
    CallerContext caller;
    OpenOutcome open_outcome;

    explicit Fixture(const char* label) : store(label) {}

    Status open(bool create = true) {
        RuntimeConfig config;
        config.store_path = store.path();
        config.create_if_missing = create;
        const Status status = runtime.open(config, open_outcome);
        caller.epoch = runtime.epoch();
        return status;
    }
};

[[nodiscard]] ExecutionPolicy test_policy() {
    ExecutionPolicy policy;
    policy.id = PolicyId{1};
    policy.generation = PolicyGeneration{1};
    policy.checkpoint_interval_actions = 0;
    return policy;
}

}  // namespace

PEF_TEST(runtime, create_bind_start_complete_and_audit) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("create-bind");
    PEF_REQUIRE(ctx, fixture.open().ok());

    PEF_PHASE(ctx, CREATE);
    CreateExecutionRequest create;
    create.request = RequestId{1};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());
    PEF_CHECK(ctx, created.execution.valid());
    PEF_CHECK(ctx, created.generation.value() == 1);

    PEF_PHASE(ctx, BIND);
    BindWorkerRequest bind;
    bind.request = RequestId{2};
    bind.execution = created.execution;
    bind.worker = derive_worker_id("worker-a");
    bind.boot = mint_worker_boot_id(bind.worker);
    BindWorkerResult bound;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());
    PEF_CHECK(ctx, bound.lifecycle == Lifecycle::Ready);
    PEF_CHECK(ctx, bound.incarnation_advanced);

    LeaseToken token;
    token.execution = created.execution;
    token.execution_generation = created.generation;
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = fixture.runtime.epoch();
    token.worker = bind.worker;
    token.boot = bind.boot;
    token.policy_generation = created.policy_generation;

    PEF_PHASE(ctx, START);
    StartRequest start;
    start.request = RequestId{3};
    start.token = token;
    PEF_REQUIRE(ctx, fixture.runtime.start(start).ok());

    PEF_PHASE(ctx, ACTION);
    BeginActionRequest begin;
    begin.request = RequestId{4};
    begin.token = token;
    begin.effect_class = SideEffectClass::Pure;
    BeginActionResult action;
    PEF_REQUIRE(ctx, fixture.runtime.begin_action(begin, action).ok());
    PEF_CHECK(ctx, action.ordinal == 1);

    PEF_PHASE(ctx, COMMIT);
    CompleteActionRequest complete;
    complete.request = RequestId{5};
    complete.token = token;
    complete.action = action.action;
    complete.action_generation = action.action_generation;
    complete.completion.effect_applied = true;
    complete.completion.evidence.kind = EvidenceKind::Real;
    complete.completion.evidence.source = "unit-test";
    CompleteActionResult committed;
    PEF_REQUIRE(ctx, fixture.runtime.complete_action(complete, committed).ok());
    PEF_CHECK(ctx, !committed.duplicate);
    PEF_CHECK(ctx, committed.progress_generation.value() == 1);
    PEF_CHECK(ctx, committed.ordinal == 1);
    PEF_CHECK(ctx, committed.commit.valid());

    PEF_PHASE(ctx, VERIFY);
    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(created.execution, view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 1);
    PEF_CHECK(ctx, view.execution.committed_actions == 1);
    PEF_CHECK(ctx, view.execution.action_frontier == 1);
    PEF_CHECK(ctx, view.actions.size() == 1);
    PEF_CHECK(ctx, view.actions[0].status == ActionStatus::Committed);
    PEF_CHECK(ctx, view.commit_count == 1);

    PEF_PHASE(ctx, SHUTDOWN);
    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());
}

PEF_TEST(runtime, duplicate_completion_cannot_advance_progress_twice) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("duplicate-completion");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{10};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());

    BindWorkerRequest bind;
    bind.request = RequestId{11};
    bind.execution = created.execution;
    bind.worker = derive_worker_id("worker-dup");
    bind.boot = mint_worker_boot_id(bind.worker);
    BindWorkerResult bound;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());

    LeaseToken token;
    token.execution = created.execution;
    token.execution_generation = created.generation;
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = fixture.runtime.epoch();
    token.worker = bind.worker;
    token.boot = bind.boot;
    token.policy_generation = created.policy_generation;

    StartRequest start;
    start.request = RequestId{12};
    start.token = token;
    PEF_REQUIRE(ctx, fixture.runtime.start(start).ok());

    BeginActionRequest begin;
    begin.request = RequestId{13};
    begin.token = token;
    begin.effect_class = SideEffectClass::Idempotent;
    BeginActionResult action;
    PEF_REQUIRE(ctx, fixture.runtime.begin_action(begin, action).ok());

    PEF_PHASE(ctx, COMMIT);
    CompleteActionRequest complete;
    complete.request = RequestId{14};
    complete.token = token;
    complete.action = action.action;
    complete.action_generation = action.action_generation;
    CompleteActionResult first;
    PEF_REQUIRE(ctx, fixture.runtime.complete_action(complete, first).ok());
    PEF_CHECK(ctx, !first.duplicate);

    // The same completion arrives again under a different request identity,
    // which is exactly what a retry after a lost acknowledgement looks like.
    complete.request = RequestId{15};
    CompleteActionResult second;
    PEF_REQUIRE(ctx, fixture.runtime.complete_action(complete, second).ok());
    PEF_CHECK(ctx, second.duplicate);
    PEF_CHECK(ctx, second.commit == first.commit);
    PEF_CHECK(ctx, second.progress_generation == first.progress_generation);

    PEF_PHASE(ctx, VERIFY);
    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(created.execution, view).ok());
    PEF_CHECK_MSG(ctx, view.execution.progress_generation.value() == 1,
                  "progress advanced more than once for one logical action");
    PEF_CHECK(ctx, view.commit_count == 1);
    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(runtime, checkpoint_registration_issues_a_valid_continuation) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("checkpoint");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{20};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());

    BindWorkerRequest bind;
    bind.request = RequestId{21};
    bind.execution = created.execution;
    bind.worker = derive_worker_id("worker-checkpoint");
    bind.boot = mint_worker_boot_id(bind.worker);
    BindWorkerResult bound;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());

    LeaseToken token;
    token.execution = created.execution;
    token.execution_generation = created.generation;
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = fixture.runtime.epoch();
    token.worker = bind.worker;
    token.boot = bind.boot;
    token.policy_generation = created.policy_generation;

    StartRequest start;
    start.request = RequestId{22};
    start.token = token;
    PEF_REQUIRE(ctx, fixture.runtime.start(start).ok());

    PEF_PHASE(ctx, CHECKPOINT);
    RegisterCheckpointRequest checkpoint;
    checkpoint.request = RequestId{23};
    checkpoint.token = token;
    checkpoint.expected_checkpoint = CheckpointId{};
    checkpoint.expected_checkpoint_generation = CheckpointGeneration{};
    checkpoint.content_size = 4096;
    checkpoint.content_hash = 0xfeedfacecafebeefULL;
    RegisterCheckpointResult registered;
    PEF_REQUIRE(ctx, fixture.runtime.register_checkpoint(checkpoint, registered).ok());
    PEF_CHECK(ctx, registered.checkpoint.valid());
    PEF_CHECK(ctx, registered.state == CheckpointState::Verified);
    PEF_CHECK(ctx, registered.lineage_depth == 0);
    PEF_CHECK(ctx, registered.continuation.valid());

    PEF_PHASE(ctx, VERIFY);
    ContinuationState state = ContinuationState::Invalid;
    std::string reason;
    PEF_REQUIRE(ctx, fixture.runtime
                         .validate_continuation(fixture.caller, created.execution, state, reason)
                         .ok());
    PEF_CHECK_MSG(ctx, state == ContinuationState::Valid, reason);

    // A second checkpoint must declare the current one as its parent.
    RegisterCheckpointRequest stale = checkpoint;
    stale.request = RequestId{24};
    stale.expected_checkpoint = CheckpointId{};
    stale.expected_checkpoint_generation = CheckpointGeneration{};
    RegisterCheckpointResult second;
    const Status status = fixture.runtime.register_checkpoint(stale, second);
    PEF_CHECK_MSG(ctx, status.code() == Code::StaleCheckpointGeneration, status.to_string());

    RegisterCheckpointRequest lineage = checkpoint;
    lineage.request = RequestId{25};
    lineage.expected_checkpoint = registered.checkpoint;
    lineage.expected_checkpoint_generation = registered.checkpoint_generation;
    lineage.content_hash = 0x1234;
    RegisterCheckpointResult third;
    PEF_REQUIRE(ctx, fixture.runtime.register_checkpoint(lineage, third).ok());
    PEF_CHECK(ctx, third.lineage_depth == 1);

    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(runtime, worker_reincarnation_fences_the_previous_boot) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("reincarnation");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{30};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());

    const WorkerId worker = derive_worker_id("worker-reincarnate");
    BindWorkerRequest first_bind;
    first_bind.request = RequestId{31};
    first_bind.execution = created.execution;
    first_bind.worker = worker;
    first_bind.boot = mint_worker_boot_id(worker);
    BindWorkerResult first;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, first_bind, first).ok());

    LeaseToken old_token;
    old_token.execution = created.execution;
    old_token.execution_generation = created.generation;
    old_token.incarnation = first.incarnation;
    old_token.incarnation_generation = first.incarnation_generation;
    old_token.lease = first.lease;
    old_token.lease_generation = first.lease_generation;
    old_token.epoch = fixture.runtime.epoch();
    old_token.worker = worker;
    old_token.boot = first_bind.boot;
    old_token.policy_generation = created.policy_generation;

    StartRequest start;
    start.request = RequestId{32};
    start.token = old_token;
    PEF_REQUIRE(ctx, fixture.runtime.start(start).ok());

    PEF_PHASE(ctx, KILL);
    // The coordinator detects worker loss when the session ends and fences the
    // boot before any replacement may bind.
    CallerContext worker_caller;
    SessionId session;
    PEF_REQUIRE(ctx, fixture.runtime.begin_session(worker, first_bind.boot, worker_caller, session)
                         .ok());
    PEF_REQUIRE(ctx, fixture.runtime.end_session(session).ok());

    ExecutionView fenced;
    PEF_REQUIRE(ctx, fixture.runtime.query(created.execution, fenced).ok());
    PEF_CHECK(ctx, fenced.execution.lifecycle == Lifecycle::Fenced);
    PEF_CHECK(ctx, !fenced.execution.worker.valid());
    PEF_CHECK(ctx, !fenced.execution.lease.valid());

    PEF_PHASE(ctx, FENCE);
    BeginActionRequest stale_action;
    stale_action.request = RequestId{33};
    stale_action.token = old_token;
    stale_action.effect_class = SideEffectClass::Pure;
    BeginActionResult stale_result;
    const Status stale_status = fixture.runtime.begin_action(stale_action, stale_result);
    PEF_CHECK_MSG(ctx, !stale_status.ok(), "a fenced boot must not be able to start work");
    PEF_CHECK(ctx, stale_status.code() == Code::StaleLease ||
                       stale_status.code() == Code::AuthorityRevoked ||
                       stale_status.code() == Code::LifecycleRefused ||
                       stale_status.code() == Code::StaleIncarnation);

    PEF_PHASE(ctx, RECOVER);
    BindWorkerRequest second_bind;
    second_bind.request = RequestId{34};
    second_bind.execution = created.execution;
    second_bind.worker = worker;
    second_bind.boot = mint_worker_boot_id(worker);
    BindWorkerResult second;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, second_bind, second).ok());
    PEF_CHECK(ctx, second.incarnation_advanced);
    PEF_CHECK(ctx, second.incarnation != first.incarnation);
    PEF_CHECK(ctx, second.lease != first.lease);
    PEF_CHECK(ctx, second.lifecycle == Lifecycle::RecoveryRequired);

    RecoveryOutcome recovered;
    PEF_REQUIRE(ctx, fixture.runtime
                         .recover(fixture.caller, RequestId{35}, created.execution, recovered)
                         .ok());
    PEF_CHECK(ctx, recovered.applied);

    LeaseToken new_token;
    new_token.execution = created.execution;
    new_token.execution_generation = created.generation;
    new_token.incarnation = second.incarnation;
    new_token.incarnation_generation = second.incarnation_generation;
    new_token.lease = second.lease;
    new_token.lease_generation = second.lease_generation;
    new_token.epoch = fixture.runtime.epoch();
    new_token.worker = worker;
    new_token.boot = second_bind.boot;
    new_token.policy_generation = created.policy_generation;

    PEF_PHASE(ctx, RESUME);
    ResumeRequest resume;
    resume.request = RequestId{36};
    resume.token = new_token;
    resume.revalidate_bindings = true;
    ResumeResult resumed;
    PEF_REQUIRE(ctx, fixture.runtime.resume(resume, resumed).ok());
    PEF_CHECK(ctx, resumed.lifecycle == Lifecycle::Running);
    PEF_CHECK(ctx, resumed.continuation_state == ContinuationState::Valid);

    PEF_PHASE(ctx, VERIFY);
    // Old traffic arriving after the replacement must be refused.
    BeginActionRequest late;
    late.request = RequestId{37};
    late.token = old_token;
    late.effect_class = SideEffectClass::Pure;
    BeginActionResult late_result;
    const Status late_status = fixture.runtime.begin_action(late, late_result);
    PEF_CHECK_MSG(ctx, !late_status.ok(), "stale boot traffic must be refused");

    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(runtime, coordinator_restart_advances_epoch_and_refuses_stale_traffic) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("coordinator-restart");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{40};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());

    const WorkerId worker = derive_worker_id("worker-restart");
    BindWorkerRequest bind;
    bind.request = RequestId{41};
    bind.execution = created.execution;
    bind.worker = worker;
    bind.boot = mint_worker_boot_id(worker);
    BindWorkerResult bound;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());

    LeaseToken token;
    token.execution = created.execution;
    token.execution_generation = created.generation;
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = fixture.runtime.epoch();
    token.worker = worker;
    token.boot = bind.boot;
    token.policy_generation = created.policy_generation;

    StartRequest start;
    start.request = RequestId{42};
    start.token = token;
    PEF_REQUIRE(ctx, fixture.runtime.start(start).ok());
    const CoordinatorEpoch first_epoch = fixture.runtime.epoch();

    PEF_PHASE(ctx, PERSIST);
    PEF_REQUIRE(ctx, fixture.runtime.take_snapshot(true).ok());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());

    PEF_PHASE(ctx, RESTART);
    Runtime restarted;
    RuntimeConfig config;
    config.store_path = fixture.store.path();
    config.create_if_missing = false;
    OpenOutcome outcome;
    PEF_REQUIRE(ctx, restarted.open(config, outcome).ok());
    PEF_CHECK(ctx, outcome.epoch.value() == first_epoch.value() + 1);
    PEF_CHECK(ctx, outcome.leases_revoked >= 1);
    PEF_CHECK(ctx, outcome.executions_reclassified >= 1);

    PEF_PHASE(ctx, VERIFY);
    ExecutionView view;
    PEF_REQUIRE(ctx, restarted.query(created.execution, view).ok());
    PEF_CHECK(ctx, view.execution.lifecycle == Lifecycle::RecoveryRequired ||
                       view.execution.lifecycle == Lifecycle::Recovering);
    PEF_CHECK_MSG(ctx, !view.execution.worker.valid(),
                  "process-local worker authority must not be restored");
    PEF_CHECK_MSG(ctx, !view.execution.lease.valid(),
                  "a lease from the previous epoch must not be restored");

    CallerContext new_caller;
    new_caller.epoch = restarted.epoch();
    StartRequest stale = start;
    stale.request = RequestId{43};
    const Status stale_status = restarted.start(stale);
    PEF_CHECK_MSG(ctx, !stale_status.ok(), "stale epoch traffic must be refused");

    AuditReport audit;
    PEF_REQUIRE(ctx, restarted.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
    PEF_REQUIRE(ctx, restarted.shutdown().ok());
}

PEF_TEST(runtime, non_repeatable_interruption_becomes_explicit_ambiguity) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("ambiguity");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{50};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());

    const WorkerId worker = derive_worker_id("worker-ambiguity");
    BindWorkerRequest bind;
    bind.request = RequestId{51};
    bind.execution = created.execution;
    bind.worker = worker;
    bind.boot = mint_worker_boot_id(worker);
    BindWorkerResult bound;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());

    LeaseToken token;
    token.execution = created.execution;
    token.execution_generation = created.generation;
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = fixture.runtime.epoch();
    token.worker = worker;
    token.boot = bind.boot;
    token.policy_generation = created.policy_generation;

    StartRequest start;
    start.request = RequestId{52};
    start.token = token;
    PEF_REQUIRE(ctx, fixture.runtime.start(start).ok());

    PEF_PHASE(ctx, ACTION);
    BeginActionRequest begin;
    begin.request = RequestId{53};
    begin.token = token;
    begin.effect_class = SideEffectClass::NonRepeatable;
    BeginActionResult action;
    PEF_REQUIRE(ctx, fixture.runtime.begin_action(begin, action).ok());
    PEF_CHECK(ctx, action.ordinal == 1);

    PEF_PHASE(ctx, KILL);
    // The worker dies after dispatch and before any durable receipt. The
    // coordinator observes the loss and fences the boot.
    CallerContext worker_caller;
    SessionId session;
    PEF_REQUIRE(ctx, fixture.runtime.begin_session(worker, bind.boot, worker_caller, session).ok());
    PEF_REQUIRE(ctx, fixture.runtime.end_session(session).ok());

    PEF_PHASE(ctx, AMBIGUITY);
    RecoveryOutcome recovered;
    PEF_REQUIRE(ctx, fixture.runtime
                         .recover(fixture.caller, RequestId{54}, created.execution, recovered)
                         .ok());
    PEF_CHECK_MSG(ctx, recovered.plan.decision == RecoveryDecision::AmbiguousCompletion,
                  std::string("unexpected decision ") +
                      std::string(recovery_decision_name(recovered.plan.decision)));

    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(created.execution, view).ok());
    PEF_CHECK(ctx, view.execution.blocked);
    PEF_CHECK(ctx, view.ambiguity.state == AmbiguityState::Open);

    // No new work may start above the ambiguous ordinal, and the ambiguous
    // action itself must not be completable.
    BindWorkerRequest rebind;
    rebind.request = RequestId{55};
    rebind.execution = created.execution;
    rebind.worker = worker;
    rebind.boot = mint_worker_boot_id(worker);
    BindWorkerResult replacement;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, rebind, replacement).ok());

    CompleteActionRequest complete;
    complete.request = RequestId{56};
    complete.token = token;
    complete.action = action.action;
    complete.action_generation = action.action_generation;
    CompleteActionResult done;
    const Status completion = fixture.runtime.complete_action(complete, done);
    PEF_CHECK_MSG(ctx, !completion.ok(),
                  "an ambiguous action must not be completed by a stale worker");

    PEF_PHASE(ctx, VERIFY);
    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(runtime, durable_receipt_resolves_interruption_without_replay) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("receipt");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{60};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());

    const WorkerId worker = derive_worker_id("worker-receipt");
    BindWorkerRequest bind;
    bind.request = RequestId{61};
    bind.execution = created.execution;
    bind.worker = worker;
    bind.boot = mint_worker_boot_id(worker);
    BindWorkerResult bound;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());

    LeaseToken token;
    token.execution = created.execution;
    token.execution_generation = created.generation;
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = fixture.runtime.epoch();
    token.worker = worker;
    token.boot = bind.boot;
    token.policy_generation = created.policy_generation;

    StartRequest start;
    start.request = RequestId{62};
    start.token = token;
    PEF_REQUIRE(ctx, fixture.runtime.start(start).ok());

    BeginActionRequest begin;
    begin.request = RequestId{63};
    begin.token = token;
    begin.effect_class = SideEffectClass::NonRepeatable;
    BeginActionResult action;
    PEF_REQUIRE(ctx, fixture.runtime.begin_action(begin, action).ok());

    PEF_PHASE(ctx, AMBIGUITY);
    ReportSideEffectRequest receipt;
    receipt.request = RequestId{64};
    receipt.token = token;
    receipt.action = action.action;
    receipt.action_generation = action.action_generation;
    receipt.applied = true;
    receipt.evidence.kind = EvidenceKind::Real;
    receipt.evidence.source = "external-receipt";
    PEF_REQUIRE(ctx, fixture.runtime.report_side_effect(receipt).ok());

    // The worker dies after recording the receipt and before acknowledging the
    // commit. No replay is needed: the receipt is durable evidence.
    CallerContext worker_caller;
    SessionId session;
    PEF_REQUIRE(ctx, fixture.runtime.begin_session(worker, bind.boot, worker_caller, session).ok());
    PEF_REQUIRE(ctx, fixture.runtime.end_session(session).ok());

    RecoveryOutcome recovered;
    PEF_REQUIRE(ctx, fixture.runtime
                         .recover(fixture.caller, RequestId{65}, created.execution, recovered)
                         .ok());
    PEF_CHECK(ctx, recovered.plan.commit_from_receipt);
    PEF_CHECK(ctx, recovered.committed);

    PEF_PHASE(ctx, COMMIT);
    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(created.execution, view).ok());
    PEF_CHECK(ctx, view.execution.progress_generation.value() == 1);
    PEF_CHECK(ctx, view.execution.committed_actions == 1);
    PEF_CHECK(ctx, !view.execution.blocked);

    PEF_PHASE(ctx, VERIFY);
    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(runtime, suspension_during_action_is_reclassified_after_restart) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("suspend");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{70};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());

    const WorkerId worker = derive_worker_id("worker-suspend");
    BindWorkerRequest bind;
    bind.request = RequestId{71};
    bind.execution = created.execution;
    bind.worker = worker;
    bind.boot = mint_worker_boot_id(worker);
    BindWorkerResult bound;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());

    LeaseToken token;
    token.execution = created.execution;
    token.execution_generation = created.generation;
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = fixture.runtime.epoch();
    token.worker = worker;
    token.boot = bind.boot;
    token.policy_generation = created.policy_generation;

    StartRequest start;
    start.request = RequestId{72};
    start.token = token;
    PEF_REQUIRE(ctx, fixture.runtime.start(start).ok());

    PEF_PHASE(ctx, SUSPEND);
    PEF_REQUIRE(ctx, fixture.runtime.suspend_begin(fixture.caller, RequestId{73}, created.execution)
                         .ok());
    ExecutionView mid;
    PEF_REQUIRE(ctx, fixture.runtime.query(created.execution, mid).ok());
    PEF_CHECK(ctx, mid.execution.lifecycle == Lifecycle::Suspending);

    // The coordinator stops while the execution is mid-suspension.
    PEF_REQUIRE(ctx, fixture.runtime.take_snapshot(true).ok());
    PEF_REQUIRE(ctx, fixture.runtime.shutdown().ok());

    PEF_PHASE(ctx, RESTART);
    Runtime restarted;
    RuntimeConfig config;
    config.store_path = fixture.store.path();
    OpenOutcome outcome;
    PEF_REQUIRE(ctx, restarted.open(config, outcome).ok());

    PEF_PHASE(ctx, VERIFY);
    ExecutionView view;
    PEF_REQUIRE(ctx, restarted.query(created.execution, view).ok());
    PEF_CHECK_MSG(ctx, view.execution.lifecycle != Lifecycle::Running,
                  "a mid-suspension execution must not come back RUNNING");
    PEF_CHECK(ctx, !view.execution.lease.valid());

    PEF_REQUIRE(ctx, restarted.shutdown().ok());
}

PEF_TEST(runtime, policy_change_and_binding_change_invalidate_continuations) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("revalidation");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{80};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());

    BindingSet bindings;
    bindings.refs.push_back(
        BindingRef{BindingDomain::Model, BindingId{0x501}, BindingGeneration{1}, "model-a"});
    bindings.canonicalize();

    const WorkerId worker = derive_worker_id("worker-revalidation");
    BindWorkerRequest bind;
    bind.request = RequestId{81};
    bind.execution = created.execution;
    bind.worker = worker;
    bind.boot = mint_worker_boot_id(worker);
    bind.bindings = bindings;
    BindWorkerResult bound;
    PEF_REQUIRE(ctx, fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());

    LeaseToken token;
    token.execution = created.execution;
    token.execution_generation = created.generation;
    token.incarnation = bound.incarnation;
    token.incarnation_generation = bound.incarnation_generation;
    token.lease = bound.lease;
    token.lease_generation = bound.lease_generation;
    token.epoch = fixture.runtime.epoch();
    token.worker = worker;
    token.boot = bind.boot;
    token.policy_generation = created.policy_generation;

    StartRequest start;
    start.request = RequestId{82};
    start.token = token;
    PEF_REQUIRE(ctx, fixture.runtime.start(start).ok());

    RegisterCheckpointRequest checkpoint;
    checkpoint.request = RequestId{83};
    checkpoint.token = token;
    checkpoint.expected_checkpoint_generation = CheckpointGeneration{};
    checkpoint.content_size = 128;
    checkpoint.content_hash = 0xabcdef;
    RegisterCheckpointResult registered;
    PEF_REQUIRE(ctx, fixture.runtime.register_checkpoint(checkpoint, registered).ok());

    PEF_PHASE(ctx, VERIFY);
    ContinuationState state = ContinuationState::Invalid;
    std::string reason;
    PEF_REQUIRE(ctx, fixture.runtime
                         .validate_continuation(fixture.caller, created.execution, state, reason)
                         .ok());
    PEF_CHECK(ctx, state == ContinuationState::Valid);

    PEF_PHASE(ctx, RECOVER);
    // A policy change invalidates the authority granted under the old policy.
    ExecutionView before;
    PEF_REQUIRE(ctx, fixture.runtime.query(created.execution, before).ok());
    ExecutionPolicy updated = before.policy;
    updated.max_replay_depth = 7;
    PEF_REQUIRE(ctx, fixture.runtime
                         .update_policy(fixture.caller, RequestId{84}, created.execution, updated,
                                        true)
                         .ok());
    PEF_REQUIRE(ctx, fixture.runtime
                         .validate_continuation(fixture.caller, created.execution, state, reason)
                         .ok());
    PEF_CHECK_MSG(ctx, state != ContinuationState::Valid,
                  "a continuation from before a policy change must not stay valid");

    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(created.execution, view).ok());
    PEF_CHECK(ctx, view.execution.policy_generation.value() == 2);
    PEF_CHECK(ctx, !view.execution.lease.valid());
    PEF_CHECK(ctx, view.execution.lifecycle == Lifecycle::RecoveryRequired);

    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(runtime, illegal_lifecycle_operations_are_refused) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("lifecycle-refusals");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{90};
    create.policy = test_policy();
    CreateExecutionResult created;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, created).ok());

    PEF_PHASE(ctx, VERIFY);
    // Starting an execution that has no lease and is not READY must fail.
    StartRequest start;
    start.request = RequestId{91};
    start.token.execution = created.execution;
    start.token.execution_generation = created.generation;
    start.token.epoch = fixture.runtime.epoch();
    const Status status = fixture.runtime.start(start);
    PEF_CHECK(ctx, !status.ok());

    PEF_REQUIRE(ctx, fixture.runtime.cancel(fixture.caller, RequestId{92}, created.execution, "test")
                         .ok());
    PEF_REQUIRE(ctx, fixture.runtime.retire(fixture.caller, RequestId{93}, created.execution, "test")
                         .ok());

    ExecutionView view;
    PEF_REQUIRE(ctx, fixture.runtime.query(created.execution, view).ok());
    PEF_CHECK(ctx, view.execution.lifecycle == Lifecycle::Retired);

    // A retired execution cannot be bound, started, suspended, or resumed.
    BindWorkerRequest bind;
    bind.request = RequestId{94};
    bind.execution = created.execution;
    bind.worker = derive_worker_id("late-worker");
    bind.boot = mint_worker_boot_id(bind.worker);
    BindWorkerResult bound;
    PEF_CHECK(ctx, !fixture.runtime.bind_worker(fixture.caller, bind, bound).ok());
    PEF_CHECK(ctx, !fixture.runtime.suspend_begin(fixture.caller, RequestId{95}, created.execution)
                         .ok());

    AuditReport audit;
    PEF_REQUIRE(ctx, fixture.runtime.audit(audit).ok());
    PEF_CHECK_MSG(ctx, audit.clean(), audit.render());
}

PEF_TEST(runtime, request_identity_makes_creation_idempotent) {
    PEF_PHASE(ctx, SETUP);
    Fixture fixture("idempotent-create");
    PEF_REQUIRE(ctx, fixture.open().ok());

    CreateExecutionRequest create;
    create.request = RequestId{0x1234};
    create.policy = test_policy();
    CreateExecutionResult first;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, first).ok());
    CreateExecutionResult second;
    PEF_REQUIRE(ctx, fixture.runtime.create_execution(fixture.caller, create, second).ok());
    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, second.existing);
    PEF_CHECK(ctx, second.execution == first.execution);

    std::vector<ExecutionRecord> all;
    PEF_REQUIRE(ctx, fixture.runtime.list_executions(all).ok());
    PEF_CHECK(ctx, all.size() == 1);
}
