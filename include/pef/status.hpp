// Persistent Execution Fabric - status and deterministic refusal codes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace pef {

// Every refusal the fabric can produce is a named code. Refusals are
// deterministic: the same durable state plus the same request yields the same
// code. "Unknown failure" is not an acceptable outcome.
enum class Code : unsigned {
    Ok = 0,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    IllegalTransition,
    StaleEpoch,
    StaleWorkerBoot,
    StaleLease,
    StaleExecutionGeneration,
    StaleIncarnation,
    StaleContinuation,
    StaleCheckpointGeneration,
    StalePolicyGeneration,
    StaleBindingGeneration,
    StaleRequest,
    DuplicateRequest,
    DuplicateCommit,
    AmbiguousCompletion,
    UnresolvedAmbiguity,
    AuthorityRevoked,
    Fenced,
    LifecycleRefused,
    PolicyRefused,
    Unsupported,
    LimitExceeded,
    CorruptState,
    TruncatedState,
    PersistenceFailure,
    TransportFailure,
    ProtocolViolation,
    ShuttingDown,
    Internal,
};

[[nodiscard]] std::string_view code_name(Code code) noexcept;

// Highest valid ordinal plus one. Used by the defensive decoder to reject a
// refusal code it does not understand instead of coercing it.
inline constexpr unsigned kCodeCount = 32;
[[nodiscard]] constexpr bool pef_valid_enum(Code code) noexcept {
    return static_cast<unsigned>(code) < kCodeCount;
}

class Status {
public:
    Status() noexcept = default;
    Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}

    [[nodiscard]] static Status success() noexcept { return Status{}; }
    [[nodiscard]] static Status failure(Code code, std::string message) {
        return Status{code, std::move(message)};
    }

    [[nodiscard]] bool ok() const noexcept { return code_ == Code::Ok; }
    [[nodiscard]] Code code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    // "ok" or "<code_name>: <message>". Message text is diagnostic only; no
    // decision in the fabric is made from message text.
    [[nodiscard]] std::string to_string() const;

private:
    Code code_ = Code::Ok;
    std::string message_;
};

// Convenience constructors used throughout the runtime.
[[nodiscard]] inline Status ok_status() { return Status{}; }
[[nodiscard]] inline Status err(Code code, std::string message) {
    return Status::failure(code, std::move(message));
}

// Propagate a failing status from an expression.
#define PEF_TRY(expr)                     \
    do {                                  \
        ::pef::Status pef_try_status_ = (expr); \
        if (!pef_try_status_.ok()) {      \
            return pef_try_status_;       \
        }                                 \
    } while (false)

}  // namespace pef
