// Persistent Execution Fabric - invariant auditor.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// The auditor is a pure function of durable state. It is callable from tests,
// from the CLI, and from the coordinator after recovery. A non-zero violation
// count is a product defect, not a warning.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pef/persistence.hpp"

namespace pef {

struct AuditFinding {
    // Stable machine-readable identifier. Never reworded between releases for
    // the same invariant.
    std::string code;
    std::string detail;
    ExecutionId execution;
    bool fatal = true;
};

struct AuditCounts {
    std::uint64_t executions = 0;
    std::uint64_t actions = 0;
    std::uint64_t progress = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t continuations = 0;
    std::uint64_t leases = 0;
    std::uint64_t replays = 0;
    std::uint64_t ambiguities = 0;
    std::uint64_t commits = 0;
    std::uint64_t recoveries = 0;
    std::uint64_t policies = 0;
};

struct AuditReport {
    AuditCounts counts;
    std::vector<AuditFinding> findings;
    CoordinatorEpoch epoch;
    StoreId store;
    std::uint64_t sequence = 0;

    [[nodiscard]] bool clean() const noexcept { return findings.empty(); }
    [[nodiscard]] std::size_t violations() const noexcept { return findings.size(); }
    // Deterministic text rendering: findings are emitted in a stable order.
    [[nodiscard]] std::string render() const;
    // Stable machine-readable digest of the report's decision-relevant content.
    [[nodiscard]] std::uint64_t digest() const;
};

// Audits a complete durable state image.
[[nodiscard]] AuditReport audit_state(const DurableState& state);

}  // namespace pef
