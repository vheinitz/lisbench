#include "lisbench/io/session.hpp"

#include <asio/experimental/awaitable_operators.hpp>

#include <array>
#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>

namespace lisbench::io {

namespace {

using namespace asio::experimental::awaitable_operators;
constexpr auto use_tuple = asio::as_tuple(asio::use_awaitable);

template <typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

void park(asio::steady_timer& timer) {
    timer.expires_at(asio::steady_timer::time_point::max());
}

asio::serial_port::parity::type to_asio(Parity p) {
    switch (p) {
        case Parity::odd: return asio::serial_port::parity::odd;
        case Parity::even: return asio::serial_port::parity::even;
        case Parity::none: break;
    }
    return asio::serial_port::parity::none;
}

asio::serial_port::stop_bits::type to_asio(StopBits s) {
    switch (s) {
        case StopBits::one_half: return asio::serial_port::stop_bits::onepointfive;
        case StopBits::two: return asio::serial_port::stop_bits::two;
        case StopBits::one: break;
    }
    return asio::serial_port::stop_bits::one;
}

asio::serial_port::flow_control::type to_asio(FlowControl f) {
    switch (f) {
        case FlowControl::software: return asio::serial_port::flow_control::software;
        case FlowControl::hardware: return asio::serial_port::flow_control::hardware;
        case FlowControl::none: break;
    }
    return asio::serial_port::flow_control::none;
}

std::variant<lis01::Link, UnframedLink> make_link(const SessionOptions& o) {
    if (o.low_level == LowLevel::lis01_a2) {
        return lis01::Link{o.role, o.timing};
    }
    return UnframedLink{o.low_level, o.timing};
}

}  // namespace

std::expected<void, std::string> check(const SessionOptions& options) {
    if (!options.faults.empty() && options.low_level != LowLevel::lis01_a2) {
        return std::unexpected(
            std::format("fault injection needs the LIS01-A2 low-level protocol, not '{}'",
                        to_string(options.low_level)));
    }
    return {};
}

Session::Session(Stream stream, SessionOptions options, SessionHandlers handlers)
    : stream_{std::move(stream)},
      link_{make_link(options)},
      faults_{std::move(options.faults)},
      handlers_{std::move(handlers)},
      link_timer_{std::visit([](auto& s) { return s.get_executor(); }, stream_)},
      write_wake_{link_timer_.get_executor()},
      wake_{link_timer_.get_executor()} {
    if (const auto ok = check(options); !ok) {
        throw std::invalid_argument(ok.error());
    }
    park(link_timer_);
    park(write_wake_);
    park(wake_);
}

asio::awaitable<void> Session::run() {
    co_await (read_loop() && write_loop() && timer_loop());
    // Whatever is still queued gets its result now.
    lis01::Events ev;
    with_link([&ev](auto& l) { l.cancel_all(ev); });
    dispatch(ev);
    notify();
}

void Session::notify() {
    wake_.cancel();
}

void Session::close_stream() {
    std::error_code ec;
    std::visit([&ec](auto& s) { s.close(ec); }, stream_);
}

void Session::stop() {
    closing_ = true;
    close_stream();
    write_wake_.cancel();
    link_timer_.cancel();
}

lis01::MessageId Session::submit(const lis01::Message& message) {
    lis01::Events ev;
    const auto id = with_link([&](auto& l) { return l.submit(message, ev); });
    if (closing_) {
        with_link([&](auto& l) { l.cancel(id, ev); });
    }
    dispatch(ev);
    return id;
}

void Session::set_receiver_ready(bool ready) {
    with_link([ready](auto& l) { l.set_receiver_ready(ready); });
}

asio::awaitable<lis01::Outcome> Session::send(lis01::Message message) {
    const auto id = submit(message);
    while (!results_.contains(id)) {
        co_await wake_.async_wait(use_tuple);
    }
    co_return results_.at(id);
}

asio::awaitable<std::optional<Transfer>> Session::receive(std::chrono::milliseconds timeout) {
    asio::steady_timer deadline{wake_.get_executor(), timeout};
    while (transfers_.empty()) {
        if (closing_ || deadline.expiry() <= asio::steady_timer::clock_type::now()) {
            co_return std::nullopt;
        }
        co_await (wake_.async_wait(use_tuple) || deadline.async_wait(use_tuple));
    }
    auto t = std::move(transfers_.front());
    transfers_.pop_front();
    co_return t;
}

asio::awaitable<void> Session::read_loop() {
    std::array<std::byte, 1024> buffer{};
    while (!closing_) {
        const auto [ec, n] = co_await std::visit(
            [&buffer](auto& s) { return s.async_read_some(asio::buffer(buffer), use_tuple); },
            stream_);
        if (ec || n == 0) {
            break;
        }
        const bytes_view data{buffer.data(), n};
        if (handlers_.on_wire) {
            handlers_.on_wire(WireEvent{.direction = Direction::in,
                                        .at = std::chrono::system_clock::now(),
                                        .data = bytes(data.begin(), data.end()),
                                        .fault = std::nullopt});
        }
        lis01::Events ev;
        with_link([&](auto& l) { l.on_bytes(data, ev); });
        dispatch(ev);
    }
    closing_ = true;
    lis01::Events ev;
    with_link([&ev](auto& l) { l.on_disconnect(ev); });
    dispatch(ev);
    write_wake_.cancel();
    link_timer_.cancel();
    notify();
}

asio::awaitable<void> Session::write_loop() {
    while (true) {
        if (write_queue_.empty()) {
            if (closing_) {
                co_return;
            }
            co_await write_wake_.async_wait(use_tuple);
            continue;
        }
        auto item = std::move(write_queue_.front());
        write_queue_.pop_front();
        if (item.delay.count() > 0) {
            asio::steady_timer hold{write_wake_.get_executor(), item.delay};
            co_await hold.async_wait(use_tuple);
        }
        if (closing_) {
            co_return;
        }
        const auto [ec, n] = co_await std::visit(
            [&item](auto& s) { return asio::async_write(s, asio::buffer(item.data), use_tuple); },
            stream_);
        if (ec) {
            close_stream();  // read_loop sees the error and ends the session
            co_return;
        }
        if (handlers_.on_wire) {
            handlers_.on_wire(WireEvent{.direction = Direction::out,
                                        .at = std::chrono::system_clock::now(),
                                        .data = std::move(item.data),
                                        .fault = item.applied});
        }
    }
}

asio::awaitable<void> Session::timer_loop() {
    while (!closing_) {
        const auto [ec] = co_await link_timer_.async_wait(use_tuple);
        if (closing_) {
            co_return;
        }
        if (ec || armed_ == 0) {
            continue;  // re-armed or cancelled
        }
        const auto id = std::exchange(armed_, 0);
        park(link_timer_);
        lis01::Events ev;
        with_link([&](auto& l) { l.on_timer(id, ev); });
        dispatch(ev);
    }
}

void Session::dispatch(const lis01::Events& events) {
    for (const auto& e : events) {
        std::visit(overloaded{
                       [this](const lis01::ev::Send& s) {
                           if (auto out = faults_.apply(s.data)) {
                               write_queue_.push_back(std::move(*out));
                               write_wake_.cancel();
                           }
                       },
                       [this](const lis01::ev::ArmTimer& t) {
                           armed_ = t.id;
                           link_timer_.expires_after(t.after);
                       },
                       [this](const lis01::ev::CancelTimer&) {
                           armed_ = 0;
                           park(link_timer_);
                       },
                       [this](const lis01::ev::SendFinished& f) {
                           results_[f.id] = f.outcome;
                           notify();
                       },
                       [this](const lis01::ev::RecordReceived& r) { records_.push_back(r.record); },
                       [this](const lis01::ev::ReceiveFinished& f) {
                           Transfer t{.records = std::exchange(records_, {}),
                                      .complete = f.complete,
                                      .reason = f.reason};
                           if (handlers_.on_transfer) {
                               handlers_.on_transfer(t);
                           }
                           transfers_.push_back(std::move(t));
                           notify();
                       },
                       [this](const lis01::ev::Warning& w) {
                           const auto& handler = w.kind == lis01::ev::Warning::Kind::start_noise
                                                     ? handlers_.on_note
                                                     : handlers_.on_warning;
                           if (handler) {
                               handler(w.text);
                           }
                       },
                   },
                   e);
    }
}

asio::awaitable<Stream> connect(Endpoint endpoint) {
    auto executor = co_await asio::this_coro::executor;
    if (const auto* tcp = std::get_if<Tcp>(&endpoint)) {
        asio::ip::tcp::resolver resolver{executor};
        const auto results = co_await resolver.async_resolve(tcp->host, std::to_string(tcp->port),
                                                             asio::use_awaitable);
        asio::ip::tcp::socket socket{executor};
        co_await asio::async_connect(socket, results, asio::use_awaitable);
        socket.set_option(asio::ip::tcp::no_delay{true});
        co_return Stream{std::move(socket)};
    }
    const auto& s = std::get<Serial>(endpoint);
    asio::serial_port port{executor, s.device};
    port.set_option(asio::serial_port::baud_rate{s.baud});
    port.set_option(asio::serial_port::character_size{s.data_bits});
    port.set_option(asio::serial_port::parity{to_asio(s.parity)});
    port.set_option(asio::serial_port::stop_bits{to_asio(s.stop_bits)});
    port.set_option(asio::serial_port::flow_control{to_asio(s.flow)});
    // Bytes that arrived while nobody listened (an instrument retrying ENQ
    // for minutes) belong to no conversation; answering them would send a
    // burst of stale ACKs. USB adapters deliver such a backlog at once, even
    // after the port was flushed, so read and drop until the line has been
    // quiet for a moment (at most 2 s, in case the partner never pauses).
    std::array<char, 512> junk{};
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer quiet{executor, std::chrono::milliseconds{30}};
        const auto r = co_await (port.async_read_some(asio::buffer(junk), use_tuple) ||
                                 quiet.async_wait(use_tuple));
        if (r.index() == 1 || std::get<0>(std::get<0>(r))) {
            break;  // quiet, or the port failed (reported by the first real read)
        }
    }
    co_return Stream{std::move(port)};
}

asio::ip::tcp::acceptor make_acceptor(const asio::any_io_executor& executor, const Tcp& tcp) {
    const asio::ip::tcp::endpoint ep{asio::ip::make_address(tcp.host), tcp.port};
    asio::ip::tcp::acceptor acceptor{executor};
    acceptor.open(ep.protocol());
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address{true});
    acceptor.bind(ep);
    acceptor.listen();
    return acceptor;
}

// The acceptor is owned by the caller, who awaits this right away.
// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
asio::awaitable<Stream> accept(asio::ip::tcp::acceptor& acceptor) {
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    socket.set_option(asio::ip::tcp::no_delay{true});
    co_return Stream{std::move(socket)};
}

}  // namespace lisbench::io
