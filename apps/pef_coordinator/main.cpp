// Persistent Execution Fabric - coordinator executable.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "app_support.hpp"
#include "pef/explain.hpp"
#include "pef/server.hpp"
#include "pef/version.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_stop{false};

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
extern "C" void console_handler(int) { g_stop.store(true); }
#endif

void install_signal_handler() {
#if defined(_WIN32)
    ::SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, console_handler);
    std::signal(SIGTERM, console_handler);
#endif
}

void print_usage() {
    pef::app::print_line(
        "usage: pef_coordinator --store <dir> [--create] [--host H] [--port N]\n"
        "                       [--max-connections N] [--request-cache N]\n"
        "                       [--no-remote-shutdown] [--snapshot-on-start]\n"
        "                       [--run-ms N]\n"
        "\n"
        "  --store <dir>            durable store directory (required)\n"
        "  --create                 create the store when it does not exist\n"
        "  --host <address>         bind address, default 127.0.0.1\n"
        "  --port <n>               bind port, 0 selects an ephemeral port\n"
        "  --max-connections <n>    concurrent control-plane connections\n"
        "  --request-cache <n>      replayed request identities retained\n"
        "  --no-remote-shutdown     refuse the SHUTDOWN message\n"
        "  --snapshot-on-start      write a snapshot after recovery\n"
        "  --run-ms <n>             stop after n milliseconds, 0 means run until asked\n");
}

}  // namespace

int main(int argc, char** argv) {
    using namespace pef;
    app::Args args(argc, argv);
    if (args.has("--help") || args.has("-h")) {
        print_usage();
        return 0;
    }
    if (args.has("--version")) {
        app::print_line(build_info_string());
        return 0;
    }
    const auto store = args.value("--store");
    if (!store.has_value()) {
        app::print_error("pef_coordinator: --store is required");
        print_usage();
        return 2;
    }

    install_signal_handler();

    ServerConfig config;
    config.host = args.value_or("--host", "127.0.0.1");
    config.port = static_cast<std::uint16_t>(args.u64_or("--port", 0));
    config.max_connections = static_cast<std::size_t>(args.u64_or("--max-connections", 64));
    config.request_cache_size = static_cast<std::size_t>(args.u64_or("--request-cache", 8192));
    config.allow_remote_shutdown = !args.has("--no-remote-shutdown");
    config.runtime.store_path = *store;
    config.runtime.create_if_missing = args.has("--create");
    config.runtime.request_cache_size = config.request_cache_size;

    CoordinatorServer server;
    OpenOutcome outcome;
    const Status started = server.start(config, outcome);
    if (!started.ok()) {
        app::print_error("pef_coordinator: " + started.to_string());
        return 1;
    }
    app::print_line("PEF COORDINATOR READY host=" + config.host +
                    " port=" + std::to_string(server.port()) +
                    " epoch=" + std::to_string(server.epoch().value()) +
                    " store=" + hex64(server.store_id().value()) +
                    " state=" + (outcome.created ? "created" : "recovered") +
                    " leases_revoked=" + std::to_string(outcome.leases_revoked) +
                    " reclassified=" + std::to_string(outcome.executions_reclassified));
    if (args.has("--verbose")) {
        app::print_line(explain_restart(outcome));
    }

    if (args.has("--snapshot-on-start")) {
        const Status snapshotted = server.runtime().take_snapshot(false);
        if (!snapshotted.ok()) {
            app::print_error("pef_coordinator: snapshot failed: " + snapshotted.to_string());
        }
    }

    const std::uint64_t run_ms = args.u64_or("--run-ms", 0);
    std::thread runner([&server]() { (void)server.run(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(run_ms);
    while (!g_stop.load()) {
        if (server.running() == false) {
            break;
        }
        if (run_ms != 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    (void)server.stop();
    if (runner.joinable()) {
        runner.join();
    }
    const Status flushed = server.runtime().flush();
    if (!flushed.ok()) {
        app::print_error("pef_coordinator: flush failed: " + flushed.to_string());
    }
    AuditReport audit;
    if (server.runtime().audit(audit).ok() && !audit.clean()) {
        app::print_error("pef_coordinator: invariant violations at shutdown: " +
                         std::to_string(audit.violations()));
        app::print_error(audit.render());
    }
    app::print_line("PEF COORDINATOR STOPPED requests=" +
                    std::to_string(server.requests_served()) +
                    " refusals=" + std::to_string(server.refusals_served()) +
                    " replayed=" + std::to_string(server.replayed_requests()) +
                    " connections=" + std::to_string(server.active_connections()));
    (void)server.shutdown_runtime();
    return 0;
}
