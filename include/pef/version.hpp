// Persistent Execution Fabric - version and build identity.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <string>

namespace pef {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

// Version of the on-disk persistence schema. Bumped only when the durable
// encoding changes in a way that older readers cannot decode.
inline constexpr std::uint32_t kPersistenceSchemaVersion = 1;

// Version of the wire protocol spoken by pef_coordinator / pef_worker / pef_cli.
inline constexpr std::uint16_t kProtocolVersion = 1;

[[nodiscard]] std::string version_string();
[[nodiscard]] std::string build_info_string();

}  // namespace pef
