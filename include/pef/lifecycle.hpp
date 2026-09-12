// Persistent Execution Fabric - execution lifecycle and legal transitions.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// The lifecycle is the durable statement of what an execution is allowed to do
// next. Every transition in this file is explicit: there are no implicit or
// self transitions. A state that is not reachable through this table cannot be
// reached by any code path in the fabric.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace pef {

enum class Lifecycle : std::uint8_t {
    Created = 0,
    Ready = 1,
    Running = 2,
    Checkpointing = 3,
    Suspending = 4,
    Suspended = 5,
    RecoveryRequired = 6,
    Recovering = 7,
    Resuming = 8,
    Draining = 9,
    Completed = 10,
    Failed = 11,
    Cancelled = 12,
    Fenced = 13,
    Retired = 14,
};

inline constexpr std::uint8_t kLifecycleCount = 15;

[[nodiscard]] std::string_view lifecycle_name(Lifecycle state) noexcept;
[[nodiscard]] std::optional<Lifecycle> parse_lifecycle(std::string_view text) noexcept;

// True only for the exact pair (from, to) present in the transition table.
// Self transitions are always false.
[[nodiscard]] bool is_legal_transition(Lifecycle from, Lifecycle to) noexcept;

// True when the state is a stable resting state that a coordinator restart may
// leave in place without reclassification.
[[nodiscard]] bool is_resting_state(Lifecycle state) noexcept;

// True when the state means "authority is currently exercisable", i.e. work may
// be dispatched. Process death in one of these states forces reclassification.
[[nodiscard]] bool is_active_state(Lifecycle state) noexcept;

// Terminal states cannot transition anywhere. Failed is deliberately excluded:
// a failed execution may still enter recovery when policy permits.
[[nodiscard]] bool is_terminal_state(Lifecycle state) noexcept;

// True when a fresh continuation may be issued in this state.
[[nodiscard]] bool admits_new_continuation(Lifecycle state) noexcept;

// True when the state requires explicit recovery before work may run again.
[[nodiscard]] bool requires_recovery(Lifecycle state) noexcept;

}  // namespace pef
