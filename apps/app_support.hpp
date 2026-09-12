// Persistent Execution Fabric - shared executable support.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pef/identity.hpp"
#include "pef/records.hpp"

namespace pef::app {

// Minimal strict argument reader. Unknown flags are reported rather than
// ignored, so a typo cannot silently change what a command does.
class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 0; i < argc; ++i) {
            values_.emplace_back(argv[i]);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] const std::string& at(std::size_t index) const { return values_[index]; }
    [[nodiscard]] const std::string& program() const { return values_[0]; }

    [[nodiscard]] bool has(std::string_view flag) const {
        for (const auto& value : values_) {
            if (value == flag) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<std::string> value(std::string_view flag) const {
        for (std::size_t i = 0; i + 1 < values_.size(); ++i) {
            if (values_[i] == flag) {
                return values_[i + 1];
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string value_or(std::string_view flag, std::string fallback) const {
        const auto found = value(flag);
        return found.has_value() ? *found : std::move(fallback);
    }

    [[nodiscard]] std::uint64_t u64_or(std::string_view flag, std::uint64_t fallback,
                                       bool& ok) const {
        const auto found = value(flag);
        if (!found.has_value()) {
            return fallback;
        }
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(found->c_str(), &end, 0);
        if (end == nullptr || *end != '\0') {
            ok = false;
            return fallback;
        }
        ok = true;
        return static_cast<std::uint64_t>(parsed);
    }

    [[nodiscard]] std::uint64_t u64_or(std::string_view flag, std::uint64_t fallback) const {
        bool ok = true;
        return u64_or(flag, fallback, ok);
    }

    [[nodiscard]] int int_or(std::string_view flag, int fallback) const {
        const auto found = value(flag);
        if (!found.has_value()) {
            return fallback;
        }
        return std::atoi(found->c_str());
    }

    // Positional arguments: everything that is not a flag and not a flag value.
    [[nodiscard]] std::vector<std::string> positional() const {
        std::vector<std::string> out;
        for (std::size_t i = 1; i < values_.size(); ++i) {
            const std::string& value = values_[i];
            if (!value.empty() && value[0] == '-') {
                ++i;  // skip the flag's value
                continue;
            }
            out.push_back(value);
        }
        return out;
    }

private:
    std::vector<std::string> values_;
};

template <class Tag>
[[nodiscard]] std::optional<Id<Tag>> parse_identifier(std::string_view text) {
    return parse_id<Tag>(text, true);
}

// Parses "domain:id:generation[:label]" into a binding reference.
[[nodiscard]] inline std::optional<BindingRef> parse_binding(std::string_view text) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == ':') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    if (parts.size() < 3) {
        return std::nullopt;
    }
    const auto domain = parse_binding_domain(parts[0]);
    if (!domain.has_value()) {
        return std::nullopt;
    }
    const auto id = parse_id<BindingIdTag>(parts[1], true);
    const auto generation = parse_gen<BindingGenerationTag>(parts[2], true);
    if (!id.has_value() || !generation.has_value() || !id->valid() || generation->is_none()) {
        return std::nullopt;
    }
    BindingRef ref;
    ref.domain = *domain;
    ref.id = *id;
    ref.generation = *generation;
    ref.label = parts.size() > 3 ? parts[3] : std::string{};
    return ref;
}

inline void print_line(const std::string& text) {
    std::cout << text << std::endl;
}

inline void print_error(const std::string& text) {
    std::cerr << text << std::endl;
}

}  // namespace pef::app
