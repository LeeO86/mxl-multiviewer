#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mv
{
struct TallyUpdate
{
    int screen = 0;
    int index = 0;
    int rh = 0;
    int text = 0;
    int lh = 0;
    int brightness = 0;
    std::string textValue;
};

struct TslMessage
{
    std::vector<TallyUpdate> displays;
    std::string error;
};

// Unwrap a DLE/STX ... DLE/ETX frame. A buffer that is not wrapped is returned unchanged.
std::vector<std::uint8_t> unwrapDle(std::uint8_t const* data, std::size_t size, bool& ok);

// Pull every complete DLE/STX ... DLE/ETX frame out of a TCP buffer. Incomplete bytes stay in `buffer`.
// Each returned vector is the unwrapped body.
std::vector<std::vector<std::uint8_t>> pullTslFrames(std::vector<std::uint8_t>& buffer);

TslMessage parseTsl5(std::uint8_t const* body, std::size_t size);
TslMessage parseTsl31(std::uint8_t const* data, std::size_t size);

// The border colour: text tally, else RH, else LH. The lamps show LH and RH themselves.
int effectiveTally(TallyUpdate const& update);
int inputForDisplay(std::string const& map, int display);
} // namespace mv
