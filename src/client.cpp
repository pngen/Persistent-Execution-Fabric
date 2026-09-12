// Persistent Execution Fabric - coordinator client.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/client.hpp"

#include <atomic>
#include <chrono>

namespace pef {
namespace {

// Mints a per-client-instance nonce. Identity mintage only: no authority
// decision anywhere consults this value.
[[nodiscard]] std::uint64_t mint_client_nonce() {
    static std::atomic<std::uint64_t> counter{0};
    const auto steady = std::chrono::steady_clock::now().time_since_epoch();
    const auto wall = std::chrono::system_clock::now().time_since_epoch();
    HashBuilder h;
    h.u64(0x3001);
    h.u64(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(steady).count()));
    h.u64(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count()));
    h.u64(counter.fetch_add(1, std::memory_order_relaxed));
    return h.digest();
}

}  // namespace

RequestId CoordinatorClient::next_request() noexcept {
    ++request_counter_;
    HashBuilder h;
    h.u64(client_nonce_);
    h.u64(request_counter_);
    return RequestId{h.digest()};
}

RequestId CoordinatorClient::peek_request() const noexcept {
    HashBuilder h;
    h.u64(client_nonce_);
    h.u64(request_counter_ + 1);
    return RequestId{h.digest()};
}

CoordinatorClient::~CoordinatorClient() { close(); }

void CoordinatorClient::close() noexcept {
    channel_.close();
    is_worker_ = false;
}

Status CoordinatorClient::connect(const ClientConfig& config) {
    net_initialize();
    Socket socket;
    PEF_TRY(connect_to(config.host, config.port, socket, config.timeout_ms));
    socket.set_no_delay(true);
    socket.set_read_timeout_ms(config.timeout_ms);
    channel_ = FrameChannel(std::move(socket));
    request_counter_ = 0;
    client_nonce_ = mint_client_nonce();
    return ok_status();
}

Status CoordinatorClient::exchange(MessageType type, RequestId request, const Bytes& payload,
                                   Frame& reply) {
    if (!channel_.valid()) {
        return err(Code::TransportFailure, "client is not connected");
    }
    Frame frame;
    frame.type = type;
    frame.request = request;
    frame.payload = payload;
    PEF_TRY(channel_.send(frame));
    std::string reason;
    PEF_TRY(channel_.receive(reply, reason));
    if (reply.base_type() != type) {
        return err(Code::ProtocolViolation,
                   "reply message type does not match the request message type");
    }
    if (reply.request != request) {
        // Responses are matched by request identity; a mismatch means the stream
        // is out of step and nothing further can be trusted on it.
        return err(Code::ProtocolViolation, "reply request identity does not match the request");
    }
    return ok_status();
}

Status CoordinatorClient::hello_as_operator(HelloReply& out) {
    HelloRequest hello;
    hello.as_worker = false;
    PEF_TRY(call_raw(MessageType::Hello, next_request(), encode_payload(hello), out));
    epoch_ = out.epoch;
    store_ = out.store;
    session_ = out.session;
    return ok_status();
}

Status CoordinatorClient::hello_as_worker(WorkerId worker, WorkerBootId boot, HelloReply& out) {
    HelloRequest hello;
    hello.as_worker = true;
    hello.worker = worker;
    hello.boot = boot;
    PEF_TRY(call_raw(MessageType::Hello, next_request(), encode_payload(hello), out));
    epoch_ = out.epoch;
    store_ = out.store;
    session_ = out.session;
    is_worker_ = true;
    return ok_status();
}

Status CoordinatorClient::ping(PingReply& out) {
    PingRequest ping;
    ping.token = request_counter_;
    PEF_TRY(call_raw(MessageType::Ping, next_request(), encode_payload(ping), out));
    // The coordinator's epoch is authoritative; a client that observes a new
    // epoch knows every token it holds is stale.
    epoch_ = out.epoch;
    return ok_status();
}

Status CoordinatorClient::request_shutdown() {
    SimpleReply out;
    return call(MessageType::Shutdown, next_request(), Bytes{}, out);
}

Status CoordinatorClient::create_execution(const ExecutionPolicy& policy,
                                           CreateExecutionResult& out) {
    CreateExecutionRequest request;
    request.request = next_request();
    request.policy = policy;
    CreateExecutionReply reply;
    PEF_TRY(call(MessageType::CreateExecution, request.request, encode_payload(request), reply));
    out = reply.result;
    return ok_status();
}

Status CoordinatorClient::bind_worker(const BindWorkerRequest& request, BindWorkerResult& out) {
    BindWorkerReply reply;
    PEF_TRY(call(MessageType::BindWorker, request.request, encode_payload(request), reply));
    out = reply.result;
    return ok_status();
}

Status CoordinatorClient::start(const StartRequest& request) {
    SimpleReply reply;
    return call(MessageType::Start, request.request, encode_payload(request), reply);
}

Status CoordinatorClient::begin_action(const BeginActionRequest& request, BeginActionResult& out) {
    BeginActionReply reply;
    PEF_TRY(call(MessageType::BeginAction, request.request, encode_payload(request), reply));
    out = reply.result;
    return ok_status();
}

Status CoordinatorClient::complete_action(const CompleteActionRequest& request,
                                          CompleteActionResult& out) {
    CompleteActionReply reply;
    PEF_TRY(call(MessageType::CompleteAction, request.request, encode_payload(request), reply));
    out = reply.result;
    return ok_status();
}

Status CoordinatorClient::fail_action(const FailActionRequest& request) {
    SimpleReply reply;
    return call(MessageType::FailAction, request.request, encode_payload(request), reply);
}

Status CoordinatorClient::report_side_effect(const ReportSideEffectRequest& request) {
    SimpleReply reply;
    return call(MessageType::ReportSideEffect, request.request, encode_payload(request), reply);
}

Status CoordinatorClient::register_checkpoint(const RegisterCheckpointRequest& request,
                                              RegisterCheckpointResult& out) {
    CheckpointReply reply;
    PEF_TRY(call(MessageType::RegisterCheckpoint, request.request, encode_payload(request), reply));
    out = reply.result;
    return ok_status();
}

Status CoordinatorClient::resume(const ResumeRequest& request, ResumeResult& out) {
    ResumeReply reply;
    PEF_TRY(call(MessageType::Resume, request.request, encode_payload(request), reply));
    out = reply.result;
    return ok_status();
}

Status CoordinatorClient::suspend_begin(ExecutionId execution) {
    ExecutionIdRequest request;
    request.execution = execution;
    SimpleReply reply;
    return call(MessageType::SuspendBegin, next_request(), encode_payload(request), reply);
}

Status CoordinatorClient::suspend_commit(ExecutionId execution) {
    ExecutionIdRequest request;
    request.execution = execution;
    SimpleReply reply;
    return call(MessageType::SuspendCommit, next_request(), encode_payload(request), reply);
}

Status CoordinatorClient::fence(ExecutionId execution, const std::string& reason) {
    ExecutionIdRequest request;
    request.execution = execution;
    request.reason = reason;
    SimpleReply reply;
    return call(MessageType::Fence, next_request(), encode_payload(request), reply);
}

Status CoordinatorClient::cancel(ExecutionId execution, const std::string& reason) {
    ExecutionIdRequest request;
    request.execution = execution;
    request.reason = reason;
    SimpleReply reply;
    return call(MessageType::Cancel, next_request(), encode_payload(request), reply);
}

Status CoordinatorClient::retire(ExecutionId execution, const std::string& reason) {
    ExecutionIdRequest request;
    request.execution = execution;
    request.reason = reason;
    SimpleReply reply;
    return call(MessageType::Retire, next_request(), encode_payload(request), reply);
}

Status CoordinatorClient::drain(ExecutionId execution) {
    ExecutionIdRequest request;
    request.execution = execution;
    SimpleReply reply;
    return call(MessageType::Drain, next_request(), encode_payload(request), reply);
}

Status CoordinatorClient::complete_execution(ExecutionId execution) {
    ExecutionIdRequest request;
    request.execution = execution;
    SimpleReply reply;
    return call(MessageType::CompleteExecution, next_request(), encode_payload(request), reply);
}

Status CoordinatorClient::advance_generation(ExecutionId execution, const std::string& reason) {
    ExecutionIdRequest request;
    request.execution = execution;
    request.reason = reason;
    SimpleReply reply;
    return call(MessageType::AdvanceGeneration, next_request(), encode_payload(request), reply);
}

Status CoordinatorClient::update_policy(ExecutionId execution, const ExecutionPolicy& policy,
                                        bool apply) {
    UpdatePolicyRequest request;
    request.execution = execution;
    request.policy = policy;
    request.apply = apply;
    SimpleReply reply;
    return call(MessageType::UpdatePolicy, next_request(), encode_payload(request), reply);
}

Status CoordinatorClient::classify(ExecutionId execution, RecoveryPlan& out) {
    ExecutionIdRequest request;
    request.execution = execution;
    ClassifyReply reply;
    PEF_TRY(call(MessageType::Classify, next_request(), encode_payload(request), reply));
    out = reply.plan;
    return ok_status();
}

Status CoordinatorClient::recover(ExecutionId execution, RecoveryOutcome& out) {
    ExecutionIdRequest request;
    request.execution = execution;
    RecoverReply reply;
    PEF_TRY(call(MessageType::Recover, next_request(), encode_payload(request), reply));
    out = reply.outcome;
    return ok_status();
}

Status CoordinatorClient::resolve_ambiguity(const ResolveAmbiguityRequest& request,
                                            ResolveAmbiguityResult& out) {
    ResolveAmbiguityReply reply;
    PEF_TRY(call(MessageType::ResolveAmbiguity, request.request, encode_payload(request), reply));
    out = reply.result;
    return ok_status();
}

Status CoordinatorClient::query(ExecutionId execution, ExecutionView& out) {
    ExecutionIdRequest request;
    request.execution = execution;
    QueryReply reply;
    PEF_TRY(call(MessageType::Query, next_request(), encode_payload(request), reply));
    out = reply.view;
    return ok_status();
}

Status CoordinatorClient::list_executions(std::vector<ExecutionRecord>& out) {
    ListExecutionsReply reply;
    PEF_TRY(call(MessageType::ListExecutions, next_request(), Bytes{}, reply));
    out = std::move(reply.executions);
    return ok_status();
}

Status CoordinatorClient::validate_continuation(ExecutionId execution, ContinuationState& state,
                                                std::string& reason) {
    ExecutionIdRequest request;
    request.execution = execution;
    ValidateContinuationReply reply;
    PEF_TRY(call(MessageType::RevalidateContinuation, next_request(), encode_payload(request),
                 reply));
    state = reply.state;
    reason = reply.reason;
    return ok_status();
}

Status CoordinatorClient::snapshot(bool truncate_journal, std::uint64_t& journal_sequence) {
    SnapshotRequest request;
    request.truncate_journal = truncate_journal;
    SnapshotReply reply;
    PEF_TRY(call(MessageType::Snapshot, next_request(), encode_payload(request), reply));
    journal_sequence = reply.journal_sequence;
    return ok_status();
}

Status CoordinatorClient::audit(AuditReport& out) {
    AuditReply reply;
    PEF_TRY(call(MessageType::Audit, next_request(), Bytes{}, reply));
    out = std::move(reply.report);
    return ok_status();
}

}  // namespace pef
