// Persistent Execution Fabric - policy validation and digest.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/policy.hpp"

#include <string>

#include "pef/hash.hpp"

namespace pef {

std::optional<std::string_view> validate_policy(const ExecutionPolicy& policy) noexcept {
    if (!policy.id.valid()) {
        return "policy id must be non-nil";
    }
    if (policy.generation.is_none()) {
        return "policy generation must be established";
    }
    if (policy.max_checkpoints_per_execution == 0) {
        return "max_checkpoints_per_execution must be at least 1";
    }
    if (policy.max_checkpoints_per_execution > 4096) {
        return "max_checkpoints_per_execution exceeds the supported bound of 4096";
    }
    if (policy.max_replay_depth > 64) {
        return "max_replay_depth exceeds the supported bound of 64";
    }
    if (policy.max_action_history == 0) {
        return "max_action_history must be at least 1";
    }
    if (policy.max_action_history > 10000000u) {
        return "max_action_history exceeds the supported bound of 10000000";
    }
    if (policy.max_replay_records > 1000000u) {
        return "max_replay_records exceeds the supported bound of 1000000";
    }
    if (policy.max_ambiguity_records > 1000000u) {
        return "max_ambiguity_records exceeds the supported bound of 1000000";
    }
    if (policy.max_continuation_history > 1000000u) {
        return "max_continuation_history exceeds the supported bound of 1000000";
    }
    if (policy.replay_allowance == ReplayAllowance::ForbidReplay && policy.allow_recompute) {
        return "allow_recompute cannot be combined with ReplayAllowance::ForbidReplay";
    }
    if (policy.ambiguity_handling == AmbiguityHandling::RequireCompensation &&
        !policy.ambiguity_blocks_later_progress) {
        return "RequireCompensation requires ambiguity_blocks_later_progress";
    }
    return std::nullopt;
}

std::uint64_t policy_digest(const ExecutionPolicy& policy) noexcept {
    HashBuilder h;
    h << policy.id << policy.generation;
    h.u32(policy.checkpoint_interval_actions);
    h.byte(policy.allow_older_checkpoints ? 1u : 0u);
    h.u32(policy.max_checkpoints_per_execution);
    h.byte(static_cast<std::uint8_t>(policy.replay_allowance));
    h.u32(policy.max_replay_depth);
    h.byte(policy.allow_recompute ? 1u : 0u);
    h.byte(static_cast<std::uint8_t>(policy.ambiguity_handling));
    h.byte(policy.ambiguity_blocks_later_progress ? 1u : 0u);
    h.byte(static_cast<std::uint8_t>(policy.recovery_preference));
    h.byte(policy.allow_recovery_from_failed ? 1u : 0u);
    h.byte(static_cast<std::uint8_t>(policy.stale_binding_behavior));
    h.byte(policy.policy_change_invalidates_continuations ? 1u : 0u);
    h.byte(static_cast<std::uint8_t>(policy.durability));
    h.u32(policy.max_action_history);
    h.u32(policy.max_replay_records);
    h.u32(policy.max_ambiguity_records);
    h.u32(policy.max_continuation_history);
    return h.digest();
}

ExecutionPolicy default_policy(PolicyId id) {
    ExecutionPolicy policy;
    policy.id = id;
    policy.generation = PolicyGeneration{1};
    return policy;
}

}  // namespace pef
