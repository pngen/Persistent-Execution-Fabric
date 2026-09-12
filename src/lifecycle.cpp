// Persistent Execution Fabric - lifecycle transition table.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/lifecycle.hpp"

#include <array>
#include <cstddef>

namespace pef {
namespace {

using Table = std::array<std::array<bool, kLifecycleCount>, kLifecycleCount>;

constexpr std::uint8_t idx(Lifecycle s) noexcept { return static_cast<std::uint8_t>(s); }

template <std::size_t N>
constexpr void allow(Table& t, Lifecycle from, const Lifecycle (&targets)[N]) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        t[idx(from)][idx(targets[i])] = true;
    }
}

constexpr Table build_table() noexcept {
    Table t{};

    // Created: an execution has durable identity but no authority yet.
    allow(t, Lifecycle::Created, {Lifecycle::Ready, Lifecycle::Suspended, Lifecycle::Fenced,
                                  Lifecycle::Cancelled, Lifecycle::Retired, Lifecycle::Failed});

    // Ready: admitted, bindable, not yet executing.
    allow(t, Lifecycle::Ready, {Lifecycle::Running, Lifecycle::Suspended, Lifecycle::Fenced,
                             Lifecycle::Cancelled, Lifecycle::Retired, Lifecycle::Failed,
                             Lifecycle::RecoveryRequired});

    // Running: exactly the states reachable while authority is exercisable.
    allow(t, Lifecycle::Running, {Lifecycle::Checkpointing, Lifecycle::Suspending,
                               Lifecycle::Draining, Lifecycle::Suspended,
                               Lifecycle::RecoveryRequired, Lifecycle::Fenced,
                               Lifecycle::Failed, Lifecycle::Cancelled, Lifecycle::Completed});

    allow(t, Lifecycle::Checkpointing, {Lifecycle::Running, Lifecycle::Suspending,
                                     Lifecycle::Suspended, Lifecycle::RecoveryRequired,
                                     Lifecycle::Fenced, Lifecycle::Failed, Lifecycle::Cancelled});

    allow(t, Lifecycle::Suspending, {Lifecycle::Suspended, Lifecycle::RecoveryRequired,
                                  Lifecycle::Fenced, Lifecycle::Failed, Lifecycle::Cancelled});

    allow(t, Lifecycle::Suspended, {Lifecycle::Resuming, Lifecycle::Ready,
                                 Lifecycle::RecoveryRequired, Lifecycle::Fenced,
                                 Lifecycle::Failed, Lifecycle::Cancelled, Lifecycle::Retired});

    // RecoveryRequired is the mandatory stop after interruption. It cannot
    // reach Running directly: recovery must classify, then resume.
    allow(t, Lifecycle::RecoveryRequired, {Lifecycle::Recovering, Lifecycle::Suspended,
                                        Lifecycle::Fenced, Lifecycle::Failed,
                                        Lifecycle::Cancelled, Lifecycle::Retired});

    allow(t, Lifecycle::Recovering, {Lifecycle::Resuming, Lifecycle::RecoveryRequired,
                                  Lifecycle::Suspended, Lifecycle::Fenced, Lifecycle::Failed,
                                  Lifecycle::Cancelled});

    allow(t, Lifecycle::Resuming, {Lifecycle::Running, Lifecycle::RecoveryRequired,
                                Lifecycle::Suspended, Lifecycle::Fenced, Lifecycle::Failed,
                                Lifecycle::Cancelled});

    allow(t, Lifecycle::Draining, {Lifecycle::Completed, Lifecycle::Cancelled, Lifecycle::Failed,
                                Lifecycle::RecoveryRequired, Lifecycle::Fenced,
                                Lifecycle::Retired, Lifecycle::Suspended});

    // Completed is a logical commit point. Only retirement follows.
    allow(t, Lifecycle::Completed, {Lifecycle::Retired});

    // Failed may re-enter recovery, but only through RecoveryRequired.
    allow(t, Lifecycle::Failed, {Lifecycle::RecoveryRequired, Lifecycle::Retired,
                              Lifecycle::Cancelled, Lifecycle::Fenced});

    allow(t, Lifecycle::Cancelled, {Lifecycle::Retired});

    // Fenced: old authority is dead; the execution is not.
    allow(t, Lifecycle::Fenced, {Lifecycle::RecoveryRequired, Lifecycle::Retired,
                              Lifecycle::Cancelled});

    // Retired is terminal.
    return t;
}

constexpr Table kTransitions = build_table();

}  // namespace

std::string_view lifecycle_name(Lifecycle state) noexcept {
    switch (state) {
        case Lifecycle::Created: return "CREATED";
        case Lifecycle::Ready: return "READY";
        case Lifecycle::Running: return "RUNNING";
        case Lifecycle::Checkpointing: return "CHECKPOINTING";
        case Lifecycle::Suspending: return "SUSPENDING";
        case Lifecycle::Suspended: return "SUSPENDED";
        case Lifecycle::RecoveryRequired: return "RECOVERY_REQUIRED";
        case Lifecycle::Recovering: return "RECOVERING";
        case Lifecycle::Resuming: return "RESUMING";
        case Lifecycle::Draining: return "DRAINING";
        case Lifecycle::Completed: return "COMPLETED";
        case Lifecycle::Failed: return "FAILED";
        case Lifecycle::Cancelled: return "CANCELLED";
        case Lifecycle::Fenced: return "FENCED";
        case Lifecycle::Retired: return "RETIRED";
    }
    return "UNKNOWN_LIFECYCLE";
}

std::optional<Lifecycle> parse_lifecycle(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i < kLifecycleCount; ++i) {
        const auto candidate = static_cast<Lifecycle>(i);
        if (lifecycle_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool is_legal_transition(Lifecycle from, Lifecycle to) noexcept {
    if (idx(from) >= kLifecycleCount || idx(to) >= kLifecycleCount) {
        return false;
    }
    return kTransitions[idx(from)][idx(to)];
}

bool is_resting_state(Lifecycle state) noexcept {
    switch (state) {
        case Lifecycle::Created:
        case Lifecycle::Ready:
        case Lifecycle::Suspended:
        case Lifecycle::RecoveryRequired:
        case Lifecycle::Completed:
        case Lifecycle::Failed:
        case Lifecycle::Cancelled:
        case Lifecycle::Fenced:
        case Lifecycle::Retired:
            return true;
        default:
            return false;
    }
}

bool is_active_state(Lifecycle state) noexcept {
    switch (state) {
        case Lifecycle::Running:
        case Lifecycle::Checkpointing:
        case Lifecycle::Suspending:
        case Lifecycle::Recovering:
        case Lifecycle::Resuming:
        case Lifecycle::Draining:
            return true;
        default:
            return false;
    }
}

bool is_terminal_state(Lifecycle state) noexcept {
    switch (state) {
        case Lifecycle::Completed:
        case Lifecycle::Cancelled:
        case Lifecycle::Retired:
            return true;
        default:
            return false;
    }
}

bool admits_new_continuation(Lifecycle state) noexcept {
    switch (state) {
        case Lifecycle::Ready:
        case Lifecycle::Running:
        case Lifecycle::Checkpointing:
        case Lifecycle::Suspended:
        case Lifecycle::Resuming:
        case Lifecycle::Recovering:
        case Lifecycle::Draining:
            return true;
        default:
            return false;
    }
}

bool requires_recovery(Lifecycle state) noexcept {
    switch (state) {
        case Lifecycle::RecoveryRequired:
        case Lifecycle::Recovering:
        case Lifecycle::Fenced:
        case Lifecycle::Failed:
            return true;
        default:
            return false;
    }
}

}  // namespace pef
