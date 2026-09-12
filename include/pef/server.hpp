// Persistent Execution Fabric - coordinator server.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// The server owns one Runtime and exposes it over the framed TCP control plane.
// It makes no authority decisions of its own: every request is translated into
// a runtime call, and every refusal comes from the runtime.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "pef/net.hpp"
#include "pef/runtime.hpp"
#include "pef/status.hpp"

namespace pef {

struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    RuntimeConfig runtime;
    std::size_t max_connections = 64;
    std::size_t request_cache_size = 8192;
    // When true the server accepts one SHUTDOWN request and then stops.
    bool allow_remote_shutdown = true;
};

class CoordinatorServer {
public:
    CoordinatorServer();
    ~CoordinatorServer();
    CoordinatorServer(const CoordinatorServer&) = delete;
    CoordinatorServer& operator=(const CoordinatorServer&) = delete;

    [[nodiscard]] Status start(const ServerConfig& config, OpenOutcome& outcome);
    // Accepts and serves connections until stop() is called from another thread
    // or a SHUTDOWN request arrives.
    [[nodiscard]] Status run();
    [[nodiscard]] Status stop();
    [[nodiscard]] Status shutdown_runtime();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] CoordinatorEpoch epoch() const;
    [[nodiscard]] StoreId store_id() const;
    [[nodiscard]] std::size_t active_connections() const;
    [[nodiscard]] std::uint64_t requests_served() const;
    [[nodiscard]] std::uint64_t refusals_served() const;
    [[nodiscard]] std::uint64_t replayed_requests() const;
    [[nodiscard]] bool running() const noexcept;
    // True once the accept loop has returned, whether because stop() was called
    // or because a SHUTDOWN request arrived.
    [[nodiscard]] bool accept_loop_finished() const noexcept;
    [[nodiscard]] Runtime& runtime() noexcept { return runtime_; }
    [[nodiscard]] const Runtime& runtime() const noexcept { return runtime_; }
    [[nodiscard]] const OpenOutcome& open_outcome() const noexcept { return outcome_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Runtime runtime_;
    OpenOutcome outcome_;
};

}  // namespace pef
