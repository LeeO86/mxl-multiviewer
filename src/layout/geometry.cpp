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

BarMetrics barMetrics(int tileWidth, int channels, int canvasHeight)
{
    double const s = std::max(0.5, canvasHeight / 1080.0);
    auto const scaled = [s](double value) { return static_cast<int>(std::lround(value * s)); };
    channels = std::clamp(channels, 1, 16);
    BarMetrics m;
    m.margin = std::max(3, scaled(6));
    m.gap = std::max(1, scaled(2));
    m.barW = std::clamp(tileWidth / (channels * 4), std::max(3, scaled(3)), std::max(4, scaled(14)));
    m.barsW = channels * m.barW + (channels - 1) * m.gap;
    m.tickW = std::max(2, scaled(4));
    m.labelReserve = scaled(28);
    return m;
}

int barsStripWidth(int tileWidth, int channels, bool scale, int canvasHeight)
{
    auto const m = barMetrics(tileWidth, channels, canvasHeight);
    int const width = 2 * m.margin + m.barsW + m.gap + (scale ? m.tickW + m.labelReserve : 0);
    int const even = (width + 1) & ~1;
    return even * 2 <= tileWidth ? even : 0;
}

int umdFontPx(int umdFont, int canvasHeight)
{
    return std::max(8, umdFont * canvasHeight / 1080);
}

int umdBandHeight(int fontPx, int tileHeight)
{
    return std::clamp(std::max(fontPx + 8, 16), 0, std::max(0, tileHeight));
}

PixelRect pictureRect(PixelRect const& tile, Tile const& options, int canvasHeight)
{
    PixelRect picture = tile;
    if (options.content != TileContent::Input)
    {
        return picture;
    }
    if (options.umd && !options.umdOverlay)
    {
        // At least one line stays for the picture; the bar is drawn over it when the font fills the tile.
        int const band = std::min(umdBandHeight(umdFontPx(options.umdFont, canvasHeight), tile.h), tile.h - 1);
        picture.h = tile.h - band;
        if (options.umdPosition == UmdPosition::Top)
        {
            picture.y += band;
        }
    }
    if (options.audioBars && !options.audioBarOverlay && options.audioBarPosition != BarsPosition::Centre)
    {
        int const strip = barsStripWidth(tile.w, options.audioBarChannels, options.audioBarScale, canvasHeight);
        picture.w -= strip;
        if (options.audioBarPosition == BarsPosition::Left)
        {
            picture.x += strip;
        }
    }
    return picture;
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
