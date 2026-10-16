#include "media/interlace.hpp"

#include "media/frame.hpp"

#include <algorithm>

namespace mv
{
VideoGrainLayout videoGrainLayout(int width, int height, bool interlaced, bool alpha)
{
    VideoGrainLayout layout;
    layout.fields = interlaced;
    layout.rows = interlaced ? height / 2 : height;
    if (width > 0 && layout.rows > 0)
    {
        layout.fillBytes = static_cast<std::size_t>(v210RowBytes(width)) * static_cast<std::size_t>(layout.rows);
        layout.keyBytes = alpha ? static_cast<std::size_t>(alpha10RowBytes(width)) * static_cast<std::size_t>(layout.rows) : 0;
    }
    return layout;
}

std::string checkVideoGrain(VideoGrainLayout const& layout, int width, int height, std::size_t grainSize)
{
    if (width < 2 || height < 1 || layout.rows < 1)
    {
        return "no_raster";
    }
    if (layout.fields && height % 2 != 0)
    {
        return "odd_interlaced_height";
    }
    if (grainSize < layout.fillBytes)
    {
        return "grain_too_small";
    }
    return {};
}

std::size_t grainsToKeep(std::uint64_t grainNs, std::uint64_t slowestHeadFrameNs, int inputOffsetGrains)
{
    if (grainNs == 0)
    {
        return 4;
    }
    std::uint64_t const spanNs = slowestHeadFrameNs * static_cast<std::uint64_t>(std::max(0, inputOffsetGrains) + 2);
    return static_cast<std::size_t>(std::clamp<std::uint64_t>((spanNs + grainNs - 1) / grainNs + 2, 4, 16));
}

int fieldLineParity(std::uint64_t index, bool bottomFieldFirst)
{
    return static_cast<int>(index & 1u) ^ (bottomFieldFirst ? 1 : 0);
}
} // namespace mv
