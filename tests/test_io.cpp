#include <catch2/catch_test_macros.hpp>

#include "lisbench/io/endpoint.hpp"
#include "lisbench/io/faults.hpp"
#include "lisbench/lis01_frame.hpp"

using namespace lisbench;
using namespace lisbench::io;

TEST_CASE("endpoints are parsed from URIs") {
    CHECK(parse_endpoint("tcp://10.0.0.5:5000").value() ==
          Endpoint{Tcp{.host = "10.0.0.5", .port = 5000, .listen = false}});
    CHECK(parse_endpoint("tcp-listen://:5000").value() ==
          Endpoint{Tcp{.host = "0.0.0.0", .port = 5000, .listen = true}});
    CHECK(parse_endpoint("tcp-listen://127.0.0.1:0"));

    const auto s = std::get<Serial>(
        parse_endpoint("serial:/dev/ttyUSB0?baud=19200&data=7&parity=even&stop=2&flow=rtscts")
            .value());
    CHECK(s.device == "/dev/ttyUSB0");
    CHECK(s.baud == 19200);
    CHECK(s.data_bits == 7);
    CHECK(s.parity == Parity::even);
    CHECK(s.stop_bits == StopBits::two);
    CHECK(s.flow == FlowControl::hardware);
    CHECK(std::get<Serial>(parse_endpoint("serial:COM3").value()) == Serial{.device = "COM3"});

    CHECK(to_string(parse_endpoint("serial://COM3?baud=9600").value()) ==
          "serial:COM3?baud=9600&data=8&parity=none&stop=1&flow=none");
    CHECK(to_string(parse_endpoint("tcp://h:1").value()) == "tcp://h:1");

    for (const auto* bad : {"http://x", "tcp://host", "tcp://:5000", "tcp://h:0", "tcp://h:70000",
                            "serial:", "serial:/dev/x?baud=fast", "serial:/dev/x?data=9",
                            "serial:/dev/x?parity=mark", "serial:/dev/x?speed=1"}) {
        INFO(bad);
        CHECK_FALSE(parse_endpoint(bad));
    }
}

TEST_CASE("endpoints: the low-level protocol is part of the URI") {
    const auto t = parse_endpoint("tcp://h:5000?low-level=mllp").value();
    CHECK(low_level_of(t) == LowLevel::mllp);
    CHECK(to_string(t) == "tcp://h:5000?low-level=mllp");
    CHECK(low_level_of(parse_endpoint("tcp-listen://:5000?low-level=none").value()) ==
          LowLevel::none);
    const auto s = parse_endpoint("serial:/dev/ttyS0?baud=9600&low-level=none").value();
    CHECK(low_level_of(s) == LowLevel::none);
    CHECK(to_string(s).ends_with("&flow=none&low-level=none"));
    CHECK(low_level_of(parse_endpoint("tcp://h:1").value()) == LowLevel::lis01_a2);
    CHECK(to_string(parse_endpoint("tcp://h:1?low-level=lis01-a2").value()) == "tcp://h:1");
    for (const auto* bad :
         {"tcp://h:1?low-level=hl7", "tcp://h:1?baud=9600", "serial:/dev/x?low-level=raw"}) {
        INFO(bad);
        CHECK_FALSE(parse_endpoint(bad));
    }
}

TEST_CASE("fault filter: selection by occurrence") {
    FaultFilter f{{Fault{.target = FaultTarget::ack, .action = FaultAction::nak, .nth = 2},
                   Fault{.target = FaultTarget::enq, .action = FaultAction::drop, .every = 2}}};
    const bytes ack{lis01::ctl::ack};
    const bytes enq{lis01::ctl::enq};
    CHECK(f.apply(ack)->data == ack);
    const auto second = f.apply(ack);
    REQUIRE(second);
    CHECK(second->data == bytes{lis01::ctl::nak});
    CHECK(second->applied.has_value());
    CHECK(f.apply(ack)->data == ack);

    CHECK(f.apply(enq).has_value());
    CHECK_FALSE(f.apply(enq).has_value());  // every 2nd dropped
    CHECK(f.apply(enq).has_value());
}

TEST_CASE("fault filter: a corrupted frame fails only its checksum") {
    FaultFilter f{{Fault{.target = FaultTarget::frame, .action = FaultAction::corrupt}}};
    const auto frame =
        lis01::encode(lis01::Frame{.number = 1, .last = true, .text = to_bytes("L|1|N\r")});
    const auto out = f.apply(frame);
    REQUIRE(out);
    CHECK(out->data.size() == frame.size());
    CHECK(lis01::decode(out->data).error() == lis01::FrameError::checksum_mismatch);
}

TEST_CASE("fault filter: delay and untouched bytes") {
    FaultFilter f{{Fault{
        .target = FaultTarget::eot, .action = FaultAction::delay, .delay = milliseconds{250}}}};
    CHECK(f.apply(bytes{lis01::ctl::eot})->delay == milliseconds{250});
    CHECK(f.apply(bytes{lis01::ctl::ack})->delay == milliseconds{0});
    CHECK(FaultFilter::classify(to_bytes("xy")) == std::nullopt);
    CHECK(fault_target("frame") == FaultTarget::frame);
    CHECK(fault_action("corrupt") == FaultAction::corrupt);
    CHECK_FALSE(fault_target("bogus"));
}
