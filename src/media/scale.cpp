#include "media/scale.hpp"

#include <algorithm>
#include <cmath>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace mv
{
namespace
{
int clampIndex(int v, int limit)
{
    return std::clamp(v, 0, std::max(0, limit - 1));
}

std::uint16_t bilerp(std::uint16_t const* plane, int stride, int width, int height, float x, float y)
{
    x = std::clamp(x, 0.f, static_cast<float>(width - 1));
    y = std::clamp(y, 0.f, static_cast<float>(height - 1));
    int const x0 = static_cast<int>(std::floor(x));
    int const y0 = static_cast<int>(std::floor(y));
    int const x1 = clampIndex(x0 + 1, width);
    int const y1 = clampIndex(y0 + 1, height);
    float const fx = x - static_cast<float>(x0);
    float const fy = y - static_cast<float>(y0);
    float const v00 = plane[y0 * stride + x0];
    float const v10 = plane[y0 * stride + x1];
    float const v01 = plane[y1 * stride + x0];
    float const v11 = plane[y1 * stride + x1];
    float const v0 = v00 + (v10 - v00) * fx;
    float const v1 = v01 + (v11 - v01) * fx;
    return static_cast<std::uint16_t>(std::lround(std::clamp(v0 + (v1 - v0) * fy, 0.f, 1023.f)));
}

std::uint16_t bilerpBob(std::uint16_t const* plane, int stride, int width, int height, float x, float y)
{
    x = std::clamp(x, 0.f, static_cast<float>(width - 1));
    y = std::clamp(y, 0.f, static_cast<float>(std::max(1, height - 1)));
    int y0 = static_cast<int>(std::floor(y)) & ~1;
    y0 = clampIndex(y0, height);
    int y1 = clampIndex(y0 + 2, height);
    if ((y1 & 1) != 0)
    {
        y1 = clampIndex(y1 - 1, height);
    }
    float const fy = std::clamp((y - static_cast<float>(y0)) * 0.5f, 0.f, 1.f);
    int const x0 = static_cast<int>(std::floor(x));
    int const x1 = clampIndex(x0 + 1, width);
    float const fx = x - static_cast<float>(x0);
    auto sample = [&](int yy, int xx) { return static_cast<float>(plane[yy * stride + xx]); };
    float const v0 = sample(y0, x0) + (sample(y0, x1) - sample(y0, x0)) * fx;
    float const v1 = sample(y1, x0) + (sample(y1, x1) - sample(y1, x0)) * fx;
    return static_cast<std::uint16_t>(std::lround(std::clamp(v0 + (v1 - v0) * fy, 0.f, 1023.f)));
}

void clearPlane(std::uint16_t* data, std::size_t count, std::uint16_t value)
{
#if defined(__SSE2__)
    __m128i const fill = _mm_set1_epi16(static_cast<short>(value));
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8)
    {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(data + i), fill);
    }
    for (; i < count; ++i)
    {
        data[i] = value;
    }
#else
    std::fill(data, data + count, value);
#endif
}
} // namespace

void scaleInto(Frame422& dst, Placement const& place, Frame422 const& src, bool bob)
{
    if (place.dst.w <= 0 || place.dst.h <= 0 || src.width <= 0 || src.height <= 0)
    {
        return;
    }
    int const dcw = dst.chromaWidth();
    int const scw = src.chromaWidth();
    for (int dy = 0; dy < place.dst.h; ++dy)
    {
        int const y = place.dst.y + dy;
        if (y < 0 || y >= dst.height)
        {
            continue;
        }
        float const sy = place.srcY + (static_cast<float>(dy) + 0.5f) * place.srcH / static_cast<float>(place.dst.h) - 0.5f;
        for (int dx = 0; dx < place.dst.w; ++dx)
        {
            int const x = place.dst.x + dx;
            if (x < 0 || x >= dst.width)
            {
                continue;
            }
            float const sx = place.srcX + (static_cast<float>(dx) + 0.5f) * place.srcW / static_cast<float>(place.dst.w) - 0.5f;
            auto const ySample = bob ? bilerpBob(src.y.data(), src.width, src.width, src.height, sx, sy)
                                     : bilerp(src.y.data(), src.width, src.width, src.height, sx, sy);
            std::uint16_t alpha = 1023;
            if (src.hasAlpha && !src.a.empty())
            {
                alpha = bob ? bilerpBob(src.a.data(), src.width, src.width, src.height, sx, sy)
                            : bilerp(src.a.data(), src.width, src.width, src.height, sx, sy);
            }
            auto& dstY = dst.y[static_cast<std::size_t>(y * dst.width + x)];
            if (alpha >= 1023)
            {
                dstY = ySample;
            }
            else if (alpha > 0)
            {
                dstY = static_cast<std::uint16_t>((static_cast<int>(ySample) * alpha + static_cast<int>(dstY) * (1023 - alpha) + 511) / 1023);
            }
            if ((x & 1) == 0 && x / 2 < dcw)
            {
                float const csx = sx * 0.5f;
                float const csrcW = place.srcW * 0.5f;
                float const cx = place.srcX * 0.5f + (static_cast<float>(dx / 2) + 0.5f) * csrcW / static_cast<float>(std::max(1, place.dst.w / 2)) - 0.5f;
                (void)csx;
                auto pick = [&](std::vector<std::uint16_t> const& plane) {
                    return bob ? bilerpBob(plane.data(), scw, scw, src.height, cx, sy) : bilerp(plane.data(), scw, scw, src.height, cx, sy);
                };
                dst.cb[static_cast<std::size_t>(y * dcw + x / 2)] = pick(src.cb);
                dst.cr[static_cast<std::size_t>(y * dcw + x / 2)] = pick(src.cr);
            }
            if (dst.hasAlpha)
            {
                dst.a[static_cast<std::size_t>(y * dst.width + x)] = alpha;
            }
        }
    }
}

void coverFrame(Frame422& dst, Frame422 const& src)
{
    if (dst.width <= 0 || dst.height <= 0)
    {
        return;
    }
    dst.fill(64, 512, 512);
    if (src.width <= 0 || src.height <= 0)
    {
        return;
    }
    auto const place = placeTile(PixelRect{0, 0, dst.width, dst.height}, src.width, src.height, ScaleMode::Fill);
    scaleInto(dst, place, src, false);
}

void composeTiles(Frame422& canvas, std::uint16_t bgY, std::uint16_t bgCb, std::uint16_t bgCr, Frame422 const* background, int backgroundCount,
    ComposeTile const* tiles, int tileCount)
{
    (void)backgroundCount;
    if (background != nullptr && background->width == canvas.width && background->height == canvas.height)
    {
        canvas.y = background->y;
        canvas.cb = background->cb;
        canvas.cr = background->cr;
    }
    else if (background != nullptr && background->width > 0 && background->height > 0)
    {
        coverFrame(canvas, *background);
    }
    else
    {
        clearPlane(canvas.y.data(), canvas.y.size(), bgY);
        clearPlane(canvas.cb.data(), canvas.cb.size(), bgCb);
        clearPlane(canvas.cr.data(), canvas.cr.size(), bgCr);
    }
    if (canvas.hasAlpha)
    {
        std::fill(canvas.a.begin(), canvas.a.end(), static_cast<std::uint16_t>(1023));
    }
    for (int i = 0; i < tileCount; ++i)
    {
        if (tiles[i].source == nullptr)
        {
            Frame422 black;
            black.allocate(2, 2, false);
            black.fill(bgY, bgCb, bgCr);
            Placement full = tiles[i].place;
            full.srcX = 0;
            full.srcY = 0;
            full.srcW = 2;
            full.srcH = 2;
            scaleInto(canvas, full, black, false);
            continue;
        }
        scaleInto(canvas, tiles[i].place, *tiles[i].source, tiles[i].bob);
    }
}

void blendStraightRgba(Frame422& canvas, std::uint8_t const* rgba, int stride)
{
    if (rgba == nullptr || stride <= 0)
    {
        return;
    }
    int const cw = canvas.chromaWidth();
    for (int y = 0; y < canvas.height; ++y)
    {
        for (int x = 0; x < canvas.width; ++x)
        {
            auto const* px = rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x) * 4u;
            int const a = px[3];
            if (a == 0)
            {
                continue;
            }
            int const r = px[0];
            int const g = px[1];
            int const b = px[2];
            double const y8 = 16.0 + (65.481 * r + 128.553 * g + 24.966 * b) / 255.0;
            auto const y10 = static_cast<int>(std::lround(std::clamp(y8 * 4.0, 0.0, 1023.0)));
            auto& dstY = canvas.y[static_cast<std::size_t>(y * canvas.width + x)];
            dstY = static_cast<std::uint16_t>((y10 * a + dstY * (255 - a) + 127) / 255);
            if ((x & 1) == 0 && x / 2 < cw)
            {
                int r2 = r;
                int g2 = g;
                int b2 = b;
                int a2 = a;
                if (x + 1 < canvas.width)
                {
                    auto const* nx = px + 4;
                    r2 = (r + nx[0]) / 2;
                    g2 = (g + nx[1]) / 2;
                    b2 = (b + nx[2]) / 2;
                    a2 = (a + nx[3]) / 2;
                }
                if (a2 != 0)
                {
                    double const cb8 = 128.0 + (-37.797 * r2 - 74.203 * g2 + 112.0 * b2) / 255.0;
                    double const cr8 = 128.0 + (112.0 * r2 - 93.786 * g2 - 18.214 * b2) / 255.0;
                    auto const cb10 = static_cast<int>(std::lround(std::clamp(cb8 * 4.0, 0.0, 1023.0)));
                    auto const cr10 = static_cast<int>(std::lround(std::clamp(cr8 * 4.0, 0.0, 1023.0)));
                    auto& dstCb = canvas.cb[static_cast<std::size_t>(y * cw + x / 2)];
                    auto& dstCr = canvas.cr[static_cast<std::size_t>(y * cw + x / 2)];
                    dstCb = static_cast<std::uint16_t>((cb10 * a2 + dstCb * (255 - a2) + 127) / 255);
                    dstCr = static_cast<std::uint16_t>((cr10 * a2 + dstCr * (255 - a2) + 127) / 255);
                }
            }
        }
    }
}
} // namespace mv
