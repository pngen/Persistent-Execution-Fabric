// Persistent Execution Fabric - status codes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/status.hpp"

namespace pef {

std::string_view code_name(Code code) noexcept {
    switch (code) {
        case Code::Ok: return "OK";
        case Code::InvalidArgument: return "INVALID_ARGUMENT";
        case Code::NotFound: return "NOT_FOUND";
        case Code::AlreadyExists: return "ALREADY_EXISTS";
        case Code::IllegalTransition: return "ILLEGAL_TRANSITION";
        case Code::StaleEpoch: return "STALE_EPOCH";
        case Code::StaleWorkerBoot: return "STALE_WORKER_BOOT";
        case Code::StaleLease: return "STALE_LEASE";
        case Code::StaleExecutionGeneration: return "STALE_EXECUTION_GENERATION";
        case Code::StaleIncarnation: return "STALE_INCARNATION";
        case Code::StaleContinuation: return "STALE_CONTINUATION";
        case Code::StaleCheckpointGeneration: return "STALE_CHECKPOINT_GENERATION";
        case Code::StalePolicyGeneration: return "STALE_POLICY_GENERATION";
        case Code::StaleBindingGeneration: return "STALE_BINDING_GENERATION";
        case Code::StaleRequest: return "STALE_REQUEST";
        case Code::DuplicateRequest: return "DUPLICATE_REQUEST";
        case Code::DuplicateCommit: return "DUPLICATE_COMMIT";
        case Code::AmbiguousCompletion: return "AMBIGUOUS_COMPLETION";
        case Code::UnresolvedAmbiguity: return "UNRESOLVED_AMBIGUITY";
        case Code::AuthorityRevoked: return "AUTHORITY_REVOKED";
        case Code::Fenced: return "FENCED";
        case Code::LifecycleRefused: return "LIFECYCLE_REFUSED";
        case Code::PolicyRefused: return "POLICY_REFUSED";
        case Code::Unsupported: return "UNSUPPORTED";
        case Code::LimitExceeded: return "LIMIT_EXCEEDED";
        case Code::CorruptState: return "CORRUPT_STATE";
        case Code::TruncatedState: return "TRUNCATED_STATE";
        case Code::PersistenceFailure: return "PERSISTENCE_FAILURE";
        case Code::TransportFailure: return "TRANSPORT_FAILURE";
        case Code::ProtocolViolation: return "PROTOCOL_VIOLATION";
        case Code::ShuttingDown: return "SHUTTING_DOWN";
        case Code::Internal: return "INTERNAL";
    }
    return "UNKNOWN_CODE";
}

std::string Status::to_string() const {
    if (ok()) {
        return "ok";
    }
    std::string out(code_name(code_));
    out += ": ";
    out += message_;
    return out;
}

}  // namespace pef
