// Persistent Execution Fabric - test harness.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Cases are addressable by name so a hang can be localized to an exact case and
// an exact lifecycle phase. Every BEGIN, PHASE, PASS, and FAIL line is flushed
// immediately, so a run that stops progressing still reports where it stopped.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace peftest {

struct CaseContext {
    std::string case_name;
    std::size_t checks = 0;
    std::size_t failures = 0;
    bool aborted = false;

    void phase(const char* name);
    void check(bool ok, const char* expression, int line);
    void fail(const std::string& message);
    void abort_case(const std::string& message);
    [[nodiscard]] bool ok() const noexcept { return failures == 0; }
};

using Body = void (*)(CaseContext&);

struct Case {
    std::string suite;
    std::string name;
    Body body = nullptr;
};

struct Registrar {
    Registrar(const char* suite, const char* name, Body body);
};

[[nodiscard]] std::vector<Case>& registry();

// Runs the registered cases. Supports:
//   --list                 print every case as "suite.name"
//   --case <name>          run only the named case (repeatable)
//   --suite <name>         run only cases in a suite
//   --repeat <n>           run every selected case n times
//   --seed <n>             seed for randomized cases
// Returns the number of failing cases.
int run_all(int argc, char** argv);

// Deterministic test randomness. Tests record the seed they used.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}
    [[nodiscard]] std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }
    [[nodiscard]] std::uint32_t below(std::uint32_t bound) {
        return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound);
    }
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

private:
    std::uint64_t state_;
    std::uint64_t seed_ = state_;
};

// A store directory outside the repository that removes itself. Test scratch
// never lands in the source tree.
class TempStore {
public:
    explicit TempStore(const std::string& label);
    ~TempStore();
    TempStore(const TempStore&) = delete;
    TempStore& operator=(const TempStore&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::string path_string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string unique_label(const std::string& prefix);

}  // namespace peftest

#define PEF_TEST(suite_name, case_name)                                              \
    static void suite_name##_##case_name##_body(::peftest::CaseContext& ctx);         \
    static const ::peftest::Registrar suite_name##_##case_name##_registrar(           \
        #suite_name, #case_name, &suite_name##_##case_name##_body);                   \
    static void suite_name##_##case_name##_body(::peftest::CaseContext& ctx)

#define PEF_PHASE(ctx, phase_name) (ctx).phase(#phase_name)

#define PEF_CHECK(ctx, expression) (ctx).check((expression), #expression, __LINE__)

#define PEF_REQUIRE(ctx, expression)                        \
    do {                                                    \
        (ctx).check((expression), #expression, __LINE__);    \
        if (!(ctx).ok()) {                                  \
            (ctx).abort_case("requirement not met");        \
            return;                                         \
        }                                                   \
    } while (false)

#define PEF_CHECK_MSG(ctx, expression, message)                              \
    do {                                                                     \
        if (!(expression)) {                                                 \
            (ctx).fail(std::string(message) + " [" + #expression + "]");     \
        } else {                                                             \
            (ctx).check(true, #expression, __LINE__);                        \
        }                                                                    \
    } while (false)
