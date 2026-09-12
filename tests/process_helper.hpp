// Persistent Execution Fabric - real OS process helper for the multiprocess proof.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Threads are not substituted for processes anywhere in this proof. Every
// helper here starts, observes, kills, or waits on a real operating-system
// process, and child output is captured through files rather than pipes so that
// nothing depends on pipe semantics.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace peftest {

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts executable with arguments, capturing stdout and stderr into files
    // inside work_dir.
    [[nodiscard]] bool spawn(const std::filesystem::path& executable,
                             const std::vector<std::string>& arguments,
                             const std::filesystem::path& work_dir, const std::string& label,
                             std::string& error);

    // True while the process is still running.
    [[nodiscard]] bool alive() const;
    // Terminates the process immediately. The handle stays valid so that the
    // exit status can still be collected.
    void kill();
    // Waits up to timeout_ms for exit. Returns the exit code when the process
    // has exited, and nullopt when it is still running.
    [[nodiscard]] std::optional<std::uint32_t> wait_for_exit(int timeout_ms);
    [[nodiscard]] std::uint32_t exit_code() const noexcept { return exit_code_; }
    [[nodiscard]] bool exited() const noexcept { return exited_; }
    [[nodiscard]] std::uint64_t process_id() const noexcept { return pid_; }

    [[nodiscard]] std::string read_stdout() const;
    [[nodiscard]] std::string read_stderr() const;
    // Waits until a line containing marker appears in stdout, or the process
    // exits, or the bound elapses. Reports what was observed either way.
    [[nodiscard]] bool wait_for_marker(const std::string& marker, int timeout_ms,
                                       std::string& observed);
    [[nodiscard]] const std::filesystem::path& stdout_path() const noexcept { return stdout_path_; }

private:
    void close_handles();

    void* process_handle_ = nullptr;
    std::uint64_t pid_ = 0;
    std::uint32_t exit_code_ = 0;
    bool exited_ = false;
    std::filesystem::path stdout_path_;
    std::filesystem::path stderr_path_;
};

// Waits until a TCP connection to host:port succeeds. Readiness is observed,
// never assumed from elapsed time.
[[nodiscard]] bool wait_for_port(const std::string& host, std::uint16_t port, int timeout_ms);

// Parses "key=value" out of a line of coordinator output.
[[nodiscard]] std::optional<std::string> field_value(const std::string& text,
                                                     const std::string& key);

}  // namespace peftest
