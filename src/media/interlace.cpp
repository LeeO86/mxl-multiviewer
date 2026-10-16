#include "media/interlace.hpp"

#include "media/frame.hpp"

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

int fieldLineParity(std::uint64_t index, bool bottomFieldFirst)
{
    return static_cast<int>(index & 1u) ^ (bottomFieldFirst ? 1 : 0);
}
} // namespace mv
