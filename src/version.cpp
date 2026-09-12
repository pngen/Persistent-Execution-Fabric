// Persistent Execution Fabric - version strings.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/version.hpp"

namespace pef {

std::string version_string() {
    return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
           std::to_string(kVersionPatch);
}

std::string build_info_string() {
    std::string info = "PersistentExecutionFabric ";
    info += version_string();
    info += " protocol=";
    info += std::to_string(kProtocolVersion);
    info += " schema=";
    info += std::to_string(kPersistenceSchemaVersion);
#if defined(_MSC_VER)
    info += " compiler=msvc-";
    info += std::to_string(_MSC_VER);
#elif defined(__clang__)
    info += " compiler=clang-";
    info += __clang_version__;
#elif defined(__GNUC__)
    info += " compiler=gcc-";
    info += __VERSION__;
#else
    info += " compiler=unknown";
#endif
#if defined(NDEBUG)
    info += " config=release";
#else
    info += " config=debug";
#endif
    return info;
}

}  // namespace pef
