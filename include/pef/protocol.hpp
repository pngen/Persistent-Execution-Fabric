// Persistent Execution Fabric - control-plane wire protocol.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// The protocol is a bounded, framed, request/reply protocol. Every frame is
// independently checksummed so that a truncated or corrupted frame is refused
// rather than partially interpreted. Frame limits are protocol limits and are
// deliberately independent of the persistence limits.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pef/audit.hpp"
#include "pef/bytes.hpp"
#include "pef/runtime.hpp"
#include "pef/status.hpp"
#include "pef/version.hpp"

namespace pef {

// ---------------------------------------------------------------------------
// Message types. The reply to a request is the request ordinal with the reply
// bit set.
// ---------------------------------------------------------------------------
enum class MessageType : std::uint16_t {
    Hello = 1,
    Ping = 2,
    Shutdown = 3,

    CreateExecution = 10,
    BindWorker = 11,
    Start = 12,
    BeginAction = 13,
    CompleteAction = 14,
    FailAction = 15,
    ReportSideEffect = 16,
    RegisterCheckpoint = 17,
    SuspendBegin = 18,
    SuspendCommit = 19,
    Resume = 20,

    Fence = 30,
    Cancel = 31,
    Retire = 32,
    Drain = 33,
    CompleteExecution = 34,
    AdvanceGeneration = 35,
    UpdatePolicy = 36,
    Classify = 37,
    Recover = 38,
    ResolveAmbiguity = 39,
    RevalidateContinuation = 40,

    Query = 50,
    ListExecutions = 51,
    Snapshot = 52,
    Audit = 53,
};

inline constexpr std::uint16_t kReplyBit = 0x8000u;
inline constexpr std::uint16_t kMaxMessageType = 53;

[[nodiscard]] std::string_view message_type_name(MessageType type) noexcept;
[[nodiscard]] std::optional<MessageType> parse_message_type(std::uint16_t raw) noexcept;
[[nodiscard]] constexpr bool pef_valid_enum(MessageType v) noexcept {
    return static_cast<std::uint16_t>(v) >= 1 && static_cast<std::uint16_t>(v) <= kMaxMessageType;
}

// ---------------------------------------------------------------------------
// Frame layout.
//
//   offset  size  field
//   0       4     magic 'P','E','F','P'
//   4       2     protocol version
//   6       2     message type (reply bit may be set)
//   8       8     request identity
//   16      4     payload length
//   20      4     flags (reserved, must be zero)
//   24      4     header crc (low 32 bits of crc64 over bytes 0..23)
//   28      n     payload
//   28+n    8     frame crc (crc64 over the header and the payload)
//
// The trailing frame checksum covers the header and the payload as one
// contiguous run, so a torn or altered frame is refused before it is
// interpreted.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kFrameHeaderSize = 28;
inline constexpr std::size_t kFrameTrailerSize = 8;
inline constexpr std::uint32_t kFrameMagic = 0x50464550u;  // 'PEFP' little endian
// 1 MiB. A frame larger than this is refused before any allocation.
inline constexpr std::uint32_t kMaxFramePayload = 1u * 1024u * 1024u;

struct Frame {
    std::uint16_t version = kProtocolVersion;
    MessageType type = MessageType::Ping;
    RequestId request;
    std::uint32_t flags = 0;
    Bytes payload;

    [[nodiscard]] bool is_reply() const noexcept {
        return (static_cast<std::uint16_t>(type) & kReplyBit) != 0;
    }
    [[nodiscard]] MessageType base_type() const noexcept {
        return static_cast<MessageType>(static_cast<std::uint16_t>(type) & ~kReplyBit);
    }
};

// Encodes a complete frame into one buffer.
[[nodiscard]] Bytes encode_frame(const Frame& frame);
// Decodes a frame from a complete buffer. Rejects a bad magic, an unsupported
// version, an unknown type, a mismatched length, and any checksum failure.
[[nodiscard]] bool decode_frame(const Bytes& bytes, Frame& out, std::string& reason);
// Decodes just the header, reporting the payload length the frame declares.
[[nodiscard]] bool decode_frame_header(const std::uint8_t* header, std::size_t size, Frame& out,
                                        std::string& reason);

// ---------------------------------------------------------------------------
// Wire payloads. Requests and results reuse the runtime's own structures so the
// client and the coordinator cannot disagree about a field.
// ---------------------------------------------------------------------------
struct StatusPayload {
    Code code = Code::Ok;
    std::string message;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, StatusPayload& out);
};

struct HelloRequest {
    std::string client_name;
    std::string client_version;
    // Worker identity, when the caller is a worker rather than an operator.
    WorkerId worker;
    WorkerBootId boot;
    bool as_worker = false;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, HelloRequest& out);
};

struct HelloReply {
    StoreId store;
    CoordinatorEpoch epoch;
    SessionId session;
    std::string coordinator_version;
    std::uint16_t protocol_version = kProtocolVersion;
    std::uint32_t persistence_schema = kPersistenceSchemaVersion;
    bool resumed_from_durable_state = false;
    std::uint64_t journal_sequence = 0;

    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, HelloReply& out);
};

struct PingRequest {
    std::uint64_t token = 0;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, PingRequest& out);
};

struct PingReply {
    std::uint64_t token = 0;
    CoordinatorEpoch epoch;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, PingReply& out);
};

struct SimpleReply {
    StatusPayload status;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, SimpleReply& out);
};

// Every operation reply begins with a status so that a client can always tell a
// refusal from a result, whatever the operation.
struct CreateExecutionReply {
    StatusPayload status;
    CreateExecutionResult result;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, CreateExecutionReply& out);
};

struct BindWorkerReply {
    StatusPayload status;
    BindWorkerResult result;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, BindWorkerReply& out);
};

struct BeginActionReply {
    StatusPayload status;
    BeginActionResult result;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, BeginActionReply& out);
};

struct CompleteActionReply {
    StatusPayload status;
    CompleteActionResult result;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, CompleteActionReply& out);
};

struct CheckpointReply {
    StatusPayload status;
    RegisterCheckpointResult result;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, CheckpointReply& out);
};

struct ResumeReply {
    StatusPayload status;
    ResumeResult result;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ResumeReply& out);
};

struct ExecutionIdRequest {
    ExecutionId execution;
    std::string reason;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ExecutionIdRequest& out);
};

struct ExecutionIdReply {
    StatusPayload status;
    ExecutionRecord execution;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ExecutionIdReply& out);
};

struct UpdatePolicyRequest {
    ExecutionId execution;
    ExecutionPolicy policy;
    bool apply = true;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, UpdatePolicyRequest& out);
};

struct ClassifyReply {
    StatusPayload status;
    RecoveryPlan plan;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ClassifyReply& out);
};

struct RecoverReply {
    StatusPayload status;
    RecoveryOutcome outcome;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, RecoverReply& out);
};

struct ResolveAmbiguityReply {
    StatusPayload status;
    ResolveAmbiguityResult result;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ResolveAmbiguityReply& out);
};

struct QueryReply {
    StatusPayload status;
    ExecutionView view;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, QueryReply& out);
};

struct ListExecutionsReply {
    StatusPayload status;
    std::vector<ExecutionRecord> executions;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ListExecutionsReply& out);
};

struct AuditReply {
    StatusPayload status;
    AuditReport report;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, AuditReply& out);
};

struct ValidateContinuationReply {
    StatusPayload status;
    ContinuationState state = ContinuationState::Invalid;
    std::string reason;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, ValidateContinuationReply& out);
};

struct SnapshotRequest {
    bool truncate_journal = false;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, SnapshotRequest& out);
};

struct SnapshotReply {
    StatusPayload status;
    std::uint64_t journal_sequence = 0;
    void encode(ByteWriter& w) const;
    [[nodiscard]] static bool decode(ByteReader& r, SnapshotReply& out);
};

// ---------------------------------------------------------------------------
// RecordTraits for every wire structure.
// ---------------------------------------------------------------------------
#define PEF_WIRE_TRAITS(Type, ...)                    \
    template <>                                       \
    struct RecordTraits<Type> {                       \
        static constexpr auto members = std::make_tuple(__VA_ARGS__); \
    }

PEF_WIRE_TRAITS(StatusPayload, &StatusPayload::code, &StatusPayload::message);
PEF_WIRE_TRAITS(CreateExecutionReply, &CreateExecutionReply::status,
                &CreateExecutionReply::result);
PEF_WIRE_TRAITS(BindWorkerReply, &BindWorkerReply::status, &BindWorkerReply::result);
PEF_WIRE_TRAITS(BeginActionReply, &BeginActionReply::status, &BeginActionReply::result);
PEF_WIRE_TRAITS(CompleteActionReply, &CompleteActionReply::status,
                &CompleteActionReply::result);
PEF_WIRE_TRAITS(CheckpointReply, &CheckpointReply::status, &CheckpointReply::result);
PEF_WIRE_TRAITS(ResumeReply, &ResumeReply::status, &ResumeReply::result);
PEF_WIRE_TRAITS(HelloRequest, &HelloRequest::client_name, &HelloRequest::client_version,
                &HelloRequest::worker, &HelloRequest::boot, &HelloRequest::as_worker);
PEF_WIRE_TRAITS(HelloReply, &HelloReply::store, &HelloReply::epoch, &HelloReply::session,
                &HelloReply::coordinator_version, &HelloReply::protocol_version,
                &HelloReply::persistence_schema, &HelloReply::resumed_from_durable_state,
                &HelloReply::journal_sequence);
PEF_WIRE_TRAITS(PingRequest, &PingRequest::token);
PEF_WIRE_TRAITS(PingReply, &PingReply::token, &PingReply::epoch);
PEF_WIRE_TRAITS(SimpleReply, &SimpleReply::status);
PEF_WIRE_TRAITS(ExecutionIdRequest, &ExecutionIdRequest::execution, &ExecutionIdRequest::reason);
PEF_WIRE_TRAITS(ExecutionIdReply, &ExecutionIdReply::status, &ExecutionIdReply::execution);
PEF_WIRE_TRAITS(UpdatePolicyRequest, &UpdatePolicyRequest::execution,
                &UpdatePolicyRequest::policy, &UpdatePolicyRequest::apply);
PEF_WIRE_TRAITS(ClassifyReply, &ClassifyReply::status, &ClassifyReply::plan);
PEF_WIRE_TRAITS(RecoverReply, &RecoverReply::status, &RecoverReply::outcome);
PEF_WIRE_TRAITS(ResolveAmbiguityReply, &ResolveAmbiguityReply::status,
                &ResolveAmbiguityReply::result);
PEF_WIRE_TRAITS(QueryReply, &QueryReply::status, &QueryReply::view);
PEF_WIRE_TRAITS(ListExecutionsReply, &ListExecutionsReply::status,
                &ListExecutionsReply::executions);
PEF_WIRE_TRAITS(AuditReply, &AuditReply::status, &AuditReply::report);
PEF_WIRE_TRAITS(ValidateContinuationReply, &ValidateContinuationReply::status,
                &ValidateContinuationReply::state, &ValidateContinuationReply::reason);
PEF_WIRE_TRAITS(SnapshotRequest, &SnapshotRequest::truncate_journal);
PEF_WIRE_TRAITS(SnapshotReply, &SnapshotReply::status, &SnapshotReply::journal_sequence);

PEF_WIRE_TRAITS(LeaseToken, &LeaseToken::execution, &LeaseToken::execution_generation,
                &LeaseToken::incarnation, &LeaseToken::incarnation_generation, &LeaseToken::lease,
                &LeaseToken::lease_generation, &LeaseToken::epoch, &LeaseToken::worker,
                &LeaseToken::boot, &LeaseToken::policy_generation);
PEF_WIRE_TRAITS(CallerContext, &CallerContext::session, &CallerContext::epoch);
PEF_WIRE_TRAITS(CreateExecutionRequest, &CreateExecutionRequest::request,
                &CreateExecutionRequest::policy);
PEF_WIRE_TRAITS(CreateExecutionResult, &CreateExecutionResult::execution,
                &CreateExecutionResult::generation, &CreateExecutionResult::policy,
                &CreateExecutionResult::policy_generation, &CreateExecutionResult::existing);
PEF_WIRE_TRAITS(BindWorkerRequest, &BindWorkerRequest::request, &BindWorkerRequest::execution,
                &BindWorkerRequest::worker, &BindWorkerRequest::boot, &BindWorkerRequest::bindings);
PEF_WIRE_TRAITS(BindWorkerResult, &BindWorkerResult::incarnation,
                &BindWorkerResult::incarnation_generation, &BindWorkerResult::lease,
                &BindWorkerResult::lease_generation, &BindWorkerResult::lifecycle,
                &BindWorkerResult::incarnation_advanced);
PEF_WIRE_TRAITS(StartRequest, &StartRequest::request, &StartRequest::token);
PEF_WIRE_TRAITS(BeginActionRequest, &BeginActionRequest::request, &BeginActionRequest::token,
                &BeginActionRequest::effect_class, &BeginActionRequest::request_key,
                &BeginActionRequest::evidence);
PEF_WIRE_TRAITS(BeginActionResult, &BeginActionResult::action, &BeginActionResult::action_generation,
                &BeginActionResult::ordinal, &BeginActionResult::attempt,
                &BeginActionResult::attempt_generation, &BeginActionResult::effect_id,
                &BeginActionResult::effect_generation);
PEF_WIRE_TRAITS(CompletionEvidence, &CompletionEvidence::effect_applied,
                &CompletionEvidence::evidence);
PEF_WIRE_TRAITS(CompleteActionRequest, &CompleteActionRequest::request,
                &CompleteActionRequest::token, &CompleteActionRequest::action,
                &CompleteActionRequest::action_generation, &CompleteActionRequest::completion);
PEF_WIRE_TRAITS(CompleteActionResult, &CompleteActionResult::commit,
                &CompleteActionResult::progress, &CompleteActionResult::progress_generation,
                &CompleteActionResult::ordinal, &CompleteActionResult::duplicate,
                &CompleteActionResult::checkpoint_due);
PEF_WIRE_TRAITS(FailActionRequest, &FailActionRequest::request, &FailActionRequest::token,
                &FailActionRequest::action, &FailActionRequest::action_generation,
                &FailActionRequest::evidence, &FailActionRequest::reason);
PEF_WIRE_TRAITS(ReportSideEffectRequest, &ReportSideEffectRequest::request,
                &ReportSideEffectRequest::token, &ReportSideEffectRequest::action,
                &ReportSideEffectRequest::action_generation, &ReportSideEffectRequest::applied,
                &ReportSideEffectRequest::evidence);
PEF_WIRE_TRAITS(RegisterCheckpointRequest, &RegisterCheckpointRequest::request,
                &RegisterCheckpointRequest::token, &RegisterCheckpointRequest::expected_checkpoint,
                &RegisterCheckpointRequest::expected_checkpoint_generation,
                &RegisterCheckpointRequest::content_size,
                &RegisterCheckpointRequest::content_hash,
                &RegisterCheckpointRequest::effect_boundary_ordinal);
PEF_WIRE_TRAITS(RegisterCheckpointResult, &RegisterCheckpointResult::checkpoint,
                &RegisterCheckpointResult::checkpoint_generation, &RegisterCheckpointResult::state,
                &RegisterCheckpointResult::continuation,
                &RegisterCheckpointResult::continuation_generation,
                &RegisterCheckpointResult::lineage_depth);
PEF_WIRE_TRAITS(ResumeRequest, &ResumeRequest::request, &ResumeRequest::token,
                &ResumeRequest::observed_bindings, &ResumeRequest::revalidate_bindings);
PEF_WIRE_TRAITS(ResumeResult, &ResumeResult::continuation_state, &ResumeResult::lifecycle,
                &ResumeResult::decision);
PEF_WIRE_TRAITS(ResolveAmbiguityRequest, &ResolveAmbiguityRequest::request,
                &ResolveAmbiguityRequest::caller, &ResolveAmbiguityRequest::execution,
                &ResolveAmbiguityRequest::ambiguity,
                &ResolveAmbiguityRequest::ambiguity_generation,
                &ResolveAmbiguityRequest::resolution, &ResolveAmbiguityRequest::evidence,
                &ResolveAmbiguityRequest::note);
PEF_WIRE_TRAITS(ResolveAmbiguityResult, &ResolveAmbiguityResult::state,
                &ResolveAmbiguityResult::committed, &ResolveAmbiguityResult::commit,
                &ResolveAmbiguityResult::progress_generation,
                &ResolveAmbiguityResult::replay_admitted, &ResolveAmbiguityResult::replay,
                &ResolveAmbiguityResult::ambiguity,
                &ResolveAmbiguityResult::ambiguity_generation, &ResolveAmbiguityResult::action,
                &ResolveAmbiguityResult::action_generation);
PEF_WIRE_TRAITS(RecoveryPlan, &RecoveryPlan::decision, &RecoveryPlan::execution,
                &RecoveryPlan::execution_generation, &RecoveryPlan::epoch, &RecoveryPlan::action,
                &RecoveryPlan::action_generation, &RecoveryPlan::checkpoint,
                &RecoveryPlan::checkpoint_generation, &RecoveryPlan::continuation,
                &RecoveryPlan::continuation_generation, &RecoveryPlan::ambiguity,
                &RecoveryPlan::ambiguity_generation, &RecoveryPlan::commit_from_receipt,
                &RecoveryPlan::explanation);
PEF_WIRE_TRAITS(RecoveryOutcome, &RecoveryOutcome::plan, &RecoveryOutcome::applied,
                &RecoveryOutcome::committed, &RecoveryOutcome::commit, &RecoveryOutcome::recovery,
                &RecoveryOutcome::recovery_generation, &RecoveryOutcome::ambiguity,
                &RecoveryOutcome::ambiguity_generation, &RecoveryOutcome::continuation,
                &RecoveryOutcome::continuation_generation);
PEF_WIRE_TRAITS(ExecutionView, &ExecutionView::found, &ExecutionView::execution,
                &ExecutionView::policy, &ExecutionView::lease, &ExecutionView::continuation,
                &ExecutionView::checkpoint, &ExecutionView::recovery, &ExecutionView::ambiguity,
                &ExecutionView::actions, &ExecutionView::checkpoints, &ExecutionView::replays,
                &ExecutionView::action_count, &ExecutionView::commit_count);
PEF_WIRE_TRAITS(AuditCounts, &AuditCounts::executions, &AuditCounts::actions,
                &AuditCounts::progress, &AuditCounts::checkpoints, &AuditCounts::continuations,
                &AuditCounts::leases, &AuditCounts::replays, &AuditCounts::ambiguities,
                &AuditCounts::commits, &AuditCounts::recoveries, &AuditCounts::policies);
PEF_WIRE_TRAITS(AuditFinding, &AuditFinding::code, &AuditFinding::detail,
                &AuditFinding::execution, &AuditFinding::fatal);
PEF_WIRE_TRAITS(AuditReport, &AuditReport::counts, &AuditReport::findings, &AuditReport::epoch,
                &AuditReport::store, &AuditReport::sequence);

#undef PEF_WIRE_TRAITS

// Encodes any wire structure into a standalone payload.
template <class T>
[[nodiscard]] Bytes encode_payload(const T& value) {
    return encode_to_bytes(value);
}

template <class T>
[[nodiscard]] bool decode_payload(const Bytes& payload, T& out) {
    return decode_from_bytes(payload, out);
}

template <class T>
[[nodiscard]] std::optional<T> decode_payload_optional(const Bytes& payload) {
    T out{};
    if (!decode_from_bytes(payload, out)) {
        return std::nullopt;
    }
    return out;
}

// Builds a reply frame carrying a refusal.
[[nodiscard]] Frame make_error_reply(RequestId request, Code code, std::string message);
// Extracts the refusal from a reply frame, when it carries one.
[[nodiscard]] bool reply_status(const Frame& frame, StatusPayload& out);

}  // namespace pef
