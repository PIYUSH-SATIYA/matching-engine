#include "wire.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "fail: " << message << '\n';
        ++failures;
    }
}

wire::Msg makeMsg(std::uint64_t id, std::uint8_t side, std::int64_t price,
                  std::uint64_t qty, std::uint64_t ts = 1234) {
    wire::Msg m{};
    m.id = id;
    m.side = side;
    m.price = price;
    m.quantity = qty;
    m.client_ts_ns = ts;
    std::memset(m.pad, 0, sizeof(m.pad));
    return m;
}

void test_layout() {
    check(sizeof(wire::Msg) == wire::kMsgSize, "wire message must be 40 bytes");
    check(wire::kMsgSize == 40, "protocol constant must be 40");
}

void test_valid_predicate() {
    check(wire::valid(makeMsg(1, 0, 100, 10)), "buy should be valid");
    check(wire::valid(makeMsg(1, 1, 100, 10)), "sell should be valid");
    check(!wire::valid(makeMsg(0, 0, 100, 10)), "id 0 is the shutdown sentinel");
    check(!wire::valid(makeMsg(1, 2, 100, 10)), "side must be 0 or 1");
    check(!wire::valid(makeMsg(1, 0, 100, 0)), "zero quantity is invalid");
}

void test_single_and_coalesced() {
    std::vector<wire::Msg> sent = {
        makeMsg(1, 0, 100, 10), makeMsg(2, 1, 101, 20),
        makeMsg(3, 0, 102, 30)};
    char buf[3 * wire::kMsgSize];
    std::memcpy(buf, sent.data(), sizeof(buf));

    std::vector<std::uint64_t> got;
    std::uint64_t invalid = 0;
    std::size_t used = wire::parse(buf, sizeof(buf), invalid,
                                   [&](const wire::Msg& m) { got.push_back(m.id); });

    check(used == 0, "full buffer must leave no partial message");
    check(invalid == 0, "valid messages must not be counted invalid");
    check(got.size() == 3, "all three coalesced messages must be parsed");
    check(got[0] == 1 && got[1] == 2 && got[2] == 3, "order must be preserved");
}

void test_split_message() {
    const wire::Msg m = makeMsg(7, 1, 4242, 5, 999);
    const char* bytes = reinterpret_cast<const char*>(&m);
    char buf[2 * wire::kMsgSize] = {};

    std::vector<std::uint64_t> got;
    std::uint64_t invalid = 0;
    auto on_msg = [&](const wire::Msg& msg) { got.push_back(msg.id); };

    // First 17 bytes: not even a header.
    std::memcpy(buf, bytes, 17);
    std::size_t used = wire::parse(buf, 17, invalid, on_msg);
    check(used == 17, "partial header must stay buffered");
    check(got.empty(), "no message parsed from a partial header");

    // Next 23 bytes complete the message.
    std::memcpy(buf + used, bytes + used, wire::kMsgSize - used);
    used = wire::parse(buf, wire::kMsgSize, invalid, on_msg);
    check(used == 0, "complete message must consume the buffer");
    check(got.size() == 1 && got[0] == 7, "split message must parse exactly once");
    check(invalid == 0, "split message must not be invalid");
}

void test_coalesced_plus_partial() {
    const wire::Msg a = makeMsg(10, 0, 100, 1);
    const wire::Msg b = makeMsg(11, 1, 101, 2);
    char buf[3 * wire::kMsgSize] = {};
    std::memcpy(buf, &a, wire::kMsgSize);
    std::memcpy(buf + wire::kMsgSize, &b, wire::kMsgSize);
    std::memcpy(buf + 2 * wire::kMsgSize, &a, 13);  // partial third

    std::vector<std::uint64_t> got;
    std::uint64_t invalid = 0;
    std::size_t used =
        wire::parse(buf, 2 * wire::kMsgSize + 13, invalid,
                    [&](const wire::Msg& m) { got.push_back(m.id); });

    check(got.size() == 2, "two complete messages must be parsed");
    check(used == 13, "partial tail must be retained");
    check(std::memcmp(buf, &a, 13) == 0,
          "partial tail must be moved to the front of the buffer");
}

void test_invalid_messages() {
    wire::Msg bad_id = makeMsg(0, 0, 100, 1);
    wire::Msg bad_side = makeMsg(5, 9, 100, 1);
    wire::Msg bad_qty = makeMsg(6, 1, 100, 0);
    wire::Msg good = makeMsg(7, 1, 100, 3);
    wire::Msg msgs[4] = {bad_id, bad_side, bad_qty, good};

    char buf[sizeof(msgs)];
    std::memcpy(buf, msgs, sizeof(msgs));

    std::uint64_t invalid = 0;
    std::vector<std::uint64_t> got;
    std::size_t used = wire::parse(buf, sizeof(buf), invalid,
                                   [&](const wire::Msg& m) { got.push_back(m.id); });

    check(used == 0, "invalid messages must be consumed, not buffered");
    check(invalid == 3, "three invalid messages must be counted");
    check(got.size() == 1 && got[0] == 7, "only the valid message is delivered");
}

void test_truncated_stream() {
    const wire::Msg m = makeMsg(21, 0, 100, 1);
    char buf[wire::kMsgSize - 1];
    std::memcpy(buf, &m, sizeof(buf));

    std::uint64_t invalid = 0;
    std::size_t used = wire::parse(buf, sizeof(buf), invalid,
                                   [](const wire::Msg&) {});
    check(used == sizeof(buf), "truncated message must be fully retained");
    check(invalid == 0, "truncated stream must not be marked invalid");
}

void test_to_order() {
    const wire::Msg m = makeMsg(33, 1, 555, 9, 777);
    const Order o = wire::toOrder(m);
    check(o.id == 33, "id conversion");
    check(o.side == Side::Sell, "side conversion");
    check(o.price == 555, "price conversion");
    check(o.quantity == 9, "quantity conversion");
    check(o.submit_time ==
              Timestamp{} + std::chrono::nanoseconds(777),
          "client timestamp conversion");
}

}  // namespace

int main() {
    test_layout();
    test_valid_predicate();
    test_single_and_coalesced();
    test_split_message();
    test_coalesced_plus_partial();
    test_invalid_messages();
    test_truncated_stream();
    test_to_order();

    if (failures != 0) {
        std::cerr << failures << " test check(s) failed.\n";
        return 1;
    }
    std::cout << "All wire tests passed.\n";
    return 0;
}
