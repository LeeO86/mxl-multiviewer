#include <doctest/doctest.h>

#include "control/tsl.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace mv;

namespace
{
void put16(std::vector<std::uint8_t>& out, int value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}
} // namespace

TEST_CASE("tsl 5.0 display message and dle stuffing")
{
    std::vector<std::uint8_t> body;
    put16(body, 0);
    body.push_back(0);
    body.push_back(0);
    put16(body, 7);
    put16(body, 3);
    std::uint16_t control = 1 | (3 << 2);
    put16(body, control);
    std::string const text = "CA\xFE M";
    put16(body, static_cast<int>(text.size()));
    body.insert(body.end(), text.begin(), text.end());
    body[0] = static_cast<std::uint8_t>((body.size() - 2) & 0xff);
    body[1] = static_cast<std::uint8_t>(((body.size() - 2) >> 8) & 0xff);

    std::vector<std::uint8_t> framed{0xfe, 0x02};
    for (auto byte : body)
    {
        if (byte == 0xfe)
        {
            framed.push_back(0xfe);
        }
        framed.push_back(byte);
    }
    framed.push_back(0xfe);
    framed.push_back(0x03);
    bool ok = false;
    auto const unwrapped = unwrapDle(framed.data(), framed.size(), ok);
    CHECK(ok);
    CHECK(unwrapped == body);
    auto const message = parseTsl5(unwrapped.data(), unwrapped.size());
    CHECK(message.error.empty());
    REQUIRE(message.displays.size() == 1);
    CHECK(message.displays[0].screen == 7);
    CHECK(message.displays[0].index == 3);
    CHECK(message.displays[0].textValue == text);
    CHECK(effectiveTally(message.displays[0]) == 3);
}

TEST_CASE("tsl 3.1 datagram")
{
    std::uint8_t packet[18] = {};
    packet[0] = 4;
    packet[1] = 0x01 | 0x02;
    std::memcpy(packet + 2, "PREVIEW", 7);
    auto const message = parseTsl31(packet, sizeof(packet));
    REQUIRE(message.displays.size() == 1);
    CHECK(message.displays[0].index == 4);
    CHECK(message.displays[0].textValue == "PREVIEW");
    CHECK(effectiveTally(message.displays[0]) == 3);
}
