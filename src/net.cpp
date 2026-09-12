// Persistent Execution Fabric - loopback TCP transport.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "pef/net.hpp"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pef {
namespace {

#if defined(_WIN32)
using NativeHandle = SOCKET;
constexpr NativeHandle kInvalidHandle = INVALID_SOCKET;
#else
using NativeHandle = int;
constexpr NativeHandle kInvalidHandle = -1;
#endif

std::atomic<bool> g_net_ready{false};

[[nodiscard]] std::string socket_error_text() {
#if defined(_WIN32)
    return "socket error " + std::to_string(::WSAGetLastError());
#else
    return std::string("socket error ") + std::strerror(errno);
#endif
}

void close_handle(NativeHandle handle) {
    if (handle == kInvalidHandle) {
        return;
    }
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

}  // namespace

struct Socket::Native {
    NativeHandle handle = kInvalidHandle;
};

struct Listener::Native {
    NativeHandle handle = kInvalidHandle;
};

void net_initialize() {
    if (g_net_ready.exchange(true)) {
        return;
    }
#if defined(_WIN32)
    WSADATA data{};
    ::WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

void net_shutdown() {
    if (!g_net_ready.exchange(false)) {
        return;
    }
#if defined(_WIN32)
    ::WSACleanup();
#endif
}

Socket::Socket() = default;

Socket Socket::adopt(std::unique_ptr<Native> native) noexcept {
    Socket socket;
    socket.native_ = std::move(native);
    return socket;
}

Socket::Socket(Socket&& other) noexcept : native_(std::move(other.native_)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        native_ = std::move(other.native_);
    }
    return *this;
}

Socket::~Socket() { close(); }

bool Socket::valid() const noexcept {
    return native_ != nullptr && native_->handle != kInvalidHandle;
}

void Socket::close() noexcept {
    if (native_ != nullptr) {
        close_handle(native_->handle);
        native_->handle = kInvalidHandle;
        native_.reset();
    }
}

Status Socket::send_all(const void* data, std::size_t size) {
    if (!valid()) {
        return err(Code::TransportFailure, "send on a closed socket");
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
#if defined(_WIN32)
        const int sent = ::send(native_->handle, reinterpret_cast<const char*>(p), chunk, 0);
#else
        const int sent = static_cast<int>(
            ::send(native_->handle, p, static_cast<std::size_t>(chunk), MSG_NOSIGNAL));
#endif
        if (sent <= 0) {
            return err(Code::TransportFailure, "send failed: " + socket_error_text());
        }
        p += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return ok_status();
}

Status Socket::recv_exact(void* data, std::size_t size) {
    auto* p = static_cast<std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        std::size_t received = 0;
        PEF_TRY(recv_some(p, remaining, received));
        if (received == 0) {
            return err(Code::TransportFailure, "connection closed while reading a frame");
        }
        p += received;
        remaining -= received;
    }
    return ok_status();
}

Status Socket::recv_some(void* data, std::size_t size, std::size_t& received) {
    received = 0;
    if (!valid()) {
        return err(Code::TransportFailure, "receive on a closed socket");
    }
    const int chunk = static_cast<int>(size > 1u << 20 ? 1u << 20 : size);
#if defined(_WIN32)
    const int got = ::recv(native_->handle, static_cast<char*>(data), chunk, 0);
#else
    const int got = static_cast<int>(::recv(native_->handle, data, static_cast<std::size_t>(chunk), 0));
#endif
    if (got < 0) {
        return err(Code::TransportFailure, "receive failed: " + socket_error_text());
    }
    received = static_cast<std::size_t>(got);
    return ok_status();
}

void Socket::set_read_timeout_ms(int milliseconds) {
    if (!valid()) {
        return;
    }
#if defined(_WIN32)
    const DWORD timeout = static_cast<DWORD>(milliseconds);
    ::setsockopt(native_->handle, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv {};
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    ::setsockopt(native_->handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void Socket::set_no_delay(bool enabled) {
    if (!valid()) {
        return;
    }
    const int value = enabled ? 1 : 0;
#if defined(_WIN32)
    ::setsockopt(native_->handle, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&value), sizeof(value));
#else
    ::setsockopt(native_->handle, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
#endif
}

std::uint16_t Socket::local_port() const {
    if (!valid()) {
        return 0;
    }
    sockaddr_in address{};
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (::getsockname(native_->handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

Listener::Listener() = default;

Listener::~Listener() { close(); }

bool Listener::valid() const noexcept {
    return native_ != nullptr && native_->handle != kInvalidHandle;
}

void Listener::close() noexcept {
    if (native_ != nullptr) {
        close_handle(native_->handle);
        native_->handle = kInvalidHandle;
        native_.reset();
    }
}

Status Listener::listen_on(const std::string& host, std::uint16_t port) {
    net_initialize();
    close();
    const NativeHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidHandle) {
        return err(Code::TransportFailure, "cannot create listener socket: " + socket_error_text());
    }
    const int reuse = 1;
#if defined(_WIN32)
    ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (host.empty() || host == "localhost") {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        close_handle(handle);
        return err(Code::InvalidArgument, "listener host is not a valid IPv4 address");
    }
    if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_handle(handle);
        return err(Code::TransportFailure,
                   "cannot bind listener on port " + std::to_string(port) + ": " +
                       socket_error_text());
    }
    if (::listen(handle, 64) != 0) {
        close_handle(handle);
        return err(Code::TransportFailure, "cannot listen: " + socket_error_text());
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int length = sizeof(bound);
#else
    socklen_t length = sizeof(bound);
#endif
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }
    native_ = std::make_unique<Native>();
    native_->handle = handle;
    return ok_status();
}

Status Listener::accept(Socket& out) {
    if (!valid()) {
        return err(Code::TransportFailure, "accept on a closed listener");
    }
    sockaddr_in peer{};
#if defined(_WIN32)
    int length = sizeof(peer);
#else
    socklen_t length = sizeof(peer);
#endif
    const NativeHandle handle =
        ::accept(native_->handle, reinterpret_cast<sockaddr*>(&peer), &length);
    if (handle == kInvalidHandle) {
        return err(Code::TransportFailure, "accept failed: " + socket_error_text());
    }
    const int value = 1;
#if defined(_WIN32)
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&value), sizeof(value));
#else
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
#endif
    out.close();
    auto native = std::make_unique<Socket::Native>();
    native->handle = handle;
    out = Socket::adopt(std::move(native));
    return ok_status();
}

Status connect_to(const std::string& host, std::uint16_t port, Socket& out, int timeout_ms) {
    net_initialize();
    const NativeHandle handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidHandle) {
        return err(Code::TransportFailure, "cannot create socket: " + socket_error_text());
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    const std::string target = host.empty() ? std::string("127.0.0.1") : host;
    if (::inet_pton(AF_INET, target.c_str(), &address.sin_addr) != 1) {
        close_handle(handle);
        return err(Code::InvalidArgument, "host is not a valid IPv4 address");
    }
    // Non-blocking connect with a bounded wait, so a coordinator that is not
    // listening fails fast instead of hanging the caller.
#if defined(_WIN32)
    u_long non_blocking = 1;
    ::ioctlsocket(handle, FIONBIO, &non_blocking);
#else
    const int flags = ::fcntl(handle, F_GETFL, 0);
    ::fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif
    const int rc = ::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    bool connected = rc == 0;
    if (!connected) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(handle, &write_set);
        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int selected = ::select(static_cast<int>(handle) + 1, nullptr, &write_set, nullptr, &tv);
        if (selected == 1) {
            int error = 0;
#if defined(_WIN32)
            int length = sizeof(error);
#else
            socklen_t length = sizeof(error);
#endif
            ::getsockopt(handle, SOL_SOCKET, SO_ERROR,
#if defined(_WIN32)
                         reinterpret_cast<char*>(&error),
#else
                         &error,
#endif
                         &length);
            connected = error == 0;
        }
    }
#if defined(_WIN32)
    non_blocking = 0;
    ::ioctlsocket(handle, FIONBIO, &non_blocking);
#else
    ::fcntl(handle, F_SETFL, flags);
#endif
    if (!connected) {
        close_handle(handle);
        return err(Code::TransportFailure, "cannot connect to the coordinator");
    }
    out.close();
    auto native = std::make_unique<Socket::Native>();
    native->handle = handle;
    out = Socket::adopt(std::move(native));
    return ok_status();
}

FrameChannel::FrameChannel(Socket socket) : socket_(std::move(socket)) {}

void FrameChannel::close() noexcept { socket_.close(); }

Status FrameChannel::send(const Frame& frame) {
    const Bytes bytes = encode_frame(frame);
    return socket_.send_all(bytes.data(), bytes.size());
}

Status FrameChannel::receive(Frame& out, std::string& reason) {
    std::uint8_t header[kFrameHeaderSize] = {};
    PEF_TRY(socket_.recv_exact(header, kFrameHeaderSize));
    Frame partial;
    if (!decode_frame_header(header, kFrameHeaderSize, partial, reason)) {
        return err(Code::ProtocolViolation, reason);
    }
    const std::uint32_t payload_size =
        static_cast<std::uint32_t>(header[16]) | (static_cast<std::uint32_t>(header[17]) << 8) |
        (static_cast<std::uint32_t>(header[18]) << 16) |
        (static_cast<std::uint32_t>(header[19]) << 24);
    Bytes whole(kFrameHeaderSize + static_cast<std::size_t>(payload_size) + kFrameTrailerSize,
                std::byte{0});
    std::memcpy(whole.data(), header, kFrameHeaderSize);
    if (payload_size > 0) {
        PEF_TRY(socket_.recv_exact(whole.data() + kFrameHeaderSize, payload_size));
    }
    PEF_TRY(socket_.recv_exact(whole.data() + kFrameHeaderSize + payload_size,
                               kFrameTrailerSize));
    if (!decode_frame(whole, out, reason)) {
        return err(Code::ProtocolViolation, reason);
    }
    return ok_status();
}

}  // namespace pef
