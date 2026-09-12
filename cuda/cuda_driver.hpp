// Persistent Execution Fabric - narrow CUDA driver adapter.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// The driver API is loaded dynamically, so the core fabric has no CUDA build
// dependency at all. If the driver or the device is unavailable, every call
// reports that plainly instead of failing to build. This header is the entire
// hardware surface the fabric touches.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pef::cuda {

// Evidence label for what a hardware observation actually is.
enum class Availability {
    // The driver loaded and a real device answered.
    Real,
    // The driver or a device is not present in this environment.
    Unsupported,
    // Present but refused the request.
    Failed,
};

struct DeviceInfo {
    Availability availability = Availability::Unsupported;
    int ordinal = 0;
    std::string name;
    std::string uuid;
    int compute_major = 0;
    int compute_minor = 0;
    std::uint64_t total_memory = 0;
    std::string detail;
};

// Reports whether the driver is usable, and why not when it is not.
[[nodiscard]] bool driver_available(std::string& reason);
[[nodiscard]] DeviceInfo describe_device(int ordinal);

// A device buffer plus the context it belongs to. Device resources are released
// deterministically by the destructor; nothing is left to process exit.
class DeviceBuffer {
public:
    DeviceBuffer() = default;
    ~DeviceBuffer();
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    DeviceBuffer(DeviceBuffer&& other) noexcept;
    DeviceBuffer& operator=(DeviceBuffer&& other) noexcept;

    [[nodiscard]] bool allocate(std::uint64_t bytes, std::string& error);
    [[nodiscard]] bool copy_in(const void* host, std::uint64_t bytes, std::string& error);
    [[nodiscard]] bool copy_out(void* host, std::uint64_t bytes, std::string& error);
    void release();

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] bool valid() const noexcept { return pointer_ != 0; }
    // The driver handle for this allocation. Only the adapter uses it.
    [[nodiscard]] std::uint64_t native_pointer() const noexcept { return pointer_; }

private:
    std::uint64_t pointer_ = 0;
    std::uint64_t size_ = 0;
};

// Runs the hardware kernel that the proof binds to: out[i] = in[i] * 2 + 1.
// The kernel is JIT-compiled by the driver from PTX, so no CUDA toolkit is
// needed at build time. Returns false and fills error on any failure.
[[nodiscard]] bool launch_scale_kernel(DeviceBuffer& out, const DeviceBuffer& in,
                                       std::uint32_t count, std::string& error);

// True when the last launched kernel's completion was observed by the host.
[[nodiscard]] bool synchronize(std::string& error);

}  // namespace pef::cuda
