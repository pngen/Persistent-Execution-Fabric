// Persistent Execution Fabric - coordinator client.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// One request/reply client over the framed control plane. The client makes no
// authority decisions: it presents identities and reports what the coordinator
// answered, including refusals.
#pragma once

#include <cstdint>
#include <string>

#include "pef/net.hpp"
#include "pef/protocol.hpp"
#include "pef/status.hpp"

namespace pef {

struct ClientConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::string name = "pef-client";
    std::string version;
    int timeout_ms = 10000;
};

class CoordinatorClient {
public:
    CoordinatorClient() = default;
    ~CoordinatorClient();
    CoordinatorClient(const CoordinatorClient&) = delete;
    CoordinatorClient& operator=(const CoordinatorClient&) = delete;

    [[nodiscard]] Status connect(const ClientConfig& config);
    void close() noexcept;
    [[nodiscard]] bool connected() const noexcept { return channel_.valid(); }

    [[nodiscard]] Status hello_as_operator(HelloReply& out);
    [[nodiscard]] Status hello_as_worker(WorkerId worker, WorkerBootId boot, HelloReply& out);
    [[nodiscard]] Status ping(PingReply& out);
    [[nodiscard]] Status request_shutdown();

    [[nodiscard]] Status create_execution(const ExecutionPolicy& policy,
                                          CreateExecutionResult& out);
    [[nodiscard]] Status bind_worker(const BindWorkerRequest& request, BindWorkerResult& out);
    [[nodiscard]] Status start(const StartRequest& request);
    [[nodiscard]] Status begin_action(const BeginActionRequest& request, BeginActionResult& out);
    [[nodiscard]] Status complete_action(const CompleteActionRequest& request,
                                         CompleteActionResult& out);
    [[nodiscard]] Status fail_action(const FailActionRequest& request);
    [[nodiscard]] Status report_side_effect(const ReportSideEffectRequest& request);
    [[nodiscard]] Status register_checkpoint(const RegisterCheckpointRequest& request,
                                             RegisterCheckpointResult& out);
    [[nodiscard]] Status resume(const ResumeRequest& request, ResumeResult& out);
    [[nodiscard]] Status suspend_begin(ExecutionId execution);
    [[nodiscard]] Status suspend_commit(ExecutionId execution);
    [[nodiscard]] Status fence(ExecutionId execution, const std::string& reason);
    [[nodiscard]] Status cancel(ExecutionId execution, const std::string& reason);
    [[nodiscard]] Status retire(ExecutionId execution, const std::string& reason);
    [[nodiscard]] Status drain(ExecutionId execution);
    [[nodiscard]] Status complete_execution(ExecutionId execution);
    [[nodiscard]] Status advance_generation(ExecutionId execution, const std::string& reason);
    [[nodiscard]] Status update_policy(ExecutionId execution, const ExecutionPolicy& policy,
                                       bool apply);
    [[nodiscard]] Status classify(ExecutionId execution, RecoveryPlan& out);
    [[nodiscard]] Status recover(ExecutionId execution, RecoveryOutcome& out);
    [[nodiscard]] Status resolve_ambiguity(const ResolveAmbiguityRequest& request,
                                           ResolveAmbiguityResult& out);
    [[nodiscard]] Status query(ExecutionId execution, ExecutionView& out);
    [[nodiscard]] Status list_executions(std::vector<ExecutionRecord>& out);
    [[nodiscard]] Status validate_continuation(ExecutionId execution, ContinuationState& state,
                                               std::string& reason);
    [[nodiscard]] Status snapshot(bool truncate_journal, std::uint64_t& journal_sequence);
    [[nodiscard]] Status audit(AuditReport& out);

    // Request identities are client-scoped and minted from a per-instance
    // nonce, so two client instances never collide in the coordinator's
    // replay-protection cache. A deliberate retry reuses the identity it
    // already holds.
    [[nodiscard]] RequestId next_request() noexcept;
    [[nodiscard]] RequestId peek_request() const noexcept;
    [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] StoreId store_id() const noexcept { return store_; }
    [[nodiscard]] SessionId session() const noexcept { return session_; }

private:
    // Sends one request and returns the decoded status. When the status is OK
    // the caller decodes the typed reply from reply.payload.
    [[nodiscard]] Status exchange(MessageType type, RequestId request, const Bytes& payload,
                                  Frame& reply);

    template <class Reply>
    [[nodiscard]] Status call(MessageType type, RequestId request, const Bytes& payload,
                              Reply& out) {
        Frame reply;
        PEF_TRY(exchange(type, request, payload, reply));
        StatusPayload status;
        if (!reply_status(reply, status)) {
            return err(Code::ProtocolViolation, "reply payload did not begin with a status");
        }
        if (status.code != Code::Ok) {
            return err(status.code, status.message);
        }
        Reply decoded{};
        if (!decode_payload(reply.payload, decoded)) {
            return err(Code::ProtocolViolation, "reply payload did not decode");
        }
        out = std::move(decoded);
        return ok_status();
    }

    // HELLO and PING replies are not status-prefixed: they are the handshake.
    template <class Reply>
    [[nodiscard]] Status call_raw(MessageType type, RequestId request, const Bytes& payload,
                                  Reply& out) {
        Frame reply;
        PEF_TRY(exchange(type, request, payload, reply));
        Reply decoded{};
        if (!decode_payload(reply.payload, decoded)) {
            return err(Code::ProtocolViolation, "handshake reply payload did not decode");
        }
        out = std::move(decoded);
        return ok_status();
    }

    FrameChannel channel_;
    std::uint64_t request_counter_ = 0;
    std::uint64_t client_nonce_ = 0;
    CoordinatorEpoch epoch_;
    StoreId store_;
    SessionId session_;
    bool is_worker_ = false;
};

}  // namespace pef
