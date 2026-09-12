// Persistent Execution Fabric - test harness implementation.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

namespace peftest {
namespace {

std::atomic<std::uint64_t> g_label_counter{0};

std::string now_label() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

Registrar::Registrar(const char* suite, const char* name, Body body) {
    registry().push_back(Case{suite, name, body});
}

void CaseContext::phase(const char* name) {
    std::cout << "  PHASE " << name << std::endl;
}

void CaseContext::check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "  CHECK-FAIL line " << line << ": " << expression << std::endl;
    }
}

void CaseContext::fail(const std::string& message) {
    ++failures;
    std::cout << "  FAILURE: " << message << std::endl;
}

void CaseContext::abort_case(const std::string& message) {
    aborted = true;
    std::cout << "  ABORT: " << message << std::endl;
}

std::string unique_label(const std::string& prefix) {
    static std::atomic<std::uint64_t> counter{0};
    std::ostringstream out;
    out << prefix << "-" << now_label() << "-" << counter.fetch_add(1);
    return out.str();
}

TempStore::TempStore(const std::string& label) {
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec);
    if (ec) {
        base = std::filesystem::current_path();
    }
    path_ = base / "pef-tests" / unique_label(label);
    std::filesystem::create_directories(path_, ec);
}

TempStore::~TempStore() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    // Remove the shared parent too, when it is empty.
    std::filesystem::remove(path_.parent_path(), ec);
}

namespace {

struct Options {
    bool list = false;
    std::vector<std::string> cases;
    std::string suite;
    int repeat = 1;
    std::uint64_t seed = 1;
};

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            options.list = true;
        } else if (arg == "--case" && i + 1 < argc) {
            options.cases.push_back(argv[++i]);
        } else if (arg == "--suite" && i + 1 < argc) {
            options.suite = argv[++i];
        } else if (arg == "--repeat" && i + 1 < argc) {
            options.repeat = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--seed" && i + 1 < argc) {
            options.seed = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }
    return options;
}

std::string qualified(const Case& test_case) {
    return test_case.suite + "." + test_case.name;
}

}  // namespace

int run_all(int argc, char** argv) {
    const Options options = parse(argc, argv);
    if (options.list) {
        for (const auto& test_case : registry()) {
            std::cout << qualified(test_case) << std::endl;
        }
        return 0;
    }

    std::size_t failed = 0;
    std::size_t executed = 0;
    const auto suite_start = std::chrono::steady_clock::now();

    for (const auto& test_case : registry()) {
        const std::string name = qualified(test_case);
        if (!options.cases.empty() &&
            std::find(options.cases.begin(), options.cases.end(), name) == options.cases.end()) {
            continue;
        }
        if (!options.suite.empty() && test_case.suite != options.suite) {
            continue;
        }
        for (int iteration = 0; iteration < options.repeat; ++iteration) {
            ++executed;
            std::cout << "BEGIN " << name;
            if (options.repeat > 1) {
                std::cout << " iteration=" << iteration << " seed=" << options.seed;
            }
            std::cout << std::endl;
            CaseContext ctx;
            ctx.case_name = name;
            const auto case_start = std::chrono::steady_clock::now();
            test_case.body(ctx);
            const auto case_end = std::chrono::steady_clock::now();
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                    case_end - case_start)
                                    .count();
            if (ctx.ok()) {
                std::cout << "PASS " << name << " checks=" << ctx.checks << " micros=" << micros
                          << std::endl;
            } else {
                ++failed;
                std::cout << "FAIL " << name << " checks=" << ctx.checks
                          << " failures=" << ctx.failures << " micros=" << micros << std::endl;
            }
        }
    }

    const auto suite_end = std::chrono::steady_clock::now();
    const auto total_micros =
        std::chrono::duration_cast<std::chrono::microseconds>(suite_end - suite_start).count();
    std::cout << "SUMMARY executed=" << executed << " failed=" << failed
              << " micros=" << total_micros << std::endl;
    return static_cast<int>(failed);
}

}  // namespace peftest
