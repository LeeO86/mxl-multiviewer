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

TEST_CASE("tsl 5.0 keeps lh, rh and text tally apart and decodes utf-16 labels")
{
    // As the platform's tally calculator sends it: screen 0, UTF-16LE, display n-1 for input n.
    std::vector<std::uint8_t> body;
    put16(body, 0);
    body.push_back(0);
    body.push_back(0x01);
    put16(body, 0);
    put16(body, 2);
    put16(body, 2 | (3 << 2) | (1 << 4));
    // "Zü", U+1F3A5 as a surrogate pair, a lone high surrogate, "A".
    std::vector<int> const units{'Z', 0xFC, 0xD83C, 0xDFA5, 0xD800, 'A'};
    put16(body, static_cast<int>(units.size() * 2));
    for (int unit : units)
    {
        put16(body, unit);
    }
    body[0] = static_cast<std::uint8_t>((body.size() - 2) & 0xff);
    body[1] = static_cast<std::uint8_t>(((body.size() - 2) >> 8) & 0xff);
    auto const message = parseTsl5(body.data(), body.size());
    CHECK(message.error.empty());
    REQUIRE(message.displays.size() == 1);
    auto const& display = message.displays[0];
    CHECK(display.screen == 0);
    CHECK(display.index == 2);
    CHECK(display.lh == 1);
    CHECK(display.rh == 2);
    CHECK(display.text == 3);
    CHECK(display.textValue == "Z\xC3\xBC\xF0\x9F\x8E\xA5\xEF\xBF\xBD" "A");
    CHECK(inputForDisplay("", display.index) == 3);
    CHECK(inputForDisplay("2:7", display.index) == 7);

    // The border colour: text tally, else RH, else LH.
    TallyUpdate update;
    CHECK(effectiveTally(update) == 0);
    update.lh = 1;
    CHECK(effectiveTally(update) == 1);
    update.rh = 2;
    CHECK(effectiveTally(update) == 2);
    update.text = 3;
    CHECK(effectiveTally(update) == 3);
}

TEST_CASE("tcp tsl frames reassemble across reads")
{
    std::vector<std::uint8_t> body;
    put16(body, 4);
    body.push_back(0);
    body.push_back(0);
    put16(body, 1);
    put16(body, 2);
    put16(body, 1);
    put16(body, 1);
    body.push_back('A');
    body[0] = static_cast<std::uint8_t>((body.size() - 2) & 0xff);
    body[1] = static_cast<std::uint8_t>(((body.size() - 2) >> 8) & 0xff);
    std::vector<std::uint8_t> framed{0xfe, 0x02};
    framed.insert(framed.end(), body.begin(), body.end());
    framed.push_back(0xfe);
    framed.push_back(0x03);
    std::vector<std::uint8_t> pending(framed.begin(), framed.begin() + 5);
    auto const first = pullTslFrames(pending);
    CHECK(first.empty());
    CHECK_FALSE(pending.empty());
    pending.insert(pending.end(), framed.begin() + 5, framed.end());
    pending.insert(pending.end(), framed.begin(), framed.end());
    auto const frames = pullTslFrames(pending);
    CHECK(frames.size() == 2);
    CHECK(pending.empty());
    auto const message = parseTsl5(frames[0].data(), frames[0].size());
    CHECK(message.error.empty());
    REQUIRE(message.displays.size() == 1);
    CHECK(message.displays[0].textValue == "A");
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
    // One tally for both lamps and the text.
    CHECK(message.displays[0].lh == 3);
    CHECK(message.displays[0].rh == 3);
    CHECK(message.displays[0].text == 3);
}
