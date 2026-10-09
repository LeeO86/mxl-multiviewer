#include "media/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#ifdef MV_WITH_BLEND2D
#include <blend2d/blend2d.h>

#include "media/dejavu_sans.hpp"
#endif

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

// The peak-hold line: the zone colour, lighter, so it shows on top of a full bar.
Rgba holdColor(double dbfs, double green, double amber)
{
    if (dbfs >= amber)
    {
        return {255, 110, 110, 255};
    }
    if (dbfs >= green)
    {
        return {255, 220, 110, 255};
    }
    return {130, 245, 150, 255};
}

Rgba alarmColor(AlarmLevel level)
{
    return level == AlarmLevel::Amber ? Rgba{240, 170, 30, 255} : Rgba{230, 40, 40, 255};
}

int scaled(double value, double s)
{
    return static_cast<int>(std::lround(value * s));
}

#ifdef MV_WITH_BLEND2D
BLContext* blendContext(Overlay const& overlay)
{
    return static_cast<BLContext*>(overlay.blContext);
}

BLRgba32 blendColor(Rgba color)
{
    return BLRgba32(color.r, color.g, color.b, color.a);
}

BLFontFace const& dejavuFace()
{
    static BLFontFace face = [] {
        BLFontData data;
        BLFontFace created;
        if (data.create_from_data(mv::font::dejavuSans, mv::font::dejavuSansSize) == BL_SUCCESS)
        {
            created.create_from_data(data, 0);
        }
        return created;
    }();
    return face;
}

void copyPremultipliedBgra(BLImage const& image, Overlay& overlay)
{
    BLImageData data;
    if (image.get_data(&data) != BL_SUCCESS || data.pixel_data == nullptr || data.stride < 0)
    {
        return;
    }
    auto const* base = static_cast<unsigned char const*>(data.pixel_data);
    for (int y = 0; y < overlay.height; ++y)
    {
        auto const* src = base + static_cast<std::ptrdiff_t>(y) * data.stride;
        auto* dst = overlay.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(overlay.width) * 4u;
        for (int x = 0; x < overlay.width; ++x)
        {
            unsigned const b = src[0];
            unsigned const g = src[1];
            unsigned const r = src[2];
            unsigned const a = src[3];
            if (a == 0)
            {
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
            }
            else if (a == 255)
            {
                dst[0] = static_cast<std::uint8_t>(r);
                dst[1] = static_cast<std::uint8_t>(g);
                dst[2] = static_cast<std::uint8_t>(b);
                dst[3] = 255;
            }
            else
            {
                dst[0] = static_cast<std::uint8_t>((r * 255u + a / 2u) / a);
                dst[1] = static_cast<std::uint8_t>((g * 255u + a / 2u) / a);
                dst[2] = static_cast<std::uint8_t>((b * 255u + a / 2u) / a);
                dst[3] = static_cast<std::uint8_t>(a);
            }
            src += 4;
            dst += 4;
        }
    }
}
#endif

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

// The largest size up to `wanted` at which `text` is at most `maxWidth` wide.
int fitSize(Overlay const& overlay, std::string const& text, int wanted, int maxWidth)
{
    int size = std::max(1, wanted);
    int const width = overlay.textWidth(text, size);
    if (width > maxWidth && width > 0)
    {
        size = std::max(1, static_cast<int>(static_cast<long long>(size) * std::max(0, maxWidth) / width));
    }
    return size;
}

// `text` cut at a code point boundary and marked with an ellipsis so it fits `maxWidth`.
std::string fitText(Overlay const& overlay, std::string text, int size, int maxWidth)
{
    if (overlay.textWidth(text, size) <= maxWidth)
    {
        return text;
    }
    std::string const more = overlayUsesBlend2d() ? "\xE2\x80\xA6" : ".."; // U+2026 in UTF-8
    auto const dropLast = [](std::string& value) {
        while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xC0u) == 0x80u)
        {
            value.pop_back();
        }
        if (!value.empty())
        {
            value.pop_back();
        }
    };
    // Start near the right length, then remove one code point at a time.
    int const width = std::max(1, overlay.textWidth(text, size));
    auto const estimate = static_cast<std::size_t>(static_cast<double>(text.size()) * std::max(0, maxWidth) / width);
    while (text.size() > estimate + 1)
    {
        dropLast(text);
    }
    while (!text.empty() && overlay.textWidth(text + more, size) > maxWidth)
    {
        dropLast(text);
    }
    return text.empty() ? std::string{} : text + more;
}

// Draws `text` centred on (centreX, centreY); text() takes the top of the em box.
void centredText(Overlay& overlay, int centreX, int centreY, std::string const& text, int size, Rgba color)
{
    int const width = overlay.textWidth(text, size);
    overlay.text(centreX - width / 2, centreY - static_cast<int>(std::lround(size * 0.56)), text, size, color);
}

// PPM bars of one tile between `top` and `bottom` (the tile minus an inside UMD band),
// SPECIFICATION.md §5.7. Returns the area they cover, so captions can move out of the way.
PixelRect drawBars(Overlay& overlay, OverlayTile const& tile, int top, int bottom)
{
    double const s = std::max(0.5, overlay.height / 1080.0);
    int const channels = std::clamp(tile.barChannels, 1, 16);
    int const margin = std::max(3, scaled(6, s));
    int const gap = std::max(1, scaled(2, s));
    int const barW = std::clamp(tile.rect.w / (channels * 4), std::max(3, scaled(3, s)), std::max(4, scaled(14, s)));
    int const barsW = channels * barW + (channels - 1) * gap;
    top += margin;
    bottom -= margin;
    int const clipH = std::max(2, barW / 2);
    int const meterTop = top + clipH + gap;
    int const meterH = bottom - meterTop;
    if (meterH < 8 || barsW + 2 * margin > tile.rect.w)
    {
        return {};
    }
    // Labels of the PPM marks when the 6 dB steps leave room for them.
    int const labelSize = std::min(meterH / 10 * 85 / 100, scaled(15, s));
    int const tickW = std::max(2, scaled(4, s));
    int labelW = 0;
    if (tile.audioRouted && labelSize >= 9)
    {
        labelW = overlay.textWidth("-48", labelSize) + gap;
    }
    if (barsW + tickW + labelW + 2 * gap + 2 * margin > tile.rect.w)
    {
        labelW = 0;
    }
    int const scaleW = tile.audioRouted ? tickW + labelW : 0;
    int const panelW = barsW + scaleW + 2 * gap;
    int barsX = tile.rect.x + tile.rect.w - margin - barsW;
    if (tile.barsPosition == BarsPosition::Left)
    {
        barsX = tile.rect.x + margin;
    }
    else if (tile.barsPosition == BarsPosition::Overlay)
    {
        barsX = tile.rect.x + (tile.rect.w - barsW) / 2;
    }
    // The scale sits on the inner side of the bars: left of them, or right of left bars.
    bool const scaleRight = tile.barsPosition == BarsPosition::Left;
    int const panelX = scaleRight ? barsX - gap : barsX - gap - scaleW;
    PixelRect const panel{panelX, top - gap, panelW, bottom - top + 2 * gap};
    overlay.fillRect(panel.x, panel.y, panel.w, panel.h, {0, 0, 0, static_cast<std::uint8_t>(tile.audioRouted ? 130 : 70)});

    auto const yOf = [&](double dbfs) {
        double const value = std::clamp(dbfs, kMeterFloorDbfs, 0.0);
        return meterTop + static_cast<int>(std::lround(value / kMeterFloorDbfs * meterH));
    };
    int const meterBottom = meterTop + meterH;
    int const lineH = std::max(1, scaled(1, s));
    for (int c = 0; c < channels; ++c)
    {
        int const x = barsX + c * (barW + gap);
        auto const ch = static_cast<std::size_t>(c);
        bool const clipped = tile.audioRouted && tile.clip[ch];
        overlay.fillRect(x, top, barW, clipH, clipped ? Rgba{255, 30, 30, 255} : Rgba{90, 24, 24, static_cast<std::uint8_t>(tile.audioRouted ? 220 : 90)});
        overlay.fillRect(x, meterTop, barW, meterH, {24, 24, 24, static_cast<std::uint8_t>(tile.audioRouted ? 220 : 90)});
        if (!tile.audioRouted)
        {
            continue;
        }
        double const level = tile.ppmDbfs[ch];
        if (level > kMeterFloorDbfs)
        {
            // Colour by position on the scale: green, then amber, then red.
            int const yLevel = yOf(level);
            int const yGreen = yOf(tile.zoneGreen);
            int const yAmber = yOf(tile.zoneAmber);
            int const greenTop = std::max(yLevel, yGreen);
            overlay.fillRect(x, greenTop, barW, meterBottom - greenTop, {40, 190, 70, 255});
            if (yLevel < yGreen)
            {
                int const amberTop = std::max(yLevel, yAmber);
                overlay.fillRect(x, amberTop, barW, yGreen - amberTop, {220, 170, 40, 255});
            }
            if (yLevel < yAmber)
            {
                overlay.fillRect(x, yLevel, barW, yAmber - yLevel, {220, 40, 40, 255});
            }
        }
        double const hold = tile.holdDbfs[ch];
        if (hold > kMeterFloorDbfs)
        {
            int const holdH = std::max(2, scaled(2, s));
            overlay.fillRect(x, std::clamp(yOf(hold) - holdH / 2, meterTop, meterBottom - holdH), barW, holdH, holdColor(hold, tile.zoneGreen, tile.zoneAmber));
        }
        if (tile.showRms && tile.rmsDbfs[ch] > kMeterFloorDbfs)
        {
            int const rmsH = std::max(2, scaled(2, s));
            overlay.fillRect(x, std::clamp(yOf(tile.rmsDbfs[ch]) - rmsH / 2, meterTop, meterBottom - rmsH), barW, rmsH, {255, 255, 255, 230});
        }
    }
    if (!tile.audioRouted)
    {
        // No audio routed: a cross over the dim bars.
        int const half = std::max(3, std::min(barsW, meterH) / 2);
        int const cx = barsX + barsW / 2;
        int const cy = meterTop + meterH / 2;
        double const width = std::max(1.6, 2.0 * s);
        overlay.line(cx - half, cy - half, cx + half, cy + half, {210, 210, 210, 200}, width);
        overlay.line(cx - half, cy + half, cx + half, cy - half, {210, 210, 210, 200}, width);
        return panel;
    }
    int const tickX = scaleRight ? barsX + barsW + gap : barsX - gap - tickW;
    for (int mark : kPpmMarks)
    {
        int const y = std::min(yOf(mark), meterBottom - lineH);
        overlay.fillRect(tickX, y, tickW, lineH, {210, 210, 210, 220});
        overlay.fillRect(barsX, y, barsW, lineH, {0, 0, 0, 110});
        if (labelW > 0)
        {
            std::string const text = std::to_string(mark);
            int const width = overlay.textWidth(text, labelSize);
            int const textX = scaleRight ? tickX + tickW + gap : tickX - gap - width;
            overlay.text(textX, y - static_cast<int>(std::lround(labelSize * 0.56)), text, labelSize, {220, 220, 220, 230});
        }
    }
    return panel;
}
} // namespace

Ycbcr10 toYcbcr10(Rgba color)
{
    double const r = color.r;
    double const g = color.g;
    double const b = color.b;
    double const y8 = 16.0 + (65.481 * r + 128.553 * g + 24.966 * b) / 255.0;
    double const cb8 = 128.0 + (-37.797 * r - 74.203 * g + 112.0 * b) / 255.0;
    double const cr8 = 128.0 + (112.0 * r - 93.786 * g - 18.214 * b) / 255.0;
    auto const ten = [](double value) { return static_cast<std::uint16_t>(std::lround(std::clamp(value * 4.0, 0.0, 1023.0))); };
    return {ten(y8), ten(cb8), ten(cr8)};
}

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
#ifdef MV_WITH_BLEND2D
    if (auto* ctx = blendContext(*this))
    {
        if (w > 0 && h > 0 && color.a != 0)
        {
            ctx->fill_rect(static_cast<double>(x), static_cast<double>(y), static_cast<double>(w), static_cast<double>(h), blendColor(color));
        }
        return;
    }
#endif
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
#ifdef MV_WITH_BLEND2D
    if (auto* ctx = blendContext(*this))
    {
        if (w > 0 && h > 0 && thickness > 0 && color.a != 0)
        {
            double const inset = thickness * 0.5;
            ctx->set_stroke_width(thickness);
            ctx->stroke_rect(static_cast<double>(x) + inset, static_cast<double>(y) + inset, std::max(0.0, static_cast<double>(w - thickness)),
                std::max(0.0, static_cast<double>(h - thickness)), blendColor(color));
        }
        return;
    }
#endif
    fillRect(x, y, w, thickness, color);
    fillRect(x, y + h - thickness, w, thickness, color);
    fillRect(x, y, thickness, h, color);
    fillRect(x + w - thickness, y, thickness, h, color);
}

void Overlay::text(int x, int y, std::string const& value, int pixelSize, Rgba color)
{
#ifdef MV_WITH_BLEND2D
    if (auto* ctx = blendContext(*this))
    {
        auto const& face = dejavuFace();
        if (!value.empty() && pixelSize > 0 && color.a != 0 && face.is_valid())
        {
            BLFont font;
            if (font.create_from_face(face, static_cast<float>(pixelSize)) == BL_SUCCESS)
            {
                ctx->fill_utf8_text(BLPoint(static_cast<double>(x), static_cast<double>(y) + font.metrics().ascent), font, value.c_str(), value.size(), blendColor(color));
            }
        }
        return;
    }
#endif
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

int Overlay::textWidth(std::string const& value, int pixelSize) const
{
    if (value.empty() || pixelSize <= 0)
    {
        return 0;
    }
#ifdef MV_WITH_BLEND2D
    if (blendContext(*this) != nullptr)
    {
        auto const& face = dejavuFace();
        BLFont font;
        if (face.is_valid() && font.create_from_face(face, static_cast<float>(pixelSize)) == BL_SUCCESS)
        {
            BLGlyphBuffer glyphs;
            glyphs.set_utf8_text(value.c_str(), value.size());
            BLTextMetrics metrics;
            if (font.get_text_metrics(glyphs, metrics) == BL_SUCCESS)
            {
                return static_cast<int>(std::lround(metrics.advance.x));
            }
        }
        return 0;
    }
#endif
    return static_cast<int>(value.size()) * 8 * std::max(1, pixelSize / 8);
}

void Overlay::line(int x0, int y0, int x1, int y1, Rgba color, double width)
{
#ifdef MV_WITH_BLEND2D
    if (auto* ctx = blendContext(*this))
    {
        if (color.a != 0)
        {
            ctx->set_stroke_width(width);
            ctx->set_stroke_caps(BL_STROKE_CAP_ROUND);
            ctx->stroke_line(static_cast<double>(x0) + 0.5, static_cast<double>(y0) + 0.5, static_cast<double>(x1) + 0.5, static_cast<double>(y1) + 0.5, blendColor(color));
        }
        return;
    }
#endif
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

namespace
{
// Analogue face or digital time, sized to the tile, with the optional timecode under it.
void drawClock(Overlay& overlay, OverlayTile const& tile, double s)
{
    PixelRect const& r = tile.rect;
    int const centreX = r.x + r.w / 2;
    int const maxW = r.w * 86 / 100;
    Rgba const white{255, 255, 255, 255};
    Rgba const timecode{255, 220, 120, 255};
    if (tile.analogue)
    {
        int const tcSize = tile.timecodeText.empty() ? 0 : fitSize(overlay, tile.timecodeText, std::max(10, r.h * 75 / 1000), maxW);
        int const tcBand = tcSize * 16 / 10;
        int const margin = std::max(4, scaled(4, s));
        int const radius = std::max(8, std::min(r.w, r.h - tcBand) / 2 - margin);
        int const cx = centreX;
        int const cy = r.y + (r.h - tcBand) / 2;
        auto polar = [&](double degrees, int length) {
            double const rad = (degrees - 90.0) * 3.141592653589793 / 180.0;
            return std::pair<int, int>{cx + static_cast<int>(std::lround(std::cos(rad) * length)), cy + static_cast<int>(std::lround(std::sin(rad) * length))};
        };
        auto hand = [&](double degrees, int length, Rgba color, double width) {
            auto const tip = polar(degrees, length);
            overlay.line(cx, cy, tip.first, tip.second, color, width);
        };
        double const tickW = std::max(1.6, radius / 60.0);
        for (int minute = 0; minute < 60; ++minute)
        {
            bool const hour = minute % 5 == 0;
            // Minute ticks only on a face large enough to tell them apart.
            if (!hour && radius < 120)
            {
                continue;
            }
            bool const quarter = minute % 15 == 0;
            int const length = hour ? std::max(4, radius / (quarter ? 6 : 9)) : std::max(2, radius / 25);
            auto const a = polar(minute * 6.0, radius - length);
            auto const b = polar(minute * 6.0, radius);
            overlay.line(a.first, a.second, b.first, b.second, {255, 255, 255, static_cast<std::uint8_t>(hour ? 230 : 150)},
                hour ? tickW * (quarter ? 1.6 : 1.0) : std::max(1.0, tickW / 2));
        }
        double const minutes = tile.clockMinute + tile.clockSecond / 60.0;
        double const hours = (tile.clockHour % 12) + minutes / 60.0;
        hand(hours * 30.0, radius / 2, white, std::max(2.0, radius / 22.0));
        hand(minutes * 6.0, radius * 3 / 4, white, std::max(1.8, radius / 32.0));
        hand(tile.clockSecond * 6.0, radius - 4, {255, 64, 64, 255}, std::max(1.2, radius / 90.0));
        if (tcSize > 0)
        {
            centredText(overlay, centreX, cy + radius + tcBand / 2, tile.timecodeText, tcSize, timecode);
        }
        return;
    }
    bool const withTimecode = !tile.timecodeText.empty();
    int const size = fitSize(overlay, tile.clockText, std::max(10, r.h * (withTimecode ? 34 : 42) / 100), maxW);
    int const tcSize = withTimecode ? fitSize(overlay, tile.timecodeText, std::max(8, size * 45 / 100), maxW) : 0;
    int const block = size + tcSize * 13 / 10;
    int const top = r.y + (r.h - block) / 2;
    centredText(overlay, centreX, top + size / 2, tile.clockText, size, white);
    if (withTimecode)
    {
        centredText(overlay, centreX, top + size + tcSize * 13 / 20, tile.timecodeText, tcSize, timecode);
    }
}
} // namespace

void renderOverlay(Overlay& overlay, std::vector<OverlayTile> const& tiles)
{
#ifdef MV_WITH_BLEND2D
    BLImage image;
    BLContext ctx;
    if (overlay.width > 0 && overlay.height > 0 && image.create(overlay.width, overlay.height, BL_FORMAT_PRGB32) == BL_SUCCESS && ctx.begin(image) == BL_SUCCESS)
    {
        // A new image is uninitialized. Clear before drawing so untouched pixels stay transparent.
        ctx.clear_all();
        // ctx outlives every draw in this function and is cleared before return.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
        overlay.blContext = &ctx;
#pragma GCC diagnostic pop
    }
#endif
    double const s = std::max(0.5, overlay.height / 1080.0);
    int const thickness = std::max(2, overlay.height / 135);
    for (auto const& tile : tiles)
    {
        PixelRect const& r = tile.rect;
        if (tile.tallyBorder)
        {
            auto const color = tallyColor(tile.tally);
            if (color.a != 0)
            {
                overlay.strokeRect(r.x, r.y, r.w, r.h, thickness, color);
            }
        }
        // The alarm border sits inside the tally border, so both stay visible (§6.3).
        if (tile.alarm != AlarmLevel::None)
        {
            overlay.strokeRect(r.x + thickness, r.y + thickness, r.w - 2 * thickness, r.h - 2 * thickness, std::max(2, thickness / 2), alarmColor(tile.alarm));
        }
        if (tile.safeArea)
        {
            int const ix = r.w / 20;
            int const iy = r.h / 20;
            overlay.strokeRect(r.x + ix, r.y + iy, r.w - 2 * ix, r.h - 2 * iy, 1, {255, 255, 255, 140});
            int const tx = r.w / 10;
            int const ty = r.h / 10;
            overlay.strokeRect(r.x + tx, r.y + ty, r.w - 2 * tx, r.h - 2 * ty, 1, {255, 255, 0, 140});
        }
        if (tile.centre)
        {
            int const cx = r.x + r.w / 2;
            int const cy = r.y + r.h / 2;
            overlay.line(cx - 8, cy, cx + 8, cy, {255, 255, 255, 180});
            overlay.line(cx, cy - 8, cx, cy + 8, {255, 255, 255, 180});
        }
        for (auto const& marker : tile.aspectMarkers)
        {
            drawAspect(overlay, r, marker);
        }
        // The UMD band is drawn last; slates and bars keep clear of it when it is inside.
        bool const umd = tile.umd && !tile.umdText.empty();
        int const band = std::max(tile.umdFont + 8, 16);
        int umdY = r.y;
        if (tile.umdPosition == UmdPosition::BottomInside)
        {
            umdY = r.y + r.h - band;
        }
        else if (tile.umdPosition == UmdPosition::BottomOutside)
        {
            umdY = r.y + r.h;
        }
        else if (tile.umdPosition == UmdPosition::TopOutside)
        {
            umdY = r.y - band;
        }
        int contentTop = r.y;
        int contentBottom = r.y + r.h;
        if (umd && tile.umdPosition == UmdPosition::BottomInside)
        {
            contentBottom = umdY;
        }
        else if (umd && tile.umdPosition == UmdPosition::TopInside)
        {
            contentTop = umdY + band;
        }
        int const centreX = r.x + r.w / 2;
        if (!tile.slate.empty())
        {
            int const areaH = std::max(1, contentBottom - contentTop);
            int const size = fitSize(overlay, tile.slate, std::max(10, areaH * 16 / 100), r.w * 8 / 10);
            int const cy = contentTop + areaH * 45 / 100;
            Rgba color{160, 160, 160, 255};
            if (tile.slate == "NO SIGNAL")
            {
                color = {235, 235, 235, 255};
            }
            else if (tile.slate == "WAITING")
            {
                color = {235, 190, 80, 255};
            }
            centredText(overlay, centreX, cy, tile.slate, size, color);
            if (!tile.slateLabel.empty())
            {
                int const small = fitSize(overlay, tile.slateLabel, std::max(8, size * 55 / 100), r.w * 8 / 10);
                centredText(overlay, centreX, cy + size * 95 / 100, tile.slateLabel, small, {170, 170, 170, 255});
            }
        }
        if (!tile.labelText.empty())
        {
            int const size = fitSize(overlay, tile.labelText, std::max(10, r.h * 4 / 10), r.w * 9 / 10);
            centredText(overlay, centreX, r.y + r.h / 2, tile.labelText, size, {255, 255, 255, 255});
        }
        if (tile.clock)
        {
            drawClock(overlay, tile, s);
        }
        PixelRect bars{};
        if (tile.bars && tile.barChannels > 0)
        {
            bars = drawBars(overlay, tile, contentTop, contentBottom);
        }
        if (umd)
        {
            // tally_text: the text tally fills the band and the text turns black, readable on red, green, and amber.
            bool const textBg = tile.textTallyBg && tile.textTally != 0;
            overlay.fillRect(r.x, umdY, r.w, band, textBg ? tallyColor(tile.textTally) : tile.umdBg);
            int const lamp = std::max(6, band / 2);
            int const textX = r.x + (tile.tallyLamp ? lamp + 8 : 8);
            int const room = r.w - (textX - r.x) - (tile.tallyLamp ? lamp + 8 : 8);
            // Text wider than the room is cut and ends with an ellipsis; the rest is aligned in the room.
            auto const text = fitText(overlay, tile.umdText, tile.umdFont, room);
            int const free = tile.umdAlign == UmdAlign::Left ? 0 : std::max(0, room - overlay.textWidth(text, tile.umdFont));
            overlay.text(textX + (tile.umdAlign == UmdAlign::Centre ? free / 2 : free), umdY + (band - tile.umdFont) / 2, text, tile.umdFont,
                textBg ? Rgba{0, 0, 0, 255} : tile.umdFg);
            if (tile.tallyLamp)
            {
                // An off lamp is not drawn.
                int const lampY = umdY + (band - lamp) / 2;
                auto const left = tallyColor(tile.lhTally);
                auto const right = tallyColor(tile.rhTally);
                if (left.a != 0)
                {
                    overlay.fillRect(r.x + 4, lampY, lamp, lamp, left);
                }
                if (right.a != 0)
                {
                    overlay.fillRect(r.x + r.w - lamp - 4, lampY, lamp, lamp, right);
                }
            }
        }
        int const caption = std::max(8, overlay.height * 16 / 1080);
        int const captionX = tile.barsPosition == BarsPosition::Left && bars.w > 0 ? bars.x + bars.w + 4 : r.x + 4;
        if (!tile.formatText.empty())
        {
            overlay.text(captionX, contentTop + 4, tile.formatText, caption, {255, 255, 255, 220});
        }
        if (!tile.latencyText.empty())
        {
            overlay.text(captionX, contentTop + caption + 6, tile.latencyText, caption, {180, 220, 255, 220});
        }
        if (!tile.badge.empty())
        {
            int const size = std::max(10, scaled(16, s));
            int const padX = std::max(4, scaled(6, s));
            int const height = size + std::max(4, scaled(6, s));
            int const width = std::min(r.w - 2 * thickness, overlay.textWidth(tile.badge, size) + 2 * padX);
            int const x = r.x + (r.w - width) / 2;
            int const y = contentTop + thickness * 2;
            bool const amber = tile.alarm == AlarmLevel::Amber;
            overlay.fillRect(x, y, width, height, amber ? Rgba{240, 170, 30, 235} : Rgba{200, 30, 30, 235});
            centredText(overlay, x + width / 2, y + height / 2, tile.badge, size, amber ? Rgba{24, 18, 6, 255} : Rgba{255, 255, 255, 255});
        }
    }
#ifdef MV_WITH_BLEND2D
    if (overlay.blContext != nullptr)
    {
        ctx.end();
        overlay.blContext = nullptr;
        copyPremultipliedBgra(image, overlay);
    }
#endif
}

bool overlayUsesBlend2d()
{
#ifdef MV_WITH_BLEND2D
    return true;
#else
    return false;
#endif
}

std::vector<PixelRect> overlayChanges(std::vector<std::uint8_t> const& before, std::vector<std::uint8_t> const& after, int width, int height)
{
    constexpr int kBlockW = 64;
    constexpr int kBlockH = 16;
    std::vector<PixelRect> changes;
    auto const bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (width <= 0 || height <= 0 || before.size() != bytes || after.size() != bytes)
    {
        changes.push_back({0, 0, std::max(width, 0), std::max(height, 0)});
        return changes;
    }
    std::size_t const stride = static_cast<std::size_t>(width) * 4u;
    for (int by = 0; by < height; by += kBlockH)
    {
        int const h = std::min(kBlockH, height - by);
        int runStart = -1;
        // One step past the last block closes a run that reaches the right edge.
        for (int bx = 0;; bx += kBlockW)
        {
            bool changed = false;
            if (bx < width)
            {
                int const w = std::min(kBlockW, width - bx);
                for (int row = by; row < by + h && !changed; ++row)
                {
                    std::size_t const offset = static_cast<std::size_t>(row) * stride + static_cast<std::size_t>(bx) * 4u;
                    changed = std::memcmp(before.data() + offset, after.data() + offset, static_cast<std::size_t>(w) * 4u) != 0;
                }
            }
            if (changed && runStart < 0)
            {
                runStart = bx;
            }
            else if (!changed && runStart >= 0)
            {
                changes.push_back({runStart, by, std::min(bx, width) - runStart, h});
                runStart = -1;
            }
            if (bx >= width)
            {
                break;
            }
        }
    }
    return changes;
}
} // namespace mv
