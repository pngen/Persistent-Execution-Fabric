// Persistent Execution Fabric - deterministic explainability rendering.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Every renderer is a pure function of durable state. Nothing consults the
// clock, the process table, pointer values, or container iteration order, so
// two runs over equivalent state produce byte-identical output.
#pragma once

#include <string>
#include <vector>

#include "pef/audit.hpp"
#include "pef/recovery.hpp"
#include "pef/runtime.hpp"

namespace pef {

// Full explanation of one execution: identity, authority, durable progress,
// checkpoints and their resumability, in-flight action, ambiguity, replay
// safety, and why automatic retry is or is not allowed.
[[nodiscard]] std::string explain_execution(const ExecutionView& view);

// One line per execution, stable ordering by identity.
[[nodiscard]] std::string render_execution_table(const std::vector<ExecutionRecord>& executions);

// Why a continuation is or is not usable, and what would have to change.
[[nodiscard]] std::string explain_continuation(const ExecutionView& view, ContinuationState state,
                                               const std::string& reason);

// Why recovery reached a decision.
[[nodiscard]] std::string explain_recovery(const RecoveryPlan& plan);

// Checkpoint resumability, including the reason a checkpoint cannot seed a
// continuation.
[[nodiscard]] std::string explain_checkpoint(const CheckpointRecord& checkpoint,
                                             const ExecutionRecord& execution);

// Audit rendering with the violation count first.
[[nodiscard]] std::string explain_audit(const AuditReport& report);

// A short, stable account of what changed when a coordinator restarted.
[[nodiscard]] std::string explain_restart(const OpenOutcome& outcome);

// Human name for an evidence label. Never inferred: the label is stored.
[[nodiscard]] std::string evidence_label(const Evidence& evidence);

}  // namespace pef
