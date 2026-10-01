#include "layout/geometry.hpp"

#include <algorithm>
#include <cmath>

namespace mv
{
PixelRect rectToPixels(NormRect const& rect, int canvasWidth, int canvasHeight)
{
    int const x0 = static_cast<int>(std::lround(rect.x * canvasWidth));
    int const y0 = static_cast<int>(std::lround(rect.y * canvasHeight));
    int const x1 = static_cast<int>(std::lround((rect.x + rect.w) * canvasWidth));
    int const y1 = static_cast<int>(std::lround((rect.y + rect.h) * canvasHeight));
    PixelRect out;
    out.x = std::clamp(x0, 0, std::max(0, canvasWidth - 2));
    out.y = std::clamp(y0, 0, std::max(0, canvasHeight - 1));
    out.w = std::max(2, x1 - out.x);
    out.h = std::max(1, y1 - out.y);
    if (out.x + out.w > canvasWidth)
    {
        out.w = canvasWidth - out.x;
    }
    if (out.y + out.h > canvasHeight)
    {
        out.h = canvasHeight - out.y;
    }
    if (out.x & 1)
    {
        out.x -= 1;
        out.w += 1;
    }
    if (out.w & 1)
    {
        out.w -= 1;
    }
    if (out.w < 2)
    {
        out.w = 2;
    }
    if (out.h < 1)
    {
        out.h = 1;
    }
    return out;
}

Placement placeTile(PixelRect const& tile, int srcWidth, int srcHeight, ScaleMode mode)
{
    Placement place;
    place.srcX = 0;
    place.srcY = 0;
    place.srcW = static_cast<float>(std::max(1, srcWidth));
    place.srcH = static_cast<float>(std::max(1, srcHeight));
    if (srcWidth <= 0 || srcHeight <= 0 || tile.w <= 0 || tile.h <= 0)
    {
        place.dst = tile;
        return place;
    }
    if (mode == ScaleMode::Fit)
    {
        float const scale = std::min(static_cast<float>(tile.w) / static_cast<float>(srcWidth), static_cast<float>(tile.h) / static_cast<float>(srcHeight));
        int w = std::max(2, static_cast<int>(std::lround(static_cast<float>(srcWidth) * scale)));
        int h = std::max(1, static_cast<int>(std::lround(static_cast<float>(srcHeight) * scale)));
        if (w & 1)
        {
            --w;
        }
        if (w < 2)
        {
            w = 2;
        }
        int x = tile.x + (tile.w - w) / 2;
        int y = tile.y + (tile.h - h) / 2;
        if (x & 1)
        {
            --x;
        }
        if (x < tile.x)
        {
            x = tile.x & ~1;
        }
        place.dst = {x, y, w, h};
        return place;
    }
    float const scale = std::max(static_cast<float>(tile.w) / static_cast<float>(srcWidth), static_cast<float>(tile.h) / static_cast<float>(srcHeight));
    place.srcW = static_cast<float>(tile.w) / scale;
    place.srcH = static_cast<float>(tile.h) / scale;
    place.srcX = (static_cast<float>(srcWidth) - place.srcW) * 0.5f;
    place.srcY = (static_cast<float>(srcHeight) - place.srcH) * 0.5f;
    place.dst = tile;
    if (place.dst.x & 1)
    {
        place.dst.x -= 1;
        place.dst.w += 1;
    }
    if (place.dst.w & 1)
    {
        place.dst.w -= 1;
    }
    return place;
}
} // namespace mv
