// Persistent Execution Fabric - randomized and property tests.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Every randomized case records the seed it used and is reproducible from that
// seed alone. Determinism is itself a property under test: equivalent canonical
// state must yield equivalent decisions and equivalent bytes.
#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "pef/audit.hpp"
#include "pef/persistence.hpp"
#include "pef/runtime.hpp"
#include "pef/version.hpp"
#include "test_framework.hpp"

using namespace pef;

namespace {

struct RandomExecution {
    peftest::TempStore dir;
    Runtime runtime;
    CallerContext caller;
    ExecutionId execution;
    LeaseToken token;
    WorkerId worker;
    WorkerBootId boot;
    ExecutionGeneration generation;
    PolicyGeneration policy_generation;
    std::uint64_t next_request = 1;
    std::uint64_t next_hash = 0x1000;

    explicit RandomExecution(const char* label) : dir(label) {}

    [[nodiscard]] RequestId request() { return RequestId{next_request++}; }

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
        policy.allow_older_checkpoints = true;
        policy.allow_recovery_from_failed = true;
        policy.max_action_history = 1000000;
        CreateExecutionRequest create;
        create.request = request();
        create.policy = policy;
        CreateExecutionResult created;
        PEF_TRY(runtime.create_execution(caller, create, created));
        execution = created.execution;
        generation = created.generation;
        policy_generation = created.policy_generation;
        return rebind();
    }

    Status rebind() {
        worker = derive_worker_id("property-worker");
        boot = mint_worker_boot_id(worker);
        BindWorkerRequest bind;
        bind.request = request();
        bind.execution = execution;
        bind.worker = worker;
        bind.boot = boot;
        BindWorkerResult bound;
        PEF_TRY(runtime.bind_worker(caller, bind, bound));
        token.execution = execution;
        token.execution_generation = generation;
        token.incarnation = bound.incarnation;
        token.incarnation_generation = bound.incarnation_generation;
        token.lease = bound.lease;
        token.lease_generation = bound.lease_generation;
        token.epoch = runtime.epoch();
        token.worker = worker;
        token.boot = boot;
        token.policy_generation = policy_generation;
        return ok_status();
    }

    Status refresh() {
        ExecutionView view;
        PEF_TRY(runtime.query(execution, view));
        generation = view.execution.generation;
        policy_generation = view.execution.policy_generation;
        token.execution_generation = generation;
        token.policy_generation = policy_generation;
        token.epoch = runtime.epoch();
        return ok_status();
    }

    [[nodiscard]] Status ensure_running() {
        ExecutionView view;
        PEF_TRY(runtime.query(execution, view));
        if (view.execution.lifecycle == Lifecycle::Running) {
            return ok_status();
        }
        if (view.execution.lifecycle == Lifecycle::Ready) {
            StartRequest start;
            start.request = request();
            start.token = token;
            return runtime.start(start);
        }
        if (view.execution.lifecycle == Lifecycle::RecoveryRequired) {
            RecoveryOutcome outcome;
            PEF_TRY(runtime.recover(caller, request(), execution, outcome));
        }
        ResumeRequest resume;
        resume.request = request();
        resume.token = token;
        resume.revalidate_bindings = true;
        ResumeResult resumed;
        return runtime.resume(resume, resumed);
    }

    [[nodiscard]] Status clear_ambiguity() {
        ExecutionView view;
        PEF_TRY(runtime.query(execution, view));
        if (!view.execution.ambiguity.valid()) {
            return ok_status();
        }
        ResolveAmbiguityRequest resolve;
        resolve.request = request();
        resolve.caller = caller;
        resolve.execution = execution;
        resolve.ambiguity = view.execution.ambiguity;
        resolve.ambiguity_generation = view.execution.ambiguity_generation;
        resolve.resolution = AmbiguityState::ResolvedNotApplied;
        resolve.note = "randomized cleanup";
        ResolveAmbiguityResult resolved;
        return runtime.resolve_ambiguity(resolve, resolved);
    }
};

constexpr SideEffectClass kClasses[] = {
    SideEffectClass::Pure,       SideEffectClass::Idempotent,
    SideEffectClass::RepeatableWithKey, SideEffectClass::Compensatable,
    SideEffectClass::NonRepeatable,     SideEffectClass::Unknown};

}  // namespace

PEF_TEST(property, randomized_lifecycle_sequences_keep_invariants) {
    PEF_PHASE(ctx, SETUP);
    constexpr std::uint64_t kSeeds = 12;
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        RandomExecution harness("prop-lifecycle");
        PEF_REQUIRE(ctx, harness.setup().ok());
        peftest::Rng rng(seed * 7919 + 13);
        std::cout << "  SEED " << (seed * 7919 + 13) << std::endl;

        for (int step = 0; step < 40; ++step) {
            const std::uint32_t choice = rng.below(100);
            if (choice < 30) {
                BeginActionRequest begin;
                begin.request = harness.request();
                begin.token = harness.token;
                begin.effect_class = kClasses[rng.below(6)];
                if (requires_request_key(begin.effect_class)) {
                    begin.request_key = "key-" + std::to_string(step);
                }
                BeginActionResult action;
                if (!harness.runtime.begin_action(begin, action).ok()) {
                    continue;
                }
                CompleteActionRequest complete;
                complete.request = harness.request();
                complete.token = harness.token;
                complete.action = action.action;
                complete.action_generation = action.action_generation;
                complete.completion.effect_applied = true;
                CompleteActionResult result;
                (void)harness.runtime.complete_action(complete, result);
            } else if (choice < 45) {
                ExecutionView view;
                if (!harness.runtime.query(harness.execution, view).ok()) {
                    continue;
                }
                RegisterCheckpointRequest checkpoint;
                checkpoint.request = harness.request();
                checkpoint.token = harness.token;
                checkpoint.expected_checkpoint = view.execution.checkpoint;
                checkpoint.expected_checkpoint_generation = view.execution.checkpoint_generation;
                checkpoint.content_size = 64 + step;
                checkpoint.content_hash = harness.next_hash++;
                checkpoint.effect_boundary_ordinal = view.execution.action_frontier;
                RegisterCheckpointResult registered;
                (void)harness.runtime.register_checkpoint(checkpoint, registered);
            } else if (choice < 55) {
                (void)harness.runtime.fence(harness.caller, harness.request(), harness.execution,
                                            "random");
            } else if (choice < 65) {
                RecoveryOutcome recovered;
                (void)harness.runtime.recover(harness.caller, harness.request(), harness.execution,
                                              recovered);
            } else if (choice < 75) {
                (void)harness.clear_ambiguity();
            } else if (choice < 85) {
                ContinuationState state = ContinuationState::Invalid;
                std::string reason;
                (void)harness.runtime.validate_continuation(harness.caller, harness.execution,
                                                            state, reason);
            } else if (choice < 95) {
                (void)harness.ensure_running();
                (void)harness.refresh();
            } else {
                (void)harness.runtime.take_snapshot(false);
            }

            AuditReport audit;
            PEF_REQUIRE(ctx, harness.runtime.audit(audit).ok());
            PEF_CHECK_MSG(ctx, audit.clean(),
                          "seed " + std::to_string(seed * 7919 + 13) + " step " +
                              std::to_string(step) + ": " + audit.render());
            if (!audit.clean()) {
                break;
            }
        }
        PEF_REQUIRE(ctx, harness.runtime.shutdown().ok());
    }
}

PEF_TEST(property, randomized_state_round_trips_byte_for_byte) {
    PEF_PHASE(ctx, CREATE);
    for (std::uint64_t seed = 1; seed <= 16; ++seed) {
        peftest::Rng rng(seed * 104729 + 3);
        DurableState state;
        state.store = StoreId{0x1000 + seed};
        state.epoch = CoordinatorEpoch{rng.below(50) + 1};
        state.sequence = rng.next() % 100000;
        state.next_execution_sequence = rng.next() % 100000 + 1;
        state.schema = kPersistenceSchemaVersion;

        const std::uint32_t executions = 1 + rng.below(6);
        for (std::uint32_t i = 0; i < executions; ++i) {
            ExecutionRecord execution;
            execution.id = ExecutionId{0x10000 + seed * 100 + i};
            execution.generation = ExecutionGeneration{1 + rng.below(4)};
            execution.lifecycle = static_cast<Lifecycle>(rng.below(kLifecycleCount));
            execution.policy = PolicyId{0x200 + i};
            execution.policy_generation = PolicyGeneration{1 + rng.below(3)};
            execution.epoch = state.epoch;
            state.executions.upsert(execution);

            ExecutionPolicy policy = default_policy(execution.policy);
            policy.generation = execution.policy_generation;
            state.policies.upsert(policy);

            const std::uint32_t actions = rng.below(5);
            for (std::uint32_t a = 0; a < actions; ++a) {
                ActionRecord action;
                action.id = derive_action_id(execution.id, a + 1);
                action.generation = ActionGeneration{1 + rng.below(3)};
                action.execution = execution.id;
                action.execution_generation = execution.generation;
                action.sequence = a + 1;
                action.effect_class = kClasses[rng.below(6)];
                if (requires_request_key(action.effect_class)) {
                    action.request_key = "k" + std::to_string(a);
                }
                action.status = static_cast<ActionStatus>(rng.below(kActionStatusCount));
                action.receipt.kind = static_cast<EvidenceKind>(rng.below(4));
                action.receipt.source = "s" + std::to_string(a);
                state.actions.upsert(action);
            }
            execution.action_frontier = actions;
            state.executions.upsert(execution);
        }

        const Bytes first = encode_to_bytes(state);
        PEF_PHASE(ctx, VERIFY);
        DurableState decoded;
        PEF_REQUIRE(ctx, decode_from_bytes(first, decoded));
        const Bytes second = encode_to_bytes(decoded);
        PEF_CHECK_MSG(ctx, first == second, "seed " + std::to_string(seed) +
                                                ": state did not round trip byte for byte");
    }
}

PEF_TEST(property, equivalent_state_yields_equivalent_decisions) {
    PEF_PHASE(ctx, SETUP);
    // The same durable inputs must always classify the same way, regardless of
    // the order in which records were inserted.
    constexpr std::uint64_t kSeeds = 24;
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        peftest::Rng rng(seed * 31337 + 5);
        ExecutionRecord execution;
        execution.id = ExecutionId{0x500 + seed};
        execution.generation = ExecutionGeneration{1};
        execution.lifecycle = Lifecycle::RecoveryRequired;
        execution.policy_generation = PolicyGeneration{1};
        execution.blocked = rng.below(2) == 1;
        execution.blocked_above_ordinal = rng.below(4);

        ExecutionPolicy policy = default_policy(PolicyId{1});
        policy.generation = PolicyGeneration{1};
        policy.ambiguity_handling =
            static_cast<AmbiguityHandling>(rng.below(3));
        policy.stale_binding_behavior = static_cast<StaleBindingBehavior>(rng.below(2));
        policy.recovery_preference = static_cast<RecoveryPreference>(rng.below(3));

        ActionRecord action;
        action.id = ActionId{0x900 + seed};
        action.generation = ActionGeneration{1};
        action.sequence = 1 + rng.below(4);
        action.effect_class = kClasses[rng.below(6)];
        action.status = static_cast<ActionStatus>(rng.below(kActionStatusCount));

        AmbiguityRecord ambiguity;
        ambiguity.id = AmbiguityId{0x700 + seed};
        ambiguity.execution = execution.id;
        ambiguity.action = action.id;
        ambiguity.action_generation = action.generation;
        ambiguity.action_ordinal = action.sequence;
        ambiguity.effect_class = action.effect_class;
        ambiguity.state = static_cast<AmbiguityState>(rng.below(kAmbiguityStateCount));

        RecoveryInput input;
        input.execution = &execution;
        input.policy = &policy;
        input.epoch = CoordinatorEpoch{2};
        input.in_flight = &action;
        input.ambiguity = execution.blocked ? &ambiguity : nullptr;
        input.binding_mismatch = rng.below(4) == 0;

        const RecoveryPlan first = classify_recovery(input);
        for (int repeat = 0; repeat < 5; ++repeat) {
            const RecoveryPlan again = classify_recovery(input);
            PEF_CHECK_MSG(ctx, again.decision == first.decision,
                          "seed " + std::to_string(seed) + ": classification is not deterministic");
            PEF_CHECK(ctx, again.explanation == first.explanation);
        }
        PEF_PHASE(ctx, VERIFY);
    }
}

PEF_TEST(property, randomized_failure_and_recovery_sequences_are_conservative) {
    PEF_PHASE(ctx, SETUP);
    constexpr std::uint64_t kSeeds = 8;
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        RandomExecution harness("prop-recovery");
        PEF_REQUIRE(ctx, harness.setup().ok());
        peftest::Rng rng(seed * 65537 + 11);
        std::cout << "  SEED " << (seed * 65537 + 11) << std::endl;

        for (int round = 0; round < 12; ++round) {
            const SideEffectClass cls = kClasses[rng.below(6)];
            BeginActionRequest begin;
            begin.request = harness.request();
            begin.token = harness.token;
            begin.effect_class = cls;
            if (requires_request_key(cls)) {
                begin.request_key = "round-" + std::to_string(round);
            }
            BeginActionResult action;
            if (!harness.runtime.begin_action(begin, action).ok()) {
                // Authority is gone; re-establish it the way a replacement
                // worker would and continue.
                (void)harness.runtime.fence(harness.caller, harness.request(), harness.execution,
                                            "random");
                PEF_REQUIRE(ctx, harness.rebind().ok());
                PEF_REQUIRE(ctx, harness.refresh().ok());
                continue;
            }

            // The worker "dies" without recording an outcome, or records one,
            // or completes the action. The runtime must classify each case the
            // same way every time.
            const std::uint32_t fate = rng.below(10);
            if (fate < 4) {
                (void)harness.runtime.fence(harness.caller, harness.request(), harness.execution,
                                            "random death");
            } else if (fate < 6) {
                ReportSideEffectRequest receipt;
                receipt.request = harness.request();
                receipt.token = harness.token;
                receipt.action = action.action;
                receipt.action_generation = action.action_generation;
                receipt.applied = rng.below(2) == 1;
                (void)harness.runtime.report_side_effect(receipt);
                (void)harness.runtime.fence(harness.caller, harness.request(), harness.execution,
                                            "random death");
            } else {
                CompleteActionRequest complete;
                complete.request = harness.request();
                complete.token = harness.token;
                complete.action = action.action;
                complete.action_generation = action.action_generation;
                complete.completion.effect_applied = true;
                CompleteActionResult result;
                (void)harness.runtime.complete_action(complete, result);
            }

            RecoveryPlan plan;
            PEF_REQUIRE(ctx, harness.runtime.classify(harness.caller, harness.execution, plan).ok());
            if (plan.decision == RecoveryDecision::AmbiguousCompletion ||
                plan.decision == RecoveryDecision::ManualResolutionRequired) {
                PEF_CHECK_MSG(ctx, !recovery_allows_automatic_continue(plan.decision),
                              "an unresolved ambiguity allowed automatic continuation");
                PEF_REQUIRE(ctx, harness.clear_ambiguity().ok());
            }
            PEF_REQUIRE(ctx, harness.rebind().ok());
            PEF_REQUIRE(ctx, harness.refresh().ok());
            (void)harness.ensure_running();

            AuditReport audit;
            PEF_REQUIRE(ctx, harness.runtime.audit(audit).ok());
            PEF_CHECK_MSG(ctx, audit.clean(), "seed " + std::to_string(seed * 65537 + 11) + ": " +
                                                  audit.render());
            if (!audit.clean()) {
                break;
            }
        }
        PEF_REQUIRE(ctx, harness.runtime.shutdown().ok());
    }
}

PEF_TEST(property, persistence_round_trip_survives_repeated_snapshot_and_reload) {
    PEF_PHASE(ctx, SETUP);
    RandomExecution harness("prop-roundtrip");
    PEF_REQUIRE(ctx, harness.setup().ok());
    PEF_REQUIRE(ctx, harness.ensure_running().ok());

    std::vector<std::uint64_t> digests;
    for (int cycle = 0; cycle < 4; ++cycle) {
        for (int i = 0; i < 5; ++i) {
            BeginActionRequest begin;
            begin.request = harness.request();
            begin.token = harness.token;
            begin.effect_class = SideEffectClass::Pure;
            BeginActionResult action;
            PEF_REQUIRE(ctx, harness.runtime.begin_action(begin, action).ok());
            CompleteActionRequest complete;
            complete.request = harness.request();
            complete.token = harness.token;
            complete.action = action.action;
            complete.action_generation = action.action_generation;
            complete.completion.effect_applied = true;
            CompleteActionResult result;
            PEF_REQUIRE(ctx, harness.runtime.complete_action(complete, result).ok());
        }
        ExecutionView view;
        PEF_REQUIRE(ctx, harness.runtime.query(harness.execution, view).ok());
        RegisterCheckpointRequest checkpoint;
        checkpoint.request = harness.request();
        checkpoint.token = harness.token;
        checkpoint.expected_checkpoint = view.execution.checkpoint;
        checkpoint.expected_checkpoint_generation = view.execution.checkpoint_generation;
        checkpoint.content_size = 100;
        checkpoint.content_hash = harness.next_hash++;
        checkpoint.effect_boundary_ordinal = view.execution.action_frontier;
        RegisterCheckpointResult registered;
        PEF_REQUIRE(ctx, harness.runtime.register_checkpoint(checkpoint, registered).ok());

        AuditReport audit;
        PEF_REQUIRE(ctx, harness.runtime.audit(audit).ok());
        digests.push_back(audit.digest());

        PEF_REQUIRE(ctx, harness.runtime.take_snapshot(false).ok());
        PEF_REQUIRE(ctx, harness.runtime.shutdown().ok());

        PEF_PHASE(ctx, RECOVER);
        Runtime reloaded;
        RuntimeConfig config;
        config.store_path = harness.dir.path();
        OpenOutcome outcome;
        PEF_REQUIRE(ctx, reloaded.open(config, outcome).ok());
        AuditReport reloaded_audit;
        PEF_REQUIRE(ctx, reloaded.audit(reloaded_audit).ok());
        PEF_CHECK_MSG(ctx, reloaded_audit.clean(), reloaded_audit.render());
        ExecutionView reloaded_view;
        PEF_REQUIRE(ctx, reloaded.query(harness.execution, reloaded_view).ok());
        PEF_CHECK(ctx, reloaded_view.execution.progress_generation.value() ==
                           static_cast<std::uint64_t>((cycle + 1) * 5));
        PEF_REQUIRE(ctx, reloaded.shutdown().ok());

        // Re-open the original harness against the same directory so the next
        // cycle continues the same durable history.
        PEF_REQUIRE(ctx, harness.runtime.open(config, outcome).ok());
        harness.caller.epoch = harness.runtime.epoch();
        // The restart revoked the previous lease and incarnation, so authority
        // has to be re-established exactly as a replacement worker would.
        PEF_REQUIRE(ctx, harness.rebind().ok());
        PEF_REQUIRE(ctx, harness.refresh().ok());
        PEF_REQUIRE(ctx, harness.ensure_running().ok());
    }

    PEF_CHECK(ctx, digests.size() == 4);
    PEF_CHECK_MSG(ctx, std::adjacent_find(digests.begin(), digests.end()) == digests.end(),
                  "successive durable states produced the same audit digest");
    PEF_REQUIRE(ctx, harness.runtime.shutdown().ok());
}
