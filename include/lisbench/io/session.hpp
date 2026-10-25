#pragma once

// Runs a lis01::Link over a real byte stream (TCP socket or serial port).
//
// Everything happens on one executor: the session must be used from the
// io_context thread it was created on. run() returns when the connection is
// gone; afterwards every message that was submitted has a result.

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "lisbench/io/endpoint.hpp"
#include "lisbench/io/faults.hpp"
#include "lisbench/lis01_link.hpp"

namespace lisbench::io {

using Stream = std::variant<asio::ip::tcp::socket, asio::serial_port>;

enum class Direction : std::uint8_t { in, out };

struct WireEvent {
    Direction direction{};
    std::chrono::system_clock::time_point at{};
    bytes data{};
    std::optional<Fault> fault{};  // out: the fault that changed these bytes
};

// One received transfer, ENQ .. EOT (or until it broke off). complete means
// "ended by EOT". A sender that gives up also sends EOT, so whether the
// message is whole can only be told from its content (the L record).
struct Transfer {
    std::vector<bytes> records{};
    bool complete{false};
    std::string reason{};
};

struct SessionHandlers {
    std::function<void(const WireEvent&)> on_wire;
    std::function<void(const Transfer&)> on_transfer;
    std::function<void(const std::string&)> on_warning;
};

struct SessionOptions {
    lis01::Role role{lis01::Role::instrument};
    lis01::Timing timing{};
    std::vector<Fault> faults{};
};

class Session {
public:
    Session(Stream stream, SessionOptions options, SessionHandlers handlers);
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() = default;

    asio::awaitable<void> run();

    lis01::MessageId submit(const lis01::Message& message);
    // Submits and waits for the result of this message.
    asio::awaitable<lis01::Outcome> send(lis01::Message message);
    // Next received transfer, or nullopt after the timeout or disconnect.
    asio::awaitable<std::optional<Transfer>> receive(std::chrono::milliseconds timeout);

    // Closes the stream; run() then finishes.
    void stop();
    [[nodiscard]] bool running() const noexcept { return !closing_; }

private:
    asio::awaitable<void> read_loop();
    asio::awaitable<void> write_loop();
    asio::awaitable<void> timer_loop();
    void dispatch(const lis01::Events& events);
    void notify();
    void close_stream();

    Stream stream_;
    lis01::Link link_;
    FaultFilter faults_{};
    SessionHandlers handlers_{};

    asio::steady_timer link_timer_;
    lis01::TimerId armed_{0};
    asio::steady_timer write_wake_;
    asio::steady_timer wake_;  // waiters in send() / receive()

    std::deque<Outgoing> write_queue_{};
    std::map<lis01::MessageId, lis01::Outcome> results_{};
    std::vector<bytes> records_{};
    std::deque<Transfer> transfers_{};
    bool closing_{false};
};

// Opening connections.
asio::awaitable<Stream> connect(Endpoint endpoint);
[[nodiscard]] asio::ip::tcp::acceptor make_acceptor(const asio::any_io_executor& executor,
                                                    const Tcp& tcp);
asio::awaitable<Stream> accept(
    asio::ip::tcp::acceptor&
        acceptor);  // NOLINT(cppcoreguidelines-avoid-reference-coroutine-parameters)

}  // namespace lisbench::io
