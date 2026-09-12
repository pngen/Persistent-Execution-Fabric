// Persistent Execution Fabric - coordinator server.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/server.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pef {
namespace {

struct CachedReply {
    MessageType type = MessageType::Ping;
    Bytes payload;
};

struct ConnectionState {
    SessionId session;
    bool established = false;
    bool is_worker = false;
    WorkerId worker;
    WorkerBootId boot;
};

[[nodiscard]] Frame make_reply(const Frame& request, MessageType type, const Bytes& payload) {
    Frame reply;
    reply.type = static_cast<MessageType>(static_cast<std::uint16_t>(type) | kReplyBit);
    reply.request = request.request;
    reply.payload = payload;
    return reply;
}

template <class T>
[[nodiscard]] Frame make_typed_reply(const Frame& request, MessageType type, const T& value) {
    return make_reply(request, type, encode_payload(value));
}

[[nodiscard]] Frame make_status_reply(const Frame& request, MessageType type, const Status& status) {
    StatusPayload payload;
    payload.code = status.code();
    payload.message = status.message();
    return make_typed_reply(request, type, payload);
}

}  // namespace

struct CoordinatorServer::Impl {
    ServerConfig config;
    Listener listener;
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> accept_finished{false};
    // Every accepted socket, retained so that stop() can close them and unblock
    // connection threads waiting in a read. Without this, stopping the
    // coordinator while a client is connected would join a blocked thread.
    std::mutex sockets_mu;
    std::vector<std::shared_ptr<Socket>> sockets;
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> refusals{0};
    std::atomic<std::uint64_t> replayed{0};
    std::atomic<std::size_t> active_connections{0};

    std::mutex cache_mu;
    std::unordered_map<RequestId, CachedReply, IdHasher<RequestIdTag>> reply_cache;
    std::deque<RequestId> reply_order;

    std::mutex threads_mu;
    std::vector<std::thread> connection_threads;
    std::thread accept_thread;

    // Serves one request/response exchange. A refusal is a successful exchange
    // that carries a refusal payload; a failing status means the connection
    // itself failed.
    Status serve(Runtime& runtime, ConnectionState& connection, const Frame& request, Frame& reply);

    void remember_reply(RequestId id, const CachedReply& reply) {
        if (!id.valid() || config.request_cache_size == 0) {
            return;
        }
        std::lock_guard<std::mutex> guard(cache_mu);
        if (reply_cache.find(id) != reply_cache.end()) {
            return;
        }
        while (reply_order.size() >= config.request_cache_size && !reply_order.empty()) {
            const RequestId oldest = reply_order.front();
            reply_order.pop_front();
            reply_cache.erase(oldest);
        }
        reply_cache.emplace(id, reply);
        reply_order.push_back(id);
    }

    [[nodiscard]] bool lookup_reply(RequestId id, CachedReply& out) {
        if (!id.valid()) {
            return false;
        }
        std::lock_guard<std::mutex> guard(cache_mu);
        const auto it = reply_cache.find(id);
        if (it == reply_cache.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
};

CoordinatorServer::CoordinatorServer() : impl_(std::make_unique<Impl>()) {}

CoordinatorServer::~CoordinatorServer() {
    (void)stop();
    (void)shutdown_runtime();
}

std::uint16_t CoordinatorServer::port() const noexcept { return impl_->listener.port(); }

CoordinatorEpoch CoordinatorServer::epoch() const { return runtime_.epoch(); }

StoreId CoordinatorServer::store_id() const { return runtime_.store_id(); }

std::size_t CoordinatorServer::active_connections() const {
    return impl_->active_connections.load();
}

std::uint64_t CoordinatorServer::requests_served() const { return impl_->requests.load(); }

std::uint64_t CoordinatorServer::refusals_served() const { return impl_->refusals.load(); }

std::uint64_t CoordinatorServer::replayed_requests() const { return impl_->replayed.load(); }

bool CoordinatorServer::running() const noexcept { return impl_->running.load(); }

bool CoordinatorServer::accept_loop_finished() const noexcept {
    return impl_->accept_finished.load();
}

Status CoordinatorServer::start(const ServerConfig& config, OpenOutcome& outcome) {
    if (impl_->running.load()) {
        return err(Code::AlreadyExists, "server is already running");
    }
    net_initialize();
    impl_->config = config;
    if (config.runtime.store_path.empty()) {
        return err(Code::InvalidArgument, "server requires a durable store path");
    }
    RuntimeConfig runtime_config = config.runtime;
    PEF_TRY(runtime_.open(runtime_config, outcome));
    outcome_ = outcome;

    PEF_TRY(impl_->listener.listen_on(config.host, config.port));
    impl_->stop_requested.store(false);
    impl_->running.store(true);
    return ok_status();
}

Status CoordinatorServer::run() {
    if (!impl_->running.load()) {
        return err(Code::NotFound, "server is not running");
    }
    impl_->accept_thread = std::thread([this]() {
        while (!impl_->stop_requested.load()) {
            Socket socket;
            const Status accepted = impl_->listener.accept(socket);
            if (!accepted.ok()) {
                if (impl_->stop_requested.load()) {
                    break;
                }
                continue;
            }
            if (impl_->active_connections.load() >= impl_->config.max_connections) {
                socket.close();
                continue;
            }
            impl_->active_connections.fetch_add(1);
            auto shared_socket = std::make_shared<Socket>(std::move(socket));
            {
                std::lock_guard<std::mutex> guard(impl_->sockets_mu);
                impl_->sockets.push_back(shared_socket);
            }
            std::lock_guard<std::mutex> guard(impl_->threads_mu);
            impl_->connection_threads.emplace_back([this, shared_socket]() {
                FrameChannel channel(shared_socket);
                ConnectionState state;
                while (!impl_->stop_requested.load()) {
                    Frame request;
                    std::string reason;
                    const Status received = channel.receive(request, reason);
                    if (!received.ok()) {
                        break;
                    }
                    Frame reply;
                    const Status served = impl_->serve(runtime_, state, request, reply);
                    if (!served.ok()) {
                        break;
                    }
                    if (!channel.send(reply).ok()) {
                        break;
                    }
                }
                // The connection is gone. Whatever worker authority it carried
                // ends here: the worker is fenced before the session is dropped.
                if (state.established && state.is_worker) {
                    (void)runtime_.end_session(state.session);
                }
                channel.close();
                {
                    std::lock_guard<std::mutex> sockets_guard(impl_->sockets_mu);
                    impl_->sockets.erase(
                        std::remove(impl_->sockets.begin(), impl_->sockets.end(), shared_socket),
                        impl_->sockets.end());
                }
                impl_->active_connections.fetch_sub(1);
            });
        }
    });
    impl_->accept_thread.join();
    impl_->accept_finished.store(true);
    return ok_status();
}

Status CoordinatorServer::stop() {
    if (!impl_->running.load() && !impl_->stop_requested.load() && !impl_->accept_finished.load()) {
        return ok_status();
    }
    impl_->stop_requested.store(true);
    impl_->listener.close();
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    // Closing an in-flight connection from another thread unblocks a read and
    // lets the connection thread finish, which is what makes shutdown while a
    // client is attached a bounded operation rather than a hang.
    {
        std::lock_guard<std::mutex> guard(impl_->sockets_mu);
        for (auto& socket : impl_->sockets) {
            socket->close();
        }
    }
    {
        std::lock_guard<std::mutex> guard(impl_->threads_mu);
        for (auto& thread : impl_->connection_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        impl_->connection_threads.clear();
    }
    {
        std::lock_guard<std::mutex> guard(impl_->sockets_mu);
        impl_->sockets.clear();
    }
    impl_->running.store(false);
    impl_->accept_finished.store(true);
    return ok_status();
}

Status CoordinatorServer::shutdown_runtime() {
    PEF_TRY(runtime_.shutdown());
    net_shutdown();
    return ok_status();
}

Status CoordinatorServer::Impl::serve(Runtime& runtime, ConnectionState& connection,
                                         const Frame& request, Frame& reply) {
    Impl& impl = *this;
    impl.requests.fetch_add(1);
    const MessageType base = request.base_type();
    if (request.is_reply()) {
        Frame error = make_error_reply(request.request, Code::ProtocolViolation,
                                       "a reply frame was received as a request");
        reply = error;
        return ok_status();
    }

    // Replay protection is per request identity. A repeated identity returns the
    // stored reply verbatim and never re-executes the operation.
    if (base != MessageType::Hello && base != MessageType::Ping) {
        CachedReply cached;
        if (impl.lookup_reply(request.request, cached)) {
            impl.replayed.fetch_add(1);
            reply = make_reply(request, cached.type, cached.payload);
            return ok_status();
        }
    }

    auto refuse = [&](Code code, std::string message) {
        impl.refusals.fetch_add(1);
        StatusPayload payload;
        payload.code = code;
        payload.message = std::move(message);
        reply = make_typed_reply(request, base, payload);
        return ok_status();
    };

    if (base == MessageType::Hello) {
        HelloRequest hello;
        if (!decode_payload(request.payload, hello)) {
            return refuse(Code::ProtocolViolation, "HELLO payload did not decode");
        }
        HelloReply hello_reply;
        hello_reply.store = runtime.store_id();
        hello_reply.epoch = runtime.epoch();
        hello_reply.coordinator_version = build_info_string();
        hello_reply.protocol_version = kProtocolVersion;
        hello_reply.persistence_schema = kPersistenceSchemaVersion;
        hello_reply.journal_sequence = runtime.journal_sequence();
        if (hello.as_worker) {
            CallerContext caller;
            SessionId session;
            const Status begun = runtime.begin_session(hello.worker, hello.boot, caller, session);
            if (!begun.ok()) {
                return refuse(begun.code(), begun.message());
            }
            connection.session = session;
            connection.established = true;
            connection.is_worker = true;
            connection.worker = hello.worker;
            connection.boot = hello.boot;
            hello_reply.session = session;
        }
        reply = make_typed_reply(request, base, hello_reply);
        return ok_status();
    }

    CallerContext caller;
    caller.epoch = runtime.epoch();
    if (connection.established) {
        caller.session = connection.session;
    }

    if (base == MessageType::Ping) {
        PingRequest ping;
        if (!decode_payload(request.payload, ping)) {
            return refuse(Code::ProtocolViolation, "PING payload did not decode");
        }
        PingReply pong;
        pong.token = ping.token;
        pong.epoch = runtime.epoch();
        reply = make_typed_reply(request, base, pong);
        return ok_status();
    }

    if (base == MessageType::Shutdown) {
        if (!impl.config.allow_remote_shutdown) {
            return refuse(Code::PolicyRefused, "remote shutdown is disabled on this coordinator");
        }
        StatusPayload payload;
        payload.code = Code::Ok;
        payload.message = "coordinator is shutting down";
        reply = make_typed_reply(request, base, payload);
        impl.stop_requested.store(true);
        impl.listener.close();
        return ok_status();
    }

    auto remember = [&](const CachedReply& cached) {
        impl.remember_reply(request.request, cached);
    };

    // Every reply carries a status first, so a refusal is never mistaken for a
    // result with defaulted fields.
    switch (base) {
        case MessageType::CreateExecution: {
            CreateExecutionRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "CREATE_EXECUTION payload did not decode");
            }
            CreateExecutionReply out;
            const Status status = runtime.create_execution(caller, typed, out.result);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::BindWorker: {
            BindWorkerRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "BIND_WORKER payload did not decode");
            }
            BindWorkerReply out;
            const Status status = runtime.bind_worker(caller, typed, out.result);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::Start: {
            StartRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "START payload did not decode");
            }
            const Status status = runtime.start(typed);
            const SimpleReply out{StatusPayload{status.code(), status.message()}};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::BeginAction: {
            BeginActionRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "BEGIN_ACTION payload did not decode");
            }
            BeginActionReply out;
            const Status status = runtime.begin_action(typed, out.result);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::CompleteAction: {
            CompleteActionRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "COMPLETE_ACTION payload did not decode");
            }
            CompleteActionReply out;
            const Status status = runtime.complete_action(typed, out.result);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::FailAction: {
            FailActionRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "FAIL_ACTION payload did not decode");
            }
            const Status status = runtime.fail_action(typed);
            const SimpleReply out{StatusPayload{status.code(), status.message()}};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::ReportSideEffect: {
            ReportSideEffectRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "REPORT_SIDE_EFFECT payload did not decode");
            }
            const Status status = runtime.report_side_effect(typed);
            const SimpleReply out{StatusPayload{status.code(), status.message()}};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::RegisterCheckpoint: {
            RegisterCheckpointRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "CHECKPOINT payload did not decode");
            }
            CheckpointReply out;
            const Status status = runtime.register_checkpoint(typed, out.result);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::Resume: {
            ResumeRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "RESUME payload did not decode");
            }
            ResumeReply out;
            const Status status = runtime.resume(typed, out.result);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::SuspendBegin:
        case MessageType::SuspendCommit: {
            ExecutionIdRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "suspend payload did not decode");
            }
            const Status status = base == MessageType::SuspendBegin
                                      ? runtime.suspend_begin(caller, request.request,
                                                              typed.execution)
                                      : runtime.suspend_commit(caller, request.request,
                                                               typed.execution);
            const SimpleReply out{StatusPayload{status.code(), status.message()}};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::Fence:
        case MessageType::Cancel:
        case MessageType::Retire:
        case MessageType::Drain:
        case MessageType::CompleteExecution:
        case MessageType::AdvanceGeneration: {
            ExecutionIdRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "lifecycle payload did not decode");
            }
            Status status;
            switch (base) {
                case MessageType::Fence:
                    status = runtime.fence(caller, request.request, typed.execution, typed.reason);
                    break;
                case MessageType::Cancel:
                    status = runtime.cancel(caller, request.request, typed.execution, typed.reason);
                    break;
                case MessageType::Retire:
                    status = runtime.retire(caller, request.request, typed.execution, typed.reason);
                    break;
                case MessageType::Drain:
                    status = runtime.drain(caller, request.request, typed.execution);
                    break;
                case MessageType::CompleteExecution:
                    status = runtime.complete_execution(caller, request.request, typed.execution);
                    break;
                default:
                    status = runtime.advance_execution_generation(caller, request.request,
                                                                  typed.execution, typed.reason);
                    break;
            }
            const SimpleReply out{StatusPayload{status.code(), status.message()}};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::UpdatePolicy: {
            UpdatePolicyRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "UPDATE_POLICY payload did not decode");
            }
            const Status status = runtime.update_policy(caller, request.request, typed.execution,
                                                        typed.policy, typed.apply);
            const SimpleReply out{StatusPayload{status.code(), status.message()}};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::Classify: {
            ExecutionIdRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "CLASSIFY payload did not decode");
            }
            ClassifyReply out;
            const Status status = runtime.classify(caller, typed.execution, out.plan);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            return ok_status();
        }
        case MessageType::Recover: {
            ExecutionIdRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "RECOVER payload did not decode");
            }
            RecoverReply out;
            const Status status =
                runtime.recover(caller, request.request, typed.execution, out.outcome);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::ResolveAmbiguity: {
            ResolveAmbiguityRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "RESOLVE_AMBIGUITY payload did not decode");
            }
            typed.caller = caller;
            ResolveAmbiguityReply out;
            const Status status = runtime.resolve_ambiguity(typed, out.result);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            if (status.ok()) {
                remember(CachedReply{base, reply.payload});
            }
            return ok_status();
        }
        case MessageType::Query: {
            ExecutionIdRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "QUERY payload did not decode");
            }
            QueryReply out;
            const Status status = runtime.query(typed.execution, out.view);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            return ok_status();
        }
        case MessageType::ListExecutions: {
            ListExecutionsReply out;
            const Status status = runtime.list_executions(out.executions);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            return ok_status();
        }
        case MessageType::RevalidateContinuation: {
            ExecutionIdRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation,
                              "VALIDATE_CONTINUATION payload did not decode");
            }
            ValidateContinuationReply out;
            const Status status =
                runtime.validate_continuation(caller, typed.execution, out.state, out.reason);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            return ok_status();
        }
        case MessageType::Snapshot: {
            SnapshotRequest typed;
            if (!decode_payload(request.payload, typed)) {
                return refuse(Code::ProtocolViolation, "SNAPSHOT payload did not decode");
            }
            SnapshotReply out;
            const Status status = runtime.take_snapshot(typed.truncate_journal);
            out.status = StatusPayload{status.code(), status.message()};
            out.journal_sequence = runtime.journal_sequence();
            reply = make_typed_reply(request, base, out);
            return ok_status();
        }
        case MessageType::Audit: {
            AuditReply out;
            const Status status = runtime.audit(out.report);
            out.status = StatusPayload{status.code(), status.message()};
            reply = make_typed_reply(request, base, out);
            return ok_status();
        }
        case MessageType::Hello:
        case MessageType::Ping:
        case MessageType::Shutdown:
            break;
    }
    return refuse(Code::ProtocolViolation, "message type is not handled by this coordinator");
}

}  // namespace pef
