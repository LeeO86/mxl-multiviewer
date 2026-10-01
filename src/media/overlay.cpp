#include "media/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"
#include "font8x8_basic.h"
#pragma GCC diagnostic pop

namespace mv
{
namespace
{
int clampByte(int v)
{
    return std::clamp(v, 0, 255);
}

void plot(Overlay& overlay, int x, int y, Rgba color)
{
    if (x < 0 || y < 0 || x >= overlay.width || y >= overlay.height || color.a == 0)
    {
        return;
    }
    auto* px = overlay.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(overlay.width) + static_cast<std::size_t>(x)) * 4u;
    int const a = color.a;
    px[0] = static_cast<std::uint8_t>((color.r * a + px[0] * (255 - a) + 127) / 255);
    px[1] = static_cast<std::uint8_t>((color.g * a + px[1] * (255 - a) + 127) / 255);
    px[2] = static_cast<std::uint8_t>((color.b * a + px[2] * (255 - a) + 127) / 255);
    px[3] = static_cast<std::uint8_t>(clampByte(px[3] + a / 2));
}

Rgba tallyColor(int tally)
{
    switch (tally)
    {
    case 1:
        return {220, 32, 32, 255};
    case 2:
        return {32, 180, 64, 255};
    case 3:
        return {220, 160, 32, 255};
    default:
        return {0, 0, 0, 0};
    }
}

Rgba zoneColor(double dbfs, double green, double amber)
{
    if (dbfs >= amber)
    {
        return {220, 40, 40, 255};
    }
    if (dbfs >= green)
    {
        return {220, 170, 40, 255};
    }
    return {40, 190, 70, 255};
}

void drawAspect(Overlay& overlay, PixelRect const& rect, std::string const& marker)
{
    double target = 16.0 / 9.0;
    if (marker == "4:3")
    {
        target = 4.0 / 3.0;
    }
    else if (marker == "1:1")
    {
        target = 1;
    }
    else if (marker == "9:16")
    {
        target = 9.0 / 16.0;
    }
    double const have = static_cast<double>(rect.w) / std::max(1, rect.h);
    int w = rect.w;
    int h = rect.h;
    if (have > target)
    {
        w = std::max(2, static_cast<int>(std::lround(rect.h * target)));
    }
    else
    {
        h = std::max(2, static_cast<int>(std::lround(rect.w / target)));
    }
    int const x = rect.x + (rect.w - w) / 2;
    int const y = rect.y + (rect.h - h) / 2;
    overlay.strokeRect(x, y, w, h, 1, {255, 255, 255, 160});
}
} // namespace

Rgba parseHexColor(std::string const& text, Rgba fallback)
{
    if (text.size() < 4 || text[0] != '#')
    {
        return fallback;
    }
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    };
    auto byte = [&](char hi, char lo) -> int {
        int const h = nib(hi);
        int const l = nib(lo);
        if (h < 0 || l < 0)
        {
            return -1;
        }
        return (h << 4) | l;
    };
    if (text.size() == 7 || text.size() == 9)
    {
        int const r = byte(text[1], text[2]);
        int const g = byte(text[3], text[4]);
        int const b = byte(text[5], text[6]);
        int a = 255;
        if (text.size() == 9)
        {
            a = byte(text[7], text[8]);
        }
        if (r < 0 || g < 0 || b < 0 || a < 0)
        {
            return fallback;
        }
        return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a)};
    }
    return fallback;
}

void Overlay::resize(int w, int h)
{
    width = w;
    height = h;
    rgba.assign(static_cast<std::size_t>(w * h * 4), 0);
}

void Overlay::clear()
{
    std::fill(rgba.begin(), rgba.end(), 0);
}

void Overlay::fillRect(int x, int y, int w, int h, Rgba color)
{
    for (int row = y; row < y + h; ++row)
    {
        for (int col = x; col < x + w; ++col)
        {
            plot(*this, col, row, color);
        }
    }
}

void Overlay::strokeRect(int x, int y, int w, int h, int thickness, Rgba color)
{
    fillRect(x, y, w, thickness, color);
    fillRect(x, y + h - thickness, w, thickness, color);
    fillRect(x, y, thickness, h, color);
    fillRect(x + w - thickness, y, thickness, h, color);
}

void Overlay::text(int x, int y, std::string const& value, int pixelSize, Rgba color)
{
    int const scale = std::max(1, pixelSize / 8);
    int pen = x;
    for (unsigned char ch : value)
    {
        if (ch > 127)
        {
            ch = '?';
        }
        for (int row = 0; row < 8; ++row)
        {
            unsigned char const bits = static_cast<unsigned char>(font8x8_basic[ch][row]);
            for (int col = 0; col < 8; ++col)
            {
                if ((bits & (1u << col)) == 0)
                {
                    continue;
                }
                for (int sy = 0; sy < scale; ++sy)
                {
                    for (int sx = 0; sx < scale; ++sx)
                    {
                        plot(*this, pen + col * scale + sx, y + row * scale + sy, color);
                    }
                }
            }
        }
        pen += 8 * scale;
    }
}

void Overlay::line(int x0, int y0, int x1, int y1, Rgba color)
{
    int const dx = std::abs(x1 - x0);
    int const dy = -std::abs(y1 - y0);
    int const sx = x0 < x1 ? 1 : -1;
    int const sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;)
    {
        plot(*this, x0, y0, color);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }
        int const e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void renderOverlay(Overlay& overlay, std::vector<OverlayTile> const& tiles)
{
    int const thickness = std::max(2, overlay.height / 135);
    for (auto const& tile : tiles)
    {
        if (tile.tallyBorder)
        {
            auto const color = tallyColor(tile.tally);
            if (color.a != 0)
            {
                overlay.strokeRect(tile.rect.x, tile.rect.y, tile.rect.w, tile.rect.h, thickness, color);
            }
        }
        if (tile.safeArea)
        {
            int const ix = tile.rect.w / 20;
            int const iy = tile.rect.h / 20;
            overlay.strokeRect(tile.rect.x + ix, tile.rect.y + iy, tile.rect.w - 2 * ix, tile.rect.h - 2 * iy, 1, {255, 255, 255, 140});
            int const tx = tile.rect.w / 10;
            int const ty = tile.rect.h / 10;
            overlay.strokeRect(tile.rect.x + tx, tile.rect.y + ty, tile.rect.w - 2 * tx, tile.rect.h - 2 * ty, 1, {255, 255, 0, 140});
        }
        if (tile.centre)
        {
            int const cx = tile.rect.x + tile.rect.w / 2;
            int const cy = tile.rect.y + tile.rect.h / 2;
            overlay.line(cx - 8, cy, cx + 8, cy, {255, 255, 255, 180});
            overlay.line(cx, cy - 8, cx, cy + 8, {255, 255, 255, 180});
        }
        for (auto const& marker : tile.aspectMarkers)
        {
            drawAspect(overlay, tile.rect, marker);
        }
        if (tile.umd && !tile.umdText.empty())
        {
            int const band = std::max(tile.umdFont + 8, 16);
            int y = tile.rect.y;
            if (tile.umdPosition == UmdPosition::BottomInside)
            {
                y = tile.rect.y + tile.rect.h - band;
            }
            else if (tile.umdPosition == UmdPosition::BottomOutside)
            {
                y = tile.rect.y + tile.rect.h;
            }
            else if (tile.umdPosition == UmdPosition::TopOutside)
            {
                y = tile.rect.y - band;
            }
            overlay.fillRect(tile.rect.x, y, tile.rect.w, band, tile.umdBg);
            overlay.text(tile.rect.x + 4, y + 2, tile.umdText, tile.umdFont, tile.umdFg);
            if (tile.tallyLamp)
            {
                auto const color = tallyColor(tile.tally);
                if (color.a != 0)
                {
                    int const lamp = std::max(6, band / 2);
                    overlay.fillRect(tile.rect.x + 2, y + 2, lamp, lamp, color);
                    overlay.fillRect(tile.rect.x + tile.rect.w - lamp - 2, y + 2, lamp, lamp, color);
                }
            }
        }
        if (!tile.formatText.empty())
        {
            overlay.text(tile.rect.x + 4, tile.rect.y + 4, tile.formatText, 16, {255, 255, 255, 220});
        }
        if (!tile.latencyText.empty())
        {
            overlay.text(tile.rect.x + 4, tile.rect.y + 20, tile.latencyText, 16, {180, 220, 255, 220});
        }
        if (!tile.badge.empty())
        {
            overlay.fillRect(tile.rect.x + 4, tile.rect.y + tile.rect.h / 2 - 10, std::min(tile.rect.w - 8, 160), 20, {160, 24, 24, 220});
            overlay.text(tile.rect.x + 8, tile.rect.y + tile.rect.h / 2 - 8, tile.badge, 16, {255, 255, 255, 255});
        }
        if (!tile.labelText.empty())
        {
            overlay.text(tile.rect.x + 8, tile.rect.y + tile.rect.h / 2, tile.labelText, std::max(16, tile.umdFont), {255, 255, 255, 255});
        }
        if (tile.clock)
        {
            if (tile.analogue)
            {
                int const cx = tile.rect.x + tile.rect.w / 2;
                int const cy = tile.rect.y + tile.rect.h / 2;
                int const radius = std::max(8, std::min(tile.rect.w, tile.rect.h) / 2 - 4);
                auto polar = [&](double degrees, int length) {
                    double const rad = (degrees - 90.0) * 3.141592653589793 / 180.0;
                    return std::pair<int, int>{cx + static_cast<int>(std::lround(std::cos(rad) * length)), cy + static_cast<int>(std::lround(std::sin(rad) * length))};
                };
                auto hand = [&](double degrees, int length, Rgba color) {
                    auto const tip = polar(degrees, length);
                    overlay.line(cx, cy, tip.first, tip.second, color);
                };
                for (int hour = 0; hour < 12; ++hour)
                {
                    int const outer = radius;
                    int const inner = radius - std::max(4, radius / 8);
                    auto const a = polar(hour * 30.0, inner);
                    auto const b = polar(hour * 30.0, outer);
                    overlay.line(a.first, a.second, b.first, b.second, {255, 255, 255, 220});
                }
                double const minutes = tile.clockMinute + tile.clockSecond / 60.0;
                double const hours = (tile.clockHour % 12) + minutes / 60.0;
                hand(hours * 30.0, radius / 2, {255, 255, 255, 255});
                hand(minutes * 6.0, radius * 3 / 4, {255, 255, 255, 255});
                hand(tile.clockSecond * 6.0, radius - 4, {255, 64, 64, 255});
            }
            else
            {
                overlay.text(tile.rect.x + 8, tile.rect.y + tile.rect.h / 2 - 8, tile.clockText, std::max(16, tile.umdFont), {255, 255, 255, 255});
            }
            if (!tile.timecodeText.empty())
            {
                overlay.text(tile.rect.x + 8, tile.rect.y + tile.rect.h - 20, tile.timecodeText, 16, {255, 220, 120, 255});
            }
        }
        if (tile.bars && tile.barChannels > 0)
        {
            int const channels = std::clamp(tile.barChannels, 1, 16);
            int const gap = 2;
            int const barW = std::max(4, std::min(14, tile.rect.w / (channels * 3)));
            int const total = channels * barW + (channels - 1) * gap;
            int x = tile.rect.x + tile.rect.w - total - 6;
            if (tile.barsPosition == BarsPosition::Left)
            {
                x = tile.rect.x + 6;
            }
            else if (tile.barsPosition == BarsPosition::Overlay)
            {
                x = tile.rect.x + (tile.rect.w - total) / 2;
            }
            int const top = tile.rect.y + 8;
            int const height = std::max(8, tile.rect.h - 16);
            for (int c = 0; c < channels; ++c)
            {
                double const dbfs = std::clamp(tile.ppmDbfs[c], -60.0, 0.0);
                int const filled = static_cast<int>(std::lround((dbfs + 60.0) / 60.0 * height));
                overlay.fillRect(x, top, barW, height, {0, 0, 0, 120});
                auto const color = zoneColor(dbfs, tile.zoneGreen, tile.zoneAmber);
                overlay.fillRect(x, top + height - filled, barW, filled, color);
                if (tile.showRms)
                {
                    double const rms = std::clamp(tile.rmsDbfs[c], -60.0, 0.0);
                    int const mark = top + height - static_cast<int>(std::lround((rms + 60.0) / 60.0 * height));
                    overlay.fillRect(x, std::clamp(mark, top, top + height - 2), barW, 2, {255, 255, 255, 230});
                }
                if (tile.clip[c])
                {
                    overlay.fillRect(x, top, barW, 3, {255, 0, 0, 255});
                }
                x += barW + gap;
            }
        }
    }
}

bool overlayUsesBlend2d()
{
#ifdef MV_WITH_BLEND2D
    return true;
#else
    return false;
#endif
}
} // namespace mv
