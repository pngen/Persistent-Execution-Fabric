// Persistent Execution Fabric - core unit tests.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <string>
#include <vector>

#include "pef/audit.hpp"
#include "pef/codec.hpp"
#include "pef/hash.hpp"
#include "pef/identity.hpp"
#include "pef/lifecycle.hpp"
#include "pef/persistence.hpp"
#include "pef/policy.hpp"
#include "pef/records.hpp"
#include "pef/recovery.hpp"
#include "pef/side_effect.hpp"
#include "pef/version.hpp"
#include "test_framework.hpp"

using namespace pef;

PEF_TEST(identity, distinct_domains_are_distinct_types) {
    PEF_PHASE(ctx, SETUP);
    // Compile-time separation: this file only compiles because these are
    // different types. The runtime check confirms value semantics.
    const ExecutionId execution{0x1111};
    const WorkerId worker{0x1111};
    const ActionId action{0x1111};
    PEF_CHECK(ctx, execution.value() == worker.value());
    PEF_CHECK(ctx, action.value() == worker.value());
    PEF_CHECK(ctx, !(execution == ExecutionId{}));
    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, !ExecutionId{}.valid());
    PEF_CHECK(ctx, ExecutionId{1}.valid());
}

PEF_TEST(identity, generations_advance_monotonically) {
    PEF_PHASE(ctx, SETUP);
    ExecutionGeneration generation;
    PEF_CHECK(ctx, generation.is_none());
    generation = generation.next();
    PEF_CHECK(ctx, generation.value() == 1);
    generation = generation.advance(4);
    PEF_CHECK(ctx, generation.value() == 5);
    PEF_CHECK(ctx, ExecutionGeneration{4} < generation);
}

PEF_TEST(identity, text_form_round_trips_and_rejects_junk) {
    PEF_PHASE(ctx, SETUP);
    const ExecutionId id{0x0123456789abcdefULL};
    const std::string text = to_string(id);
    PEF_CHECK(ctx, text == "0123456789abcdef");
    const auto parsed = parse_id<ExecutionIdTag>(text);
    PEF_REQUIRE(ctx, parsed.has_value());
    PEF_CHECK(ctx, *parsed == id);
    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, !parse_id<ExecutionIdTag>("0123456789abcde").has_value());
    PEF_CHECK(ctx, !parse_id<ExecutionIdTag>("0123456789abcdeg").has_value());
    PEF_CHECK(ctx, !parse_id<ExecutionIdTag>(" 123456789abcdef").has_value());
    PEF_CHECK(ctx, !parse_id<ExecutionIdTag>("0x123456789abcd").has_value());
    PEF_CHECK(ctx, parse_id<ExecutionIdTag>("18446744073709551615", true).has_value());
    PEF_CHECK(ctx, !parse_id<ExecutionIdTag>("18446744073709551616", true).has_value());
}

PEF_TEST(lifecycle, forbidden_transitions_are_refused) {
    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, !is_legal_transition(Lifecycle::Completed, Lifecycle::Running));
    PEF_CHECK(ctx, !is_legal_transition(Lifecycle::Retired, Lifecycle::Ready));
    PEF_CHECK(ctx, !is_legal_transition(Lifecycle::RecoveryRequired, Lifecycle::Running));
    PEF_CHECK(ctx, !is_legal_transition(Lifecycle::Fenced, Lifecycle::Running));
    PEF_CHECK(ctx, !is_legal_transition(Lifecycle::Completed, Lifecycle::Completed));
    for (std::uint8_t i = 0; i < kLifecycleCount; ++i) {
        const auto state = static_cast<Lifecycle>(i);
        PEF_CHECK_MSG(ctx, !is_legal_transition(state, state),
                      "self transitions must never be legal");
    }
    PEF_CHECK(ctx, is_legal_transition(Lifecycle::Running, Lifecycle::Completed));
    PEF_CHECK(ctx, is_legal_transition(Lifecycle::Failed, Lifecycle::RecoveryRequired));
    PEF_CHECK(ctx, is_legal_transition(Lifecycle::Fenced, Lifecycle::RecoveryRequired));
    PEF_CHECK(ctx, is_terminal_state(Lifecycle::Retired));
    PEF_CHECK(ctx, !is_terminal_state(Lifecycle::Failed));
    PEF_CHECK(ctx, is_active_state(Lifecycle::Running));
    PEF_CHECK(ctx, !is_active_state(Lifecycle::Suspended));
}

PEF_TEST(lifecycle, names_round_trip) {
    PEF_PHASE(ctx, VERIFY);
    for (std::uint8_t i = 0; i < kLifecycleCount; ++i) {
        const auto state = static_cast<Lifecycle>(i);
        const auto parsed = parse_lifecycle(lifecycle_name(state));
        PEF_CHECK(ctx, parsed.has_value() && *parsed == state);
    }
    PEF_CHECK(ctx, !parse_lifecycle("RUNNING_NOW").has_value());
}

PEF_TEST(side_effect, taxonomy_is_conservative) {
    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, auto_replayable(SideEffectClass::Pure));
    PEF_CHECK(ctx, auto_replayable(SideEffectClass::Idempotent));
    PEF_CHECK(ctx, auto_replayable(SideEffectClass::RepeatableWithKey));
    PEF_CHECK(ctx, !auto_replayable(SideEffectClass::NonRepeatable));
    PEF_CHECK(ctx, !auto_replayable(SideEffectClass::Unknown));
    PEF_CHECK(ctx, !auto_replayable(SideEffectClass::Compensatable));
    PEF_CHECK(ctx, requires_request_key(SideEffectClass::RepeatableWithKey));
    PEF_CHECK(ctx, !requires_request_key(SideEffectClass::Idempotent));
    PEF_CHECK(ctx, forbids_automatic_retry(SideEffectClass::NonRepeatable));
    PEF_CHECK(ctx, forbids_automatic_retry(SideEffectClass::Unknown));
    PEF_CHECK(ctx, requires_compensation(SideEffectClass::Compensatable));
    for (std::uint8_t i = 0; i < kSideEffectClassCount; ++i) {
        const auto cls = static_cast<SideEffectClass>(i);
        PEF_CHECK(ctx, parse_side_effect_class(side_effect_class_name(cls)).has_value());
    }
}

PEF_TEST(codec, records_round_trip_deterministically) {
    PEF_PHASE(ctx, CREATE);
    ExecutionRecord execution;
    execution.id = ExecutionId{0xdeadbeefcafebabeULL};
    execution.generation = ExecutionGeneration{3};
    execution.incarnation = ExecutionIncarnationId{0x1234};
    execution.incarnation_generation = ExecutionIncarnationGeneration{2};
    execution.lifecycle = Lifecycle::Running;
    execution.policy = PolicyId{0x55};
    execution.policy_generation = PolicyGeneration{4};
    execution.epoch = CoordinatorEpoch{7};
    execution.bindings.refs.push_back(
        BindingRef{BindingDomain::Tool, BindingId{0x99}, BindingGeneration{5}, "tool-a"});
    execution.bindings.canonicalize();

    const Bytes first = encode_to_bytes(execution);
    const Bytes second = encode_to_bytes(execution);
    PEF_CHECK(ctx, first == second);

    PEF_PHASE(ctx, VERIFY);
    ExecutionRecord decoded;
    PEF_CHECK(ctx, decode_from_bytes(first, decoded));
    PEF_CHECK(ctx, decoded.id == execution.id);
    PEF_CHECK(ctx, decoded.generation == execution.generation);
    PEF_CHECK(ctx, decoded.lifecycle == execution.lifecycle);
    PEF_CHECK(ctx, decoded.epoch == execution.epoch);
    PEF_CHECK(ctx, decoded.bindings.refs.size() == 1);
    PEF_CHECK(ctx, decoded.bindings.refs[0].label == "tool-a");
    PEF_CHECK(ctx, record_digest(decoded) == record_digest(execution));
}

PEF_TEST(codec, binding_set_is_canonical_regardless_of_input_order) {
    PEF_PHASE(ctx, CREATE);
    BindingSet a;
    a.refs.push_back(BindingRef{BindingDomain::Tool, BindingId{2}, BindingGeneration{1}, "b"});
    a.refs.push_back(BindingRef{BindingDomain::Model, BindingId{1}, BindingGeneration{9}, "a"});
    BindingSet b;
    b.refs.push_back(BindingRef{BindingDomain::Model, BindingId{1}, BindingGeneration{9}, "a"});
    b.refs.push_back(BindingRef{BindingDomain::Tool, BindingId{2}, BindingGeneration{1}, "b"});
    a.canonicalize();
    b.canonicalize();
    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, encode_to_bytes(a) == encode_to_bytes(b));
    PEF_CHECK(ctx, a.equals_generations(b));
    BindingSet c = b;
    c.refs[0].generation = BindingGeneration{10};
    c.canonicalize();
    PEF_CHECK(ctx, !a.equals_generations(c));
    PEF_CHECK(ctx, binding_set_defect(a).has_value() == false);
}

PEF_TEST(codec, decoding_rejects_truncation_and_trailing_bytes) {
    PEF_PHASE(ctx, CREATE);
    ActionRecord action;
    action.id = ActionId{0x42};
    action.generation = ActionGeneration{1};
    action.execution = ExecutionId{0x43};
    action.effect_class = SideEffectClass::Idempotent;
    action.request_key = "key-1";
    action.status = ActionStatus::InFlight;
    action.sequence = 4;
    const Bytes bytes = encode_to_bytes(action);
    PEF_REQUIRE(ctx, bytes.size() > 4);

    PEF_PHASE(ctx, VERIFY);
    Bytes truncated(bytes.begin(), bytes.end() - 2);
    ActionRecord out;
    PEF_CHECK(ctx, !decode_from_bytes(truncated, out));
    Bytes extended = bytes;
    extended.push_back(std::byte{0x7f});
    PEF_CHECK(ctx, !decode_from_bytes(extended, out));
    // A flip inside an opaque identifier is not detectable by the record codec;
    // record integrity is protected by the journal frame checksum instead. A
    // flip inside a length prefix must be refused, because a length is a
    // structural claim rather than a value.
    Bytes hostile = bytes;
    for (std::size_t i = 65; i < 69; ++i) {
        hostile[i] = std::byte{0xFF};
    }
    PEF_CHECK(ctx, !decode_from_bytes(hostile, out));
}

PEF_TEST(codec, invalid_enum_ordinal_is_rejected) {
    PEF_PHASE(ctx, CREATE);
    ExecutionRecord execution;
    execution.id = ExecutionId{1};
    execution.generation = ExecutionGeneration{1};
    execution.lifecycle = Lifecycle::Running;
    const Bytes bytes = encode_to_bytes(execution);
    Bytes corrupted = bytes;
    // The lifecycle byte is the fifth 64-bit field followed by one byte; scan
    // for the encoded ordinal 2 and replace it with an out-of-range value.
    bool replaced = false;
    for (std::size_t i = 0; i + 1 < corrupted.size(); ++i) {
        if (corrupted[i] == std::byte{2} && corrupted[i + 1] == std::byte{0}) {
            corrupted[i] = std::byte{0xFE};
            replaced = true;
            break;
        }
    }
    PEF_REQUIRE(ctx, replaced);
    PEF_PHASE(ctx, VERIFY);
    ExecutionRecord decoded;
    PEF_CHECK(ctx, !decode_from_bytes(corrupted, decoded));
}

PEF_TEST(codec, hostile_counts_are_bounded) {
    PEF_PHASE(ctx, CREATE);
    ByteWriter writer;
    writer.u32(0xFFFFFFFFu);  // a container count no valid state can have
    ByteReader reader(writer.bytes());
    BindingSet set;
    const bool ok = read_field(reader, set);
    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, !ok);
    PEF_CHECK(ctx, reader.failed());
}

PEF_TEST(hash, crc_and_fnv_are_stable) {
    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, crc64_ecma("123456789", 9) == 0x995DC9BBDF1939FAULL);
    PEF_CHECK(ctx, fnv1a("") == kFnvOffsetBasis);
    PEF_CHECK(ctx, fnv1a("a") == 0xaf63dc4c8601ec8cULL);
    PEF_CHECK(ctx, fnv1a("foobar") == 0x85944171f73967e8ULL);
    std::uint64_t parsed = 0;
    PEF_CHECK(ctx, parse_hex_u64("0123456789abcdef", parsed));
    PEF_CHECK(ctx, parsed == 0x0123456789abcdefULL);
    PEF_CHECK(ctx, !parse_hex_u64("0123456789abcde", parsed));
}

PEF_TEST(records, derived_identities_are_pure_functions) {
    PEF_PHASE(ctx, VERIFY);
    const ExecutionId execution{0xabc};
    const ActionId action{0xdef};
    PEF_CHECK(ctx, derive_commit_id(execution, action, ActionGeneration{1}, ProgressGeneration{2}) ==
                       derive_commit_id(execution, action, ActionGeneration{1},
                                        ProgressGeneration{2}));
    PEF_CHECK(ctx, derive_commit_id(execution, action, ActionGeneration{1}, ProgressGeneration{2}) !=
                       derive_commit_id(execution, action, ActionGeneration{1},
                                        ProgressGeneration{3}));
    PEF_CHECK(ctx, derive_action_id(execution, 1) != derive_action_id(execution, 2));
    // Domain separation: the same input tuple in two domains must not collide.
    PEF_CHECK(ctx, derive_commit_id(execution, action, ActionGeneration{1}, ProgressGeneration{1})
                           .value() !=
                       derive_side_effect_id(execution, action, ActionGeneration{1}).value());
}

PEF_TEST(policy, validation_refuses_unusable_policies) {
    PEF_PHASE(ctx, CREATE);
    ExecutionPolicy policy = default_policy(PolicyId{7});
    PEF_CHECK(ctx, !validate_policy(policy).has_value());
    const std::uint64_t digest = policy_digest(policy);
    PEF_CHECK(ctx, digest == policy_digest(policy));

    PEF_PHASE(ctx, VERIFY);
    ExecutionPolicy no_id = policy;
    no_id.id = PolicyId{};
    PEF_CHECK(ctx, validate_policy(no_id).has_value());
    ExecutionPolicy zero_checkpoints = policy;
    zero_checkpoints.max_checkpoints_per_execution = 0;
    PEF_CHECK(ctx, validate_policy(zero_checkpoints).has_value());
    ExecutionPolicy contradictory = policy;
    contradictory.replay_allowance = ReplayAllowance::ForbidReplay;
    contradictory.allow_recompute = true;
    PEF_CHECK(ctx, validate_policy(contradictory).has_value());
    ExecutionPolicy changed = policy;
    changed.max_replay_depth = 9;
    PEF_CHECK(ctx, policy_digest(changed) != digest);
}

PEF_TEST(recovery, decisions_are_classified_by_evidence_not_guesswork) {
    PEF_PHASE(ctx, SETUP);
    ExecutionRecord execution;
    execution.id = ExecutionId{0x11};
    execution.generation = ExecutionGeneration{1};
    execution.policy_generation = PolicyGeneration{1};
    execution.lifecycle = Lifecycle::RecoveryRequired;
    ExecutionPolicy policy = default_policy(PolicyId{1});
    policy.generation = PolicyGeneration{1};

    RecoveryInput input;
    input.execution = &execution;
    input.policy = &policy;
    input.epoch = CoordinatorEpoch{2};

    PEF_PHASE(ctx, RECOVER);
    RecoveryPlan plan = classify_recovery(input);
    PEF_CHECK(ctx, plan.decision == RecoveryDecision::ResumeFromCurrent);

    ActionRecord action;
    action.id = ActionId{0x22};
    action.generation = ActionGeneration{1};
    action.sequence = 3;
    action.effect_class = SideEffectClass::NonRepeatable;
    action.status = ActionStatus::InFlight;
    input.in_flight = &action;
    plan = classify_recovery(input);
    PEF_CHECK(ctx, plan.decision == RecoveryDecision::AmbiguousCompletion);

    action.status = ActionStatus::EffectApplied;
    plan = classify_recovery(input);
    PEF_CHECK(ctx, plan.decision == RecoveryDecision::ResumeFromCurrent);
    PEF_CHECK(ctx, plan.commit_from_receipt);

    action.status = ActionStatus::EffectNotApplied;
    plan = classify_recovery(input);
    PEF_CHECK(ctx, plan.decision == RecoveryDecision::ReplaySafeAction);

    action.status = ActionStatus::InFlight;
    action.effect_class = SideEffectClass::Pure;
    plan = classify_recovery(input);
    PEF_CHECK(ctx, plan.decision == RecoveryDecision::ReplaySafeAction);

    action.effect_class = SideEffectClass::Unknown;
    policy.ambiguity_handling = AmbiguityHandling::ManualResolutionRequired;
    plan = classify_recovery(input);
    PEF_CHECK(ctx, plan.decision == RecoveryDecision::ManualResolutionRequired);

    PEF_PHASE(ctx, VERIFY);
    ExecutionRecord terminal = execution;
    terminal.lifecycle = Lifecycle::Completed;
    input.in_flight = nullptr;
    input.execution = &terminal;
    plan = classify_recovery(input);
    PEF_CHECK(ctx, plan.decision == RecoveryDecision::Terminal);

    input.execution = &execution;
    input.binding_mismatch = true;
    plan = classify_recovery(input);
    PEF_CHECK(ctx, plan.decision == RecoveryDecision::RevalidateBindings);
    policy.stale_binding_behavior = StaleBindingBehavior::Refuse;
    plan = classify_recovery(input);
    PEF_CHECK(ctx, plan.decision == RecoveryDecision::Unsupported);
}

PEF_TEST(recovery, failed_execution_needs_explicit_policy) {
    PEF_PHASE(ctx, SETUP);
    ExecutionRecord execution;
    execution.id = ExecutionId{0x31};
    execution.generation = ExecutionGeneration{1};
    execution.lifecycle = Lifecycle::Failed;
    ExecutionPolicy policy = default_policy(PolicyId{1});
    RecoveryInput input;
    input.execution = &execution;
    input.policy = &policy;
    input.epoch = CoordinatorEpoch{1};

    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, classify_recovery(input).decision == RecoveryDecision::Terminal);
    policy.allow_recovery_from_failed = true;
    PEF_CHECK(ctx, classify_recovery(input).decision == RecoveryDecision::ResumeFromCurrent);
}

PEF_TEST(version, identity_is_reported) {
    PEF_PHASE(ctx, VERIFY);
    PEF_CHECK(ctx, version_string() == "1.0.0");
    PEF_CHECK(ctx, build_info_string().find("PersistentExecutionFabric 1.0.0") == 0);
    PEF_CHECK(ctx, kProtocolVersion == 1);
    PEF_CHECK(ctx, kPersistenceSchemaVersion == 1);
}
