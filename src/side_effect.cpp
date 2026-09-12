// Persistent Execution Fabric - side-effect classification.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/side_effect.hpp"

namespace pef {

std::string_view side_effect_class_name(SideEffectClass cls) noexcept {
    switch (cls) {
        case SideEffectClass::Pure: return "PURE";
        case SideEffectClass::Idempotent: return "IDEMPOTENT";
        case SideEffectClass::RepeatableWithKey: return "REPEATABLE_WITH_KEY";
        case SideEffectClass::Compensatable: return "COMPENSATABLE";
        case SideEffectClass::NonRepeatable: return "NON_REPEATABLE";
        case SideEffectClass::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::optional<SideEffectClass> parse_side_effect_class(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i < kSideEffectClassCount; ++i) {
        const auto candidate = static_cast<SideEffectClass>(i);
        if (side_effect_class_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool auto_replayable(SideEffectClass cls) noexcept {
    switch (cls) {
        case SideEffectClass::Pure:
        case SideEffectClass::Idempotent:
        case SideEffectClass::RepeatableWithKey:
            return true;
        case SideEffectClass::Compensatable:
        case SideEffectClass::NonRepeatable:
        case SideEffectClass::Unknown:
            return false;
    }
    return false;
}

bool requires_request_key(SideEffectClass cls) noexcept {
    return cls == SideEffectClass::RepeatableWithKey;
}

bool forbids_automatic_retry(SideEffectClass cls) noexcept {
    return cls == SideEffectClass::NonRepeatable || cls == SideEffectClass::Unknown;
}

bool requires_compensation(SideEffectClass cls) noexcept {
    return cls == SideEffectClass::Compensatable;
}

bool physically_repeatable(SideEffectClass cls) noexcept {
    switch (cls) {
        case SideEffectClass::Pure:
        case SideEffectClass::Idempotent:
        case SideEffectClass::RepeatableWithKey:
        case SideEffectClass::Compensatable:
            return true;
        case SideEffectClass::NonRepeatable:
        case SideEffectClass::Unknown:
            return false;
    }
    return false;
}

std::string_view evidence_kind_name(EvidenceKind kind) noexcept {
    switch (kind) {
        case EvidenceKind::None: return "NONE";
        case EvidenceKind::Real: return "REAL";
        case EvidenceKind::Synthetic: return "SYNTHETIC";
        case EvidenceKind::Unsupported: return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i <= 3; ++i) {
        const auto candidate = static_cast<EvidenceKind>(i);
        if (evidence_kind_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string_view action_status_name(ActionStatus status) noexcept {
    switch (status) {
        case ActionStatus::Prepared: return "PREPARED";
        case ActionStatus::InFlight: return "IN_FLIGHT";
        case ActionStatus::EffectApplied: return "EFFECT_APPLIED";
        case ActionStatus::EffectNotApplied: return "EFFECT_NOT_APPLIED";
        case ActionStatus::CompletedUnacknowledged: return "COMPLETED_UNACKNOWLEDGED";
        case ActionStatus::Committed: return "COMMITTED";
        case ActionStatus::Failed: return "FAILED";
        case ActionStatus::Ambiguous: return "AMBIGUOUS";
        case ActionStatus::Compensated: return "COMPENSATED";
        case ActionStatus::Abandoned: return "ABANDONED";
    }
    return "UNKNOWN_STATUS";
}

std::optional<ActionStatus> parse_action_status(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i < kActionStatusCount; ++i) {
        const auto candidate = static_cast<ActionStatus>(i);
        if (action_status_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool action_is_physically_sealed(ActionStatus status) noexcept {
    switch (status) {
        case ActionStatus::Committed:
        case ActionStatus::Ambiguous:
        case ActionStatus::Compensated:
        case ActionStatus::Abandoned:
        case ActionStatus::CompletedUnacknowledged:
        case ActionStatus::EffectApplied:
            return true;
        default:
            return false;
    }
}

std::string_view ambiguity_state_name(AmbiguityState state) noexcept {
    switch (state) {
        case AmbiguityState::Open: return "OPEN";
        case AmbiguityState::ResolvedApplied: return "RESOLVED_APPLIED";
        case AmbiguityState::ResolvedNotApplied: return "RESOLVED_NOT_APPLIED";
        case AmbiguityState::Accepted: return "ACCEPTED";
        case AmbiguityState::Abandoned: return "ABANDONED";
        case AmbiguityState::ManualResolutionRequired: return "MANUAL_RESOLUTION_REQUIRED";
    }
    return "UNKNOWN_AMBIGUITY";
}

std::optional<AmbiguityState> parse_ambiguity_state(std::string_view text) noexcept {
    for (std::uint8_t i = 0; i < kAmbiguityStateCount; ++i) {
        const auto candidate = static_cast<AmbiguityState>(i);
        if (ambiguity_state_name(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool ambiguity_is_open(AmbiguityState state) noexcept {
    return state == AmbiguityState::Open || state == AmbiguityState::ManualResolutionRequired;
}

}  // namespace pef
