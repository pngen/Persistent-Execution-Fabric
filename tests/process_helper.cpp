// Persistent Execution Fabric - real OS process helper for the multiprocess proof.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "process_helper.hpp"

#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "pef/net.hpp"

namespace peftest {
namespace {

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool contains_line_with(const std::string& text, const std::string& marker) {
    return text.find(marker) != std::string::npos;
}

}  // namespace

ChildProcess::~ChildProcess() {
    if (!exited_ && process_handle_ != nullptr) {
        kill();
        (void)wait_for_exit(5000);
    }
    close_handles();
}

void ChildProcess::close_handles() {
#if defined(_WIN32)
    if (process_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(process_handle_));
        process_handle_ = nullptr;
    }
#endif
}

bool ChildProcess::spawn(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments,
                         const std::filesystem::path& work_dir, const std::string& label,
                         std::string& error) {
    stdout_path_ = work_dir / (label + ".out");
    stderr_path_ = work_dir / (label + ".err");
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE out = ::CreateFileW(stdout_path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = ::CreateFileW(stderr_path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE) {
        error = "cannot create child output files";
        if (out != INVALID_HANDLE_VALUE) {
            ::CloseHandle(out);
        }
        if (err != INVALID_HANDLE_VALUE) {
            ::CloseHandle(err);
        }
        return false;
    }

    std::wstring command_line = L"\"" + executable.wstring() + L"\"";
    for (const auto& argument : arguments) {
        command_line += L" \"";
        for (wchar_t ch : std::wstring(argument.begin(), argument.end())) {
            command_line.push_back(ch);
        }
        command_line += L"\"";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out;
    startup.hStdError = err;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    const BOOL started =
        ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr, work_dir.wstring().c_str(), &startup, &info);
    ::CloseHandle(out);
    ::CloseHandle(err);
    if (started == 0) {
        error = "CreateProcess failed for " + executable.string();
        return false;
    }
    ::CloseHandle(info.hThread);
    process_handle_ = info.hProcess;
    pid_ = static_cast<std::uint64_t>(info.dwProcessId);
    exited_ = false;
    return true;
#else
    std::vector<std::string> argument_storage;
    argument_storage.push_back(executable.string());
    for (const auto& argument : arguments) {
        argument_storage.push_back(argument);
    }
    std::vector<char*> argv;
    for (auto& value : argument_storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::freopen(stdout_path_.c_str(), "w", stdout);
        ::freopen(stderr_path_.c_str(), "w", stderr);
        ::chdir(work_dir.c_str());
        ::execv(executable.c_str(), argv.data());
        ::_exit(127);
    }
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    pid_ = static_cast<std::uint64_t>(pid);
    process_handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
    exited_ = false;
    return true;
#endif
}

bool ChildProcess::alive() const {
    if (exited_) {
        return false;
    }
#if defined(_WIN32)
    if (process_handle_ == nullptr) {
        return false;
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code) == 0) {
        return false;
    }
    return code == STILL_ACTIVE;
#else
    if (process_handle_ == nullptr) {
        return false;
    }
    int status = 0;
    const pid_t pid = static_cast<pid_t>(static_cast<std::intptr_t>(process_handle_));
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    return result == 0;
#endif
}

void ChildProcess::kill() {
    if (exited_) {
        return;
    }
#if defined(_WIN32)
    if (process_handle_ != nullptr) {
        ::TerminateProcess(static_cast<HANDLE>(process_handle_), 137);
    }
#else
    const pid_t pid = static_cast<pid_t>(static_cast<std::intptr_t>(process_handle_));
    ::kill(pid, SIGKILL);
#endif
}

std::optional<std::uint32_t> ChildProcess::wait_for_exit(int timeout_ms) {
    if (exited_) {
        return exit_code_;
    }
#if defined(_WIN32)
    if (process_handle_ == nullptr) {
        return std::nullopt;
    }
    const DWORD waited = ::WaitForSingleObject(static_cast<HANDLE>(process_handle_),
                                               static_cast<DWORD>(timeout_ms));
    if (waited != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code);
    exit_code_ = static_cast<std::uint32_t>(code);
    exited_ = true;
    return exit_code_;
#else
    int status = 0;
    const pid_t pid = static_cast<pid_t>(static_cast<std::intptr_t>(process_handle_));
    for (int elapsed = 0; elapsed <= timeout_ms; elapsed += 5) {
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            exit_code_ = static_cast<std::uint32_t>(WIFEXITED(status) ? WEXITSTATUS(status)
                                                                      : 128 + WTERMSIG(status));
            exited_ = true;
            return exit_code_;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
#endif
}

std::string ChildProcess::read_stdout() const { return read_text_file(stdout_path_); }

std::string ChildProcess::read_stderr() const { return read_text_file(stderr_path_); }

bool ChildProcess::wait_for_marker(const std::string& marker, int timeout_ms,
                                   std::string& observed) {
    for (int elapsed = 0; elapsed <= timeout_ms; elapsed += 10) {
        observed = read_stdout();
        if (contains_line_with(observed, marker)) {
            return true;
        }
        if (!alive()) {
            observed = read_stdout();
            return contains_line_with(observed, marker);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    observed = read_stdout();
    return contains_line_with(observed, marker);
}

bool wait_for_port(const std::string& host, std::uint16_t port, int timeout_ms) {
    pef::net_initialize();
    for (int elapsed = 0; elapsed <= timeout_ms; elapsed += 20) {
        pef::Socket socket;
        if (pef::connect_to(host, port, socket, 200).ok()) {
            socket.close();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::optional<std::string> field_value(const std::string& text, const std::string& key) {
    const std::string needle = key + "=";
    std::size_t position = text.find(needle);
    if (position == std::string::npos) {
        return std::nullopt;
    }
    position += needle.size();
    std::size_t end = position;
    while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\r') {
        ++end;
    }
    return text.substr(position, end - position);
}

}  // namespace peftest
