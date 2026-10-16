#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mv
{
// What one MXL video grain holds and how the reader turns it into a picture (1.4.4).
//
// MXL (218ddaa, FlowParser) stores an interlaced flow as fields: each grain is ONE field of
// frame_height / 2 lines, and the grain rate is twice the flow definition's grain_rate (a
// 1080i flow declared at 25/1 has 50 grains per second). Grain 2k is the first field of
// frame k in time: the top field for interlaced_tff, the bottom field for interlaced_bff.
struct VideoGrainLayout
{
    bool fields = false;       // interlaced: a grain is one field
    int rows = 0;              // lines in one grain
    std::size_t fillBytes = 0; // v210 bytes of those lines
    std::size_t keyBytes = 0;  // v210a: the 10-bit key plane after the fill (0 for v210)
};

VideoGrainLayout videoGrainLayout(int width, int height, bool interlaced, bool alpha);

// Empty when a grain of `grainSize` bytes can be read with `layout`, else the reason the input
// is unsupported ("odd_interlaced_height", "grain_too_small"). A v210a grain without its key
// plane is still readable (the fill only).
std::string checkVideoGrain(VideoGrainLayout const& layout, int width, int height, std::size_t grainSize);

// The frame lines of the field in grain `index`: 0 = top field (even lines 0, 2, …),
// 1 = bottom field (odd lines).
int fieldLineParity(std::uint64_t index, bool bottomFieldFirst);
} // namespace mv
