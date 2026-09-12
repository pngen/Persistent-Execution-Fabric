// Persistent Execution Fabric - narrow CUDA driver adapter.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "cuda_driver.hpp"

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace pef::cuda {
namespace {

// Minimal subset of the CUDA driver ABI. Only the entry points this adapter
// needs are declared, so a driver that lacks any of them is reported as
// unsupported rather than crashing.
using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUmodule = void*;
using CUfunction = void*;
using CUdeviceptr = std::uint64_t;

struct CUuuid {
    char bytes[16];
};

constexpr CUresult kSuccess = 0;

using CuInitFn = CUresult (*)(unsigned);
using CuDeviceGetCountFn = CUresult (*)(int*);
using CuDeviceGetFn = CUresult (*)(CUdevice*, int);
using CuDeviceGetNameFn = CUresult (*)(char*, int, CUdevice);
using CuDeviceGetUuidFn = CUresult (*)(CUuuid*, CUdevice);
using CuDeviceComputeCapabilityFn = CUresult (*)(int*, int*, CUdevice);
using CuDeviceTotalMemFn = CUresult (*)(std::size_t*, CUdevice);
using CuCtxCreateFn = CUresult (*)(CUcontext*, unsigned, CUdevice);
using CuCtxDestroyFn = CUresult (*)(CUcontext);
using CuCtxPushCurrentFn = CUresult (*)(CUcontext);
using CuCtxPopCurrentFn = CUresult (*)(CUcontext*);
using CuMemAllocFn = CUresult (*)(CUdeviceptr*, std::size_t);
using CuMemFreeFn = CUresult (*)(CUdeviceptr);
using CuMemcpyHtoDFn = CUresult (*)(CUdeviceptr, const void*, std::size_t);
using CuMemcpyDtoHFn = CUresult (*)(void*, CUdeviceptr, std::size_t);
using CuModuleLoadDataFn = CUresult (*)(CUmodule*, const void*);
using CuModuleUnloadFn = CUresult (*)(CUmodule);
using CuModuleGetFunctionFn = CUresult (*)(CUfunction*, CUmodule, const char*);
using CuLaunchKernelFn = CUresult (*)(CUfunction, unsigned, unsigned, unsigned, unsigned,
                                      unsigned, unsigned, unsigned, CUcontext, void**, void**);
using CuCtxSynchronizeFn = CUresult (*)();
using CuGetErrorNameFn = CUresult (*)(CUresult, const char**);

struct Driver {
    void* library = nullptr;
    bool initialized = false;
    std::string detail;
    CuInitFn cuInit = nullptr;
    CuDeviceGetCountFn cuDeviceGetCount = nullptr;
    CuDeviceGetFn cuDeviceGet = nullptr;
    CuDeviceGetNameFn cuDeviceGetName = nullptr;
    CuDeviceGetUuidFn cuDeviceGetUuid = nullptr;
    CuDeviceComputeCapabilityFn cuDeviceComputeCapability = nullptr;
    CuDeviceTotalMemFn cuDeviceTotalMem = nullptr;
    CuCtxCreateFn cuCtxCreate = nullptr;
    CuCtxDestroyFn cuCtxDestroy = nullptr;
    CuCtxPushCurrentFn cuCtxPushCurrent = nullptr;
    CuCtxPopCurrentFn cuCtxPopCurrent = nullptr;
    CuMemAllocFn cuMemAlloc = nullptr;
    CuMemFreeFn cuMemFree = nullptr;
    CuMemcpyHtoDFn cuMemcpyHtoD = nullptr;
    CuMemcpyDtoHFn cuMemcpyDtoH = nullptr;
    CuModuleLoadDataFn cuModuleLoadData = nullptr;
    CuModuleUnloadFn cuModuleUnload = nullptr;
    CuModuleGetFunctionFn cuModuleGetFunction = nullptr;
    CuLaunchKernelFn cuLaunchKernel = nullptr;
    CuCtxSynchronizeFn cuCtxSynchronize = nullptr;
    CuGetErrorNameFn cuGetErrorName = nullptr;
};

Driver& driver() {
    static Driver instance;
    return instance;
}

std::mutex& driver_mutex() {
    static std::mutex mutex;
    return mutex;
}

void* open_library() {
#if defined(_WIN32)
    return ::LoadLibraryW(L"nvcuda.dll");
#else
    return ::dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
}

template <class Fn>
void bind(void* library, Fn& target, const char* name) {
#if defined(_WIN32)
    target = reinterpret_cast<Fn>(::GetProcAddress(static_cast<HMODULE>(library), name));
#else
    target = reinterpret_cast<Fn>(::dlsym(library, name));
#endif
}

std::string error_name(CUresult result) {
    Driver& api = driver();
    if (api.cuGetErrorName != nullptr) {
        const char* text = nullptr;
        if (api.cuGetErrorName(result, &text) == kSuccess && text != nullptr) {
            return text;
        }
    }
    return "CUDA error " + std::to_string(result);
}

// A context that is current on the calling thread for as long as it lives.
class CurrentContext {
public:
    explicit CurrentContext(CUcontext context) : context_(context) {
        (void)driver().cuCtxPushCurrent(context_);
    }
    ~CurrentContext() {
        CUcontext popped = nullptr;
        if (driver().cuCtxPopCurrent != nullptr) {
            (void)driver().cuCtxPopCurrent(&popped);
        }
    }
    CurrentContext(const CurrentContext&) = delete;
    CurrentContext& operator=(const CurrentContext&) = delete;

private:
    CUcontext context_ = nullptr;
};

// A device context created on first use and kept for the process lifetime, so
// that a buffer allocated by one call is usable by the next.
CUcontext shared_context() {
    static CUcontext context = nullptr;
    static bool attempted = false;
    static std::string failure;
    if (context != nullptr || attempted) {
        return context;
    }
    attempted = true;
    Driver& api = driver();
    if (!api.initialized) {
        failure = api.detail;
        return nullptr;
    }
    CUdevice device = 0;
    if (api.cuDeviceGet(&device, 0) != kSuccess) {
        failure = "no CUDA device at ordinal 0";
        return nullptr;
    }
    if (api.cuCtxCreate(&context, 0, device) != kSuccess) {
        failure = "cuCtxCreate failed";
        context = nullptr;
        return nullptr;
    }
    return context;
}

// Hand-written PTX for out[i] = in[i] * 2 + 1. The driver JIT-compiles it for
// whatever device is present, so the proof needs no CUDA toolkit at build time.
constexpr const char* kScaleKernelPtx = R"PTX(
.version 7.0
.target sm_70
.address_size 64

.visible .entry pef_scale_kernel(
    .param .u64 pef_out,
    .param .u64 pef_in,
    .param .u32 pef_count
)
{
    .reg .pred %p1;
    .reg .b32 %r1, %r2, %r3, %r4, %r5;
    .reg .b64 %rd1, %rd2, %rd3, %rd4, %rd5;
    .reg .f32 %f1, %f2;

    mov.u32 %r1, %ctaid.x;
    mov.u32 %r2, %ntid.x;
    mov.u32 %r3, %tid.x;
    mad.lo.u32 %r4, %r1, %r2, %r3;
    ld.param.u32 %r5, [pef_count];
    setp.ge.u32 %p1, %r4, %r5;
    @%p1 bra PEF_DONE;

    ld.param.u64 %rd1, [pef_in];
    ld.param.u64 %rd2, [pef_out];
    mul.wide.u32 %rd3, %r4, 4;
    add.u64 %rd4, %rd1, %rd3;
    add.u64 %rd5, %rd2, %rd3;
    ld.global.f32 %f1, [%rd4];
    fma.rn.f32 %f2, %f1, 0f40000000, 0f3F800000;
    st.global.f32 [%rd5], %f2;

PEF_DONE:
    ret;
}
)PTX";

}  // namespace

bool driver_available(std::string& reason) {
    std::lock_guard<std::mutex> guard(driver_mutex());
    Driver& api = driver();
    if (api.initialized) {
        reason.clear();
        return true;
    }
    if (api.library == nullptr) {
        api.library = open_library();
        if (api.library == nullptr) {
            api.detail = "the CUDA driver library is not present";
            reason = api.detail;
            return false;
        }
        bind(api.library, api.cuInit, "cuInit");
        bind(api.library, api.cuDeviceGetCount, "cuDeviceGetCount");
        bind(api.library, api.cuDeviceGet, "cuDeviceGet");
        bind(api.library, api.cuDeviceGetName, "cuDeviceGetName");
        bind(api.library, api.cuDeviceGetUuid, "cuDeviceGetUuid");
        bind(api.library, api.cuDeviceComputeCapability, "cuDeviceComputeCapability");
        bind(api.library, api.cuDeviceTotalMem, "cuDeviceTotalMem_v2");
        bind(api.library, api.cuCtxCreate, "cuCtxCreate_v2");
        bind(api.library, api.cuCtxDestroy, "cuCtxDestroy_v2");
        bind(api.library, api.cuCtxPushCurrent, "cuCtxPushCurrent_v2");
        bind(api.library, api.cuCtxPopCurrent, "cuCtxPopCurrent_v2");
        bind(api.library, api.cuMemAlloc, "cuMemAlloc_v2");
        bind(api.library, api.cuMemFree, "cuMemFree_v2");
        bind(api.library, api.cuMemcpyHtoD, "cuMemcpyHtoD_v2");
        bind(api.library, api.cuMemcpyDtoH, "cuMemcpyDtoH_v2");
        bind(api.library, api.cuModuleLoadData, "cuModuleLoadData");
        bind(api.library, api.cuModuleUnload, "cuModuleUnload");
        bind(api.library, api.cuModuleGetFunction, "cuModuleGetFunction");
        bind(api.library, api.cuLaunchKernel, "cuLaunchKernel");
        bind(api.library, api.cuCtxSynchronize, "cuCtxSynchronize");
        bind(api.library, api.cuGetErrorName, "cuGetErrorName");
    }
    const bool complete = api.cuInit != nullptr && api.cuDeviceGetCount != nullptr &&
                          api.cuDeviceGet != nullptr && api.cuCtxCreate != nullptr &&
                          api.cuMemAlloc != nullptr && api.cuMemcpyHtoD != nullptr &&
                          api.cuMemcpyDtoH != nullptr && api.cuModuleLoadData != nullptr &&
                          api.cuModuleGetFunction != nullptr && api.cuLaunchKernel != nullptr &&
                          api.cuCtxSynchronize != nullptr && api.cuCtxPushCurrent != nullptr &&
                          api.cuCtxPopCurrent != nullptr && api.cuMemFree != nullptr;
    if (!complete) {
        api.detail = "the CUDA driver library is missing required entry points";
        reason = api.detail;
        return false;
    }
    const CUresult result = api.cuInit(0);
    if (result != kSuccess) {
        api.detail = "cuInit failed: " + error_name(result);
        reason = api.detail;
        return false;
    }
    int count = 0;
    if (api.cuDeviceGetCount(&count) != kSuccess || count <= 0) {
        api.detail = "no CUDA device is present";
        reason = api.detail;
        return false;
    }
    api.initialized = true;
    reason.clear();
    return true;
}

DeviceInfo describe_device(int ordinal) {
    DeviceInfo info;
    std::string reason;
    if (!driver_available(reason)) {
        info.availability = Availability::Unsupported;
        info.detail = reason;
        return info;
    }
    std::lock_guard<std::mutex> guard(driver_mutex());
    Driver& api = driver();
    CUdevice device = 0;
    if (api.cuDeviceGet(&device, ordinal) != kSuccess) {
        info.availability = Availability::Failed;
        info.detail = "cuDeviceGet failed for ordinal " + std::to_string(ordinal);
        return info;
    }
    info.ordinal = ordinal;
    char name[256] = {};
    if (api.cuDeviceGetName != nullptr && api.cuDeviceGetName(name, sizeof(name), device) == kSuccess) {
        info.name = name;
    }
    if (api.cuDeviceGetUuid != nullptr) {
        CUuuid uuid{};
        if (api.cuDeviceGetUuid(&uuid, device) == kSuccess) {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string text;
            text.reserve(36);
            for (int i = 0; i < 16; ++i) {
                if (i == 4 || i == 6 || i == 8 || i == 10) {
                    text.push_back('-');
                }
                const auto byte = static_cast<std::uint8_t>(uuid.bytes[i]);
                text.push_back(kDigits[byte >> 4]);
                text.push_back(kDigits[byte & 0x0F]);
            }
            info.uuid = text;
        }
    }
    if (api.cuDeviceComputeCapability != nullptr) {
        int major = 0;
        int minor = 0;
        if (api.cuDeviceComputeCapability(&major, &minor, device) == kSuccess) {
            info.compute_major = major;
            info.compute_minor = minor;
        }
    }
    if (api.cuDeviceTotalMem != nullptr) {
        std::size_t total = 0;
        if (api.cuDeviceTotalMem(&total, device) == kSuccess) {
            info.total_memory = total;
        }
    }
    info.availability = Availability::Real;
    info.detail = "driver reported a real device";
    return info;
}

DeviceBuffer::~DeviceBuffer() { release(); }

DeviceBuffer::DeviceBuffer(DeviceBuffer&& other) noexcept
    : pointer_(other.pointer_), size_(other.size_) {
    other.pointer_ = 0;
    other.size_ = 0;
}

DeviceBuffer& DeviceBuffer::operator=(DeviceBuffer&& other) noexcept {
    if (this != &other) {
        release();
        pointer_ = other.pointer_;
        size_ = other.size_;
        other.pointer_ = 0;
        other.size_ = 0;
    }
    return *this;
}

void DeviceBuffer::release() {
    if (pointer_ == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(driver_mutex());
    Driver& api = driver();
    CUcontext context = shared_context();
    if (context != nullptr && api.cuMemFree != nullptr) {
        CurrentContext current(context);
        (void)api.cuMemFree(pointer_);
    }
    pointer_ = 0;
    size_ = 0;
}

bool DeviceBuffer::allocate(std::uint64_t bytes, std::string& error) {
    release();
    std::lock_guard<std::mutex> guard(driver_mutex());
    Driver& api = driver();
    CUcontext context = shared_context();
    if (context == nullptr) {
        error = "no usable CUDA context";
        return false;
    }
    CurrentContext current(context);
    CUdeviceptr pointer = 0;
    const CUresult result = api.cuMemAlloc(&pointer, static_cast<std::size_t>(bytes));
    if (result != kSuccess) {
        error = "cuMemAlloc failed: " + error_name(result);
        return false;
    }
    pointer_ = pointer;
    size_ = bytes;
    return true;
}

bool DeviceBuffer::copy_in(const void* host, std::uint64_t bytes, std::string& error) {
    if (!valid() || bytes > size_) {
        error = "device buffer is too small for the transfer";
        return false;
    }
    std::lock_guard<std::mutex> guard(driver_mutex());
    Driver& api = driver();
    CurrentContext current(shared_context());
    const CUresult result = api.cuMemcpyHtoD(pointer_, host, static_cast<std::size_t>(bytes));
    if (result != kSuccess) {
        error = "cuMemcpyHtoD failed: " + error_name(result);
        return false;
    }
    return true;
}

bool DeviceBuffer::copy_out(void* host, std::uint64_t bytes, std::string& error) {
    if (!valid() || bytes > size_) {
        error = "device buffer is too small for the transfer";
        return false;
    }
    std::lock_guard<std::mutex> guard(driver_mutex());
    Driver& api = driver();
    CurrentContext current(shared_context());
    const CUresult result = api.cuMemcpyDtoH(host, pointer_, static_cast<std::size_t>(bytes));
    if (result != kSuccess) {
        error = "cuMemcpyDtoH failed: " + error_name(result);
        return false;
    }
    return true;
}

bool launch_scale_kernel(DeviceBuffer& out, const DeviceBuffer& in, std::uint32_t count,
                         std::string& error) {
    if (!out.valid() || !in.valid()) {
        error = "device buffers are not allocated";
        return false;
    }
    std::lock_guard<std::mutex> guard(driver_mutex());
    Driver& api = driver();
    CUcontext context = shared_context();
    if (context == nullptr) {
        error = "no usable CUDA context";
        return false;
    }
    CurrentContext current(context);

    CUmodule module = nullptr;
    CUresult result = api.cuModuleLoadData(&module, kScaleKernelPtx);
    if (result != kSuccess) {
        error = "cuModuleLoadData failed: " + error_name(result);
        return false;
    }
    CUfunction function = nullptr;
    result = api.cuModuleGetFunction(&function, module, "pef_scale_kernel");
    if (result != kSuccess) {
        if (api.cuModuleUnload != nullptr) {
            (void)api.cuModuleUnload(module);
        }
        error = "cuModuleGetFunction failed: " + error_name(result);
        return false;
    }

    std::uint64_t out_pointer = out.native_pointer();
    std::uint64_t in_pointer = in.native_pointer();

    void* arguments[] = {&out_pointer, &in_pointer, &count};
    constexpr unsigned kThreads = 128;
    const unsigned blocks = (count + kThreads - 1) / kThreads;
    result = api.cuLaunchKernel(function, blocks, 1, 1, kThreads, 1, 1, 0, nullptr, arguments,
                                nullptr);
    if (result != kSuccess) {
        if (api.cuModuleUnload != nullptr) {
            (void)api.cuModuleUnload(module);
        }
        error = "cuLaunchKernel failed: " + error_name(result);
        return false;
    }
    result = api.cuCtxSynchronize();
    if (api.cuModuleUnload != nullptr) {
        (void)api.cuModuleUnload(module);
    }
    if (result != kSuccess) {
        error = "cuCtxSynchronize failed: " + error_name(result);
        return false;
    }
    return true;
}

bool synchronize(std::string& error) {
    std::lock_guard<std::mutex> guard(driver_mutex());
    Driver& api = driver();
    CUcontext context = shared_context();
    if (context == nullptr) {
        error = "no usable CUDA context";
        return false;
    }
    CurrentContext current(context);
    const CUresult result = api.cuCtxSynchronize();
    if (result != kSuccess) {
        error = "cuCtxSynchronize failed: " + error_name(result);
        return false;
    }
    return true;
}

}  // namespace pef::cuda
