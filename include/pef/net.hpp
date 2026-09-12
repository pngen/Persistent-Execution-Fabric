// Persistent Execution Fabric - loopback TCP transport.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Narrow socket wrappers. The control plane uses them; nothing else in the
// fabric does. Sockets are a transport only: no authority decision is ever made
// from a connection, an address, or a port.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "pef/protocol.hpp"
#include "pef/status.hpp"

namespace pef {

// Initializes the platform socket layer once per process.
void net_initialize();
void net_shutdown();

class Socket {
public:
    // Declared here and defined in the implementation so that the incomplete
    // native handle type is never needed by an inline special member.
    Socket();
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    void close() noexcept;

    [[nodiscard]] Status send_all(const void* data, std::size_t size);
    // Reads exactly size bytes or fails. Used for the fixed frame header.
    [[nodiscard]] Status recv_exact(void* data, std::size_t size);
    // Reads up to size bytes. Returns 0 bytes on a clean end of stream.
    [[nodiscard]] Status recv_some(void* data, std::size_t size, std::size_t& received);

    void set_read_timeout_ms(int milliseconds);
    void set_no_delay(bool enabled);

    [[nodiscard]] std::uint16_t local_port() const;

    struct Native;

    // Adopts an already-connected native handle.
    [[nodiscard]] static Socket adopt(std::unique_ptr<Native> native) noexcept;

private:
    std::unique_ptr<Native> native_;
};

class Listener {
public:
    Listener();
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Binds and listens. Port 0 selects an ephemeral port, which is reported by
    // port(). host may be an empty string for the loopback interface.
    [[nodiscard]] Status listen_on(const std::string& host, std::uint16_t port);
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] Status accept(Socket& out);
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept;

    struct Native;

private:
    std::unique_ptr<Native> native_;
    std::uint16_t port_ = 0;
};

// Framed transport over one connection. Reads the fixed header first, enforces
// the protocol payload bound before allocating, then reads exactly the declared
// payload.
class FrameChannel {
public:
    FrameChannel() = default;
    explicit FrameChannel(Socket socket);
    // Constructs a channel over a socket that the caller also retains, so that
    // the server can close it from another thread to unblock a pending read.
    explicit FrameChannel(std::shared_ptr<Socket> socket);

    [[nodiscard]] Status send(const Frame& frame);
    [[nodiscard]] Status receive(Frame& out, std::string& reason);
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept { return socket_ != nullptr && socket_->valid(); }
    void set_read_timeout_ms(int milliseconds) {
        if (socket_) {
            socket_->set_read_timeout_ms(milliseconds);
        }
    }

private:
    std::shared_ptr<Socket> socket_;
};

[[nodiscard]] Status connect_to(const std::string& host, std::uint16_t port, Socket& out,
                                int timeout_ms);

}  // namespace pef
