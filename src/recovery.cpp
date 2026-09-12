// Persistent Execution Fabric - recovery classification.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/recovery.hpp"

namespace pef {
namespace {

[[nodiscard]] std::string ordinal_text(std::uint64_t value) {
    return std::to_string(value);
}

}  // namespace

std::string_view recovery_decision_name(RecoveryDecision decision) noexcept {
    switch (decision) {
        case RecoveryDecision::ResumeFromCurrent: return "RESUME_FROM_CURRENT";
        case RecoveryDecision::ResumeFromCheckpoint: return "RESUME_FROM_CHECKPOINT";
        case RecoveryDecision::ReplaySafeAction: return "REPLAY_SAFE_ACTION";
        case RecoveryDecision::RevalidateBindings: return "REVALIDATE_BINDINGS";
        case RecoveryDecision::Recompute: return "RECOMPUTE";
        case RecoveryDecision::Compensate: return "COMPENSATE";
        case RecoveryDecision::AmbiguousCompletion: return "AMBIGUOUS_COMPLETION";
        case RecoveryDecision::ManualResolutionRequired: return "MANUAL_RESOLUTION_REQUIRED";
        case RecoveryDecision::Unsupported: return "UNSUPPORTED";
        case RecoveryDecision::Terminal: return "TERMINAL";
    }
    return "UNKNOWN_DECISION";
}

std::optional<RecoveryDecision> parse_recovery_decision(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i < kRecoveryDecisionCount; ++i) {
        const auto candidate = static_cast<RecoveryDecision>(i);
        if (recovery_decision_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool recovery_allows_automatic_continue(RecoveryDecision decision) noexcept {
    switch (decision) {
        case RecoveryDecision::ResumeFromCurrent:
        case RecoveryDecision::ResumeFromCheckpoint:
        case RecoveryDecision::ReplaySafeAction:
        case RecoveryDecision::Recompute:
            return true;
        default:
            return false;
    }
}

bool recovery_requires_operator(RecoveryDecision decision) noexcept {
    switch (decision) {
        case RecoveryDecision::AmbiguousCompletion:
        case RecoveryDecision::ManualResolutionRequired:
        case RecoveryDecision::Unsupported:
        case RecoveryDecision::RevalidateBindings:
        case RecoveryDecision::Compensate:
            return true;
        default:
            return false;
    }
}

RecoveryPlan classify_recovery(const RecoveryInput& input) {
    RecoveryPlan plan;
    const ExecutionRecord* execution = input.execution;
    if (execution == nullptr) {
        plan.decision = RecoveryDecision::Unsupported;
        plan.explanation = "no durable execution record";
        return plan;
    }
    const ExecutionPolicy* policy = input.policy;

    plan.execution = execution->id;
    plan.execution_generation = execution->generation;
    plan.epoch = input.epoch;
    plan.continuation = execution->continuation;
    plan.continuation_generation = execution->continuation_generation;
    plan.checkpoint = execution->checkpoint;
    plan.checkpoint_generation = execution->checkpoint_generation;
    plan.ambiguity = execution->ambiguity;
    plan.ambiguity_generation = execution->ambiguity_generation;

    const Lifecycle lifecycle = execution->lifecycle;

    // 1. Terminal lifecycle states never continue.
    if (is_terminal_state(lifecycle)) {
        plan.decision = RecoveryDecision::Terminal;
        plan.explanation = std::string("lifecycle is ") + std::string(lifecycle_name(lifecycle));
        return plan;
    }

    // 2. Failed executions continue only when policy explicitly permits it.
    if (lifecycle == Lifecycle::Failed &&
        (policy == nullptr || !policy->allow_recovery_from_failed)) {
        plan.decision = RecoveryDecision::Terminal;
        plan.explanation = "execution failed and policy does not allow recovery from FAILED";
        return plan;
    }

    if (execution->blocked) {
        // A durable frontier block is the runtime's own statement that progress
        // beyond an ordinal is not permitted yet.
        if (input.ambiguity != nullptr && ambiguity_is_open(input.ambiguity->state)) {
            plan.action = input.ambiguity->action;
            plan.action_generation = input.ambiguity->action_generation;
            plan.decision = input.ambiguity->state == AmbiguityState::ManualResolutionRequired
                                ? RecoveryDecision::ManualResolutionRequired
                                : RecoveryDecision::AmbiguousCompletion;
            plan.explanation = "progress above ordinal " +
                               ordinal_text(execution->blocked_above_ordinal) +
                               " is blocked by an unresolved ambiguity";
            return plan;
        }
    }

    // 3. Stale external bindings must be resolved before authority is reissued.
    if (input.binding_mismatch) {
        if (policy != nullptr && policy->stale_binding_behavior == StaleBindingBehavior::Refuse) {
            plan.decision = RecoveryDecision::Unsupported;
            plan.explanation = "a required binding generation changed and policy refuses revalidation";
            return plan;
        }
        plan.decision = RecoveryDecision::RevalidateBindings;
        plan.explanation = "a required binding generation changed; the continuation requires revalidation";
        return plan;
    }

    // 4. An open ambiguity dominates every other consideration.
    if (input.ambiguity != nullptr && ambiguity_is_open(input.ambiguity->state)) {
        plan.action = input.ambiguity->action;
        plan.action_generation = input.ambiguity->action_generation;
        plan.decision = input.ambiguity->state == AmbiguityState::ManualResolutionRequired
                            ? RecoveryDecision::ManualResolutionRequired
                            : RecoveryDecision::AmbiguousCompletion;
        plan.explanation = std::string("ambiguity is ") +
                           std::string(ambiguity_state_name(input.ambiguity->state)) +
                           " for action ordinal " + ordinal_text(input.ambiguity->action_ordinal);
        return plan;
    }

    // 5. A referenced checkpoint that cannot be interpreted is a hard stop.
    if (input.checkpoint_unusable) {
        plan.decision = RecoveryDecision::Unsupported;
        plan.explanation = "a referenced checkpoint is missing, corrupt, or unsupported";
        return plan;
    }

    // 6. An action that never reached a sealed state decides how execution may
    //    proceed. The side-effect class and the durable receipt are the only
    //    inputs; nothing here guesses.
    if (input.in_flight != nullptr) {
        const ActionRecord& action = *input.in_flight;
        plan.action = action.id;
        plan.action_generation = action.generation;
        const SideEffectClass cls = action.effect_class;

        if (action.status == ActionStatus::EffectApplied ||
            action.status == ActionStatus::CompletedUnacknowledged) {
            // The durable receipt states the physical effect applied. Committing
            // the logical progress is safe and requires no worker.
            if (cls == SideEffectClass::Compensatable && policy != nullptr &&
                policy->ambiguity_handling == AmbiguityHandling::RequireCompensation) {
                plan.decision = RecoveryDecision::Compensate;
                plan.explanation = "effect applied but policy requires compensation for COMPENSATABLE actions";
                return plan;
            }
            plan.decision = RecoveryDecision::ResumeFromCurrent;
            plan.commit_from_receipt = true;
            plan.explanation = "a durable side-effect receipt states the action applied; "
                               "the logical commit may proceed without replay";
            return plan;
        }

        if (action.status == ActionStatus::EffectNotApplied) {
            // A durable receipt states the effect did not apply. Replay is
            // therefore a re-execution of work that never happened.
            if (policy != nullptr && policy->replay_allowance == ReplayAllowance::ForbidReplay) {
                plan.decision = RecoveryDecision::Unsupported;
                plan.explanation = "effect did not apply but policy forbids replay";
                return plan;
            }
            plan.decision = RecoveryDecision::ReplaySafeAction;
            plan.explanation = "a durable side-effect receipt states the action did not apply; "
                               "the action may be re-driven";
            return plan;
        }

        if (action.status == ActionStatus::Committed) {
            // Should not be reachable: a committed action is not in flight. Kept
            // explicit so the classifier never silently falls through.
            plan.decision = RecoveryDecision::ResumeFromCurrent;
            plan.explanation = "the interrupted action already holds a durable logical commit";
            return plan;
        }

        if (action.status == ActionStatus::Compensated) {
            plan.decision = RecoveryDecision::ResumeFromCurrent;
            plan.explanation = "the interrupted action was compensated; execution may continue";
            return plan;
        }

        if (action.status == ActionStatus::Abandoned) {
            plan.decision = RecoveryDecision::ResumeFromCurrent;
            plan.explanation = "the interrupted action was explicitly abandoned; execution may continue";
            return plan;
        }

        // An explicit resolution that the effect did not apply is a durable
        // statement, so re-driving the same logical action is a re-execution of
        // work that never happened.
        if (input.ambiguity != nullptr &&
            input.ambiguity->state == AmbiguityState::ResolvedNotApplied &&
            input.ambiguity->action == action.id &&
            input.ambiguity->action_generation == action.generation) {
            if (policy != nullptr && policy->replay_allowance == ReplayAllowance::ForbidReplay) {
                plan.decision = RecoveryDecision::Unsupported;
                plan.explanation =
                    "the effect was resolved as not applied but policy forbids replay";
                return plan;
            }
            plan.decision = RecoveryDecision::ReplaySafeAction;
            plan.explanation =
                "an explicit resolution established that the effect did not apply; the same "
                "logical action may be re-driven under a new generation";
            return plan;
        }

        // No usable receipt. The side-effect class decides whether automatic
        // repetition is permitted at all.
        if (policy != nullptr && policy->replay_allowance == ReplayAllowance::ForbidReplay) {
            plan.decision = RecoveryDecision::Unsupported;
            plan.explanation = "action was interrupted in flight and policy forbids replay";
            return plan;
        }
        if (auto_replayable(cls)) {
            plan.decision = RecoveryDecision::ReplaySafeAction;
            plan.explanation = std::string("action was interrupted in flight; class ") +
                               std::string(side_effect_class_name(cls)) +
                               " permits re-driving the same logical action";
            return plan;
        }
        if (cls == SideEffectClass::Compensatable) {
            if (policy != nullptr &&
                policy->ambiguity_handling == AmbiguityHandling::RequireCompensation) {
                plan.decision = RecoveryDecision::Compensate;
                plan.explanation = "action was interrupted in flight and policy requires compensation";
                return plan;
            }
        }
        // NON_REPEATABLE, UNKNOWN, and any COMPENSATABLE case that policy does
        // not resolve become explicit ambiguity.
        plan.decision = (policy != nullptr &&
                         policy->ambiguity_handling == AmbiguityHandling::ManualResolutionRequired)
                            ? RecoveryDecision::ManualResolutionRequired
                            : RecoveryDecision::AmbiguousCompletion;
        plan.explanation = std::string("action was interrupted in flight with class ") +
                           std::string(side_effect_class_name(cls)) +
                           " and no durable receipt; the physical outcome is unknown";
        return plan;
    }

    // 7. No interrupted action. A validated checkpoint is the preferred durable
    //    point when policy prefers one; otherwise the progress frontier itself
    //    is a legal continuation point.
    const bool prefer_checkpoint =
        policy != nullptr &&
        policy->recovery_preference != RecoveryPreference::RequireManualSelection;

    if (prefer_checkpoint && input.current_checkpoint != nullptr &&
        checkpoint_can_seed_continuation(input.current_checkpoint->state)) {
        plan.decision = RecoveryDecision::ResumeFromCurrent;
        plan.checkpoint = input.current_checkpoint->id;
        plan.checkpoint_generation = input.current_checkpoint->generation;
        plan.explanation = "the current checkpoint is verified and binds the current generations";
        return plan;
    }

    if (prefer_checkpoint && !input.candidates.empty()) {
        const CheckpointRecord* chosen = input.candidates.front();
        if (policy != nullptr &&
            policy->recovery_preference ==
                RecoveryPreference::PreferCheckpointBeforeInterruption) {
            // The newest candidate at or before the durable frontier ordinal.
            for (const CheckpointRecord* candidate : input.candidates) {
                if (candidate->effect_boundary_ordinal <= execution->action_frontier) {
                    chosen = candidate;
                    break;
                }
            }
        }
        plan.decision = RecoveryDecision::ResumeFromCheckpoint;
        plan.checkpoint = chosen->id;
        plan.checkpoint_generation = chosen->generation;
        plan.explanation = "no action is in flight; resuming from checkpoint generation " +
                           ordinal_text(chosen->generation.value());
        return plan;
    }

    if (policy != nullptr && policy->recovery_preference ==
                                 RecoveryPreference::RequireManualSelection) {
        plan.decision = RecoveryDecision::ManualResolutionRequired;
        plan.explanation = "policy requires an explicit continuation selection";
        return plan;
    }

    plan.decision = RecoveryDecision::ResumeFromCurrent;
    plan.explanation = "no action is in flight; the durable progress frontier is a legal "
                       "continuation point";
    return plan;
}

}  // namespace pef
