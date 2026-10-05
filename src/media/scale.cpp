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

// A sample position of bilerp()/bilerpBob() as two indices and the weight of the second one
// in 1/1024 steps.
struct Tap
{
    int i0 = 0;
    int i1 = 0;
    std::uint32_t w = 0;
};

Tap linearTap(float v, int size)
{
    v = std::clamp(v, 0.f, static_cast<float>(size - 1));
    int const i0 = static_cast<int>(std::floor(v));
    float const f = v - static_cast<float>(i0);
    return Tap{i0, clampIndex(i0 + 1, size), static_cast<std::uint32_t>(std::lround(f * 1024.f))};
}

Tap bobTap(float v, int size)
{
    v = std::clamp(v, 0.f, static_cast<float>(std::max(1, size - 1)));
    int i0 = clampIndex(static_cast<int>(std::floor(v)) & ~1, size);
    int i1 = clampIndex(i0 + 2, size);
    if ((i1 & 1) != 0)
    {
        i1 = clampIndex(i1 - 1, size);
    }
    float const f = std::clamp((v - static_cast<float>(i0)) * 0.5f, 0.f, 1.f);
    return Tap{i0, i1, static_cast<std::uint32_t>(std::lround(f * 1024.f))};
}

std::uint16_t mix(std::uint16_t const* line0, std::uint16_t const* line1, Tap const& x, std::uint32_t wy)
{
    std::uint32_t const top = line0[x.i0] * (1024u - x.w) + line0[x.i1] * x.w;
    std::uint32_t const bottom = line1[x.i0] * (1024u - x.w) + line1[x.i1] * x.w;
    std::uint32_t const v = (top * (1024u - wy) + bottom * wy + (1u << 19)) >> 20;
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(v, 1023u));
}

// Unpacked source lines; four kept, so the two lines of the next output line are often there.
class LineCache
{
public:
    explicit LineCache(V210View const& src)
        : src_(src)
    {
        for (auto& slot : slots_)
        {
            slot.y.resize(static_cast<std::size_t>(src.width));
            slot.cb.resize(static_cast<std::size_t>(src.width / 2 + 1));
            slot.cr.resize(slot.cb.size());
        }
    }

    struct Line
    {
        int row = -1;
        std::uint64_t used = 0;
        std::vector<std::uint16_t> y, cb, cr;
    };

    Line const& get(int row)
    {
        Line* victim = &slots_[0];
        for (auto& slot : slots_)
        {
            if (slot.row == row)
            {
                slot.used = ++clock_;
                return slot;
            }
            if (slot.used < victim->used)
            {
                victim = &slot;
            }
        }
        unpackV210Line(src_.data + static_cast<std::size_t>(row) * static_cast<std::size_t>(src_.rowBytes), src_.width, victim->y.data(), victim->cb.data(),
            victim->cr.data());
        victim->row = row;
        victim->used = ++clock_;
        return *victim;
    }

private:
    V210View src_;
    Line slots_[4];
    std::uint64_t clock_ = 0;
};
} // namespace

void scaleV210Into(Frame422& dst, Placement const& place, V210View const& src, bool bob)
{
    if (place.dst.w <= 0 || place.dst.h <= 0 || src.width <= 0 || src.height <= 0 || src.data == nullptr)
    {
        return;
    }
    int const dcw = dst.chromaWidth();
    int const scw = src.width / 2;
    // Column positions once per placement, with scaleInto()'s float expressions.
    std::vector<Tap> luma(static_cast<std::size_t>(place.dst.w));
    std::vector<Tap> chroma(static_cast<std::size_t>(place.dst.w));
    float const csrcW = place.srcW * 0.5f;
    for (int dx = 0; dx < place.dst.w; ++dx)
    {
        float const sx = place.srcX + (static_cast<float>(dx) + 0.5f) * place.srcW / static_cast<float>(place.dst.w) - 0.5f;
        luma[static_cast<std::size_t>(dx)] = linearTap(sx, src.width);
        float const cx = place.srcX * 0.5f + (static_cast<float>(dx / 2) + 0.5f) * csrcW / static_cast<float>(std::max(1, place.dst.w / 2)) - 0.5f;
        chroma[static_cast<std::size_t>(dx)] = linearTap(cx, std::max(1, scw));
    }
    LineCache lines(src);
    for (int dy = 0; dy < place.dst.h; ++dy)
    {
        int const y = place.dst.y + dy;
        if (y < 0 || y >= dst.height)
        {
            continue;
        }
        float const sy = place.srcY + (static_cast<float>(dy) + 0.5f) * place.srcH / static_cast<float>(place.dst.h) - 0.5f;
        Tap const ty = bob ? bobTap(sy, src.height) : linearTap(sy, src.height);
        auto const& line0 = lines.get(ty.i0);
        auto const& line1 = lines.get(ty.i1);
        auto* rowY = dst.y.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dst.width);
        auto* rowCb = dst.cb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dcw);
        auto* rowCr = dst.cr.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dcw);
        for (int dx = 0; dx < place.dst.w; ++dx)
        {
            int const x = place.dst.x + dx;
            if (x < 0 || x >= dst.width)
            {
                continue;
            }
            rowY[x] = mix(line0.y.data(), line1.y.data(), luma[static_cast<std::size_t>(dx)], ty.w);
            if ((x & 1) == 0 && x / 2 < dcw)
            {
                rowCb[x / 2] = mix(line0.cb.data(), line1.cb.data(), chroma[static_cast<std::size_t>(dx)], ty.w);
                rowCr[x / 2] = mix(line0.cr.data(), line1.cr.data(), chroma[static_cast<std::size_t>(dx)], ty.w);
            }
            if (dst.hasAlpha)
            {
                dst.a[static_cast<std::size_t>(y) * static_cast<std::size_t>(dst.width) + static_cast<std::size_t>(x)] = 1023;
            }
        }
    }
}

PreparedOverlay prepareOverlay(std::uint8_t const* rgba, int width, int height, int stride)
{
    PreparedOverlay out;
    if (rgba == nullptr || stride <= 0 || width <= 0 || height <= 0)
    {
        return out;
    }
    out.width = width;
    out.height = height;
    int const cw = width / 2;
    out.y.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    out.a.assign(out.y.size(), 0);
    out.cb.assign(static_cast<std::size_t>(cw) * static_cast<std::size_t>(height), 0);
    out.cr.assign(out.cb.size(), 0);
    out.ca.assign(out.cb.size(), 0);
    out.spans.resize(static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y)
    {
        int spanStart = -1;
        for (int x = 0; x <= width; ++x)
        {
            int a = 0;
            if (x < width)
            {
                auto const* px = rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x) * 4u;
                a = px[3];
                if (a != 0)
                {
                    // blendStraightRgba()'s conversion, once per overlay instead of once per frame.
                    int const r = px[0];
                    int const g = px[1];
                    int const b = px[2];
                    double const y8 = 16.0 + (65.481 * r + 128.553 * g + 24.966 * b) / 255.0;
                    auto const i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                    out.y[i] = static_cast<std::uint16_t>(std::lround(std::clamp(y8 * 4.0, 0.0, 1023.0)));
                    out.a[i] = static_cast<std::uint8_t>(a);
                    if ((x & 1) == 0 && x / 2 < cw)
                    {
                        int r2 = r;
                        int g2 = g;
                        int b2 = b;
                        int a2 = a;
                        if (x + 1 < width)
                        {
                            auto const* nx = px + 4;
                            r2 = (r + nx[0]) / 2;
                            g2 = (g + nx[1]) / 2;
                            b2 = (b + nx[2]) / 2;
                            a2 = (a + nx[3]) / 2;
                        }
                        auto const c = static_cast<std::size_t>(y) * static_cast<std::size_t>(cw) + static_cast<std::size_t>(x / 2);
                        out.ca[c] = static_cast<std::uint8_t>(a2);
                        if (a2 != 0)
                        {
                            double const cb8 = 128.0 + (-37.797 * r2 - 74.203 * g2 + 112.0 * b2) / 255.0;
                            double const cr8 = 128.0 + (112.0 * r2 - 93.786 * g2 - 18.214 * b2) / 255.0;
                            out.cb[c] = static_cast<std::uint16_t>(std::lround(std::clamp(cb8 * 4.0, 0.0, 1023.0)));
                            out.cr[c] = static_cast<std::uint16_t>(std::lround(std::clamp(cr8 * 4.0, 0.0, 1023.0)));
                        }
                    }
                }
            }
            if (a != 0 && spanStart < 0)
            {
                spanStart = x;
            }
            else if (a == 0 && spanStart >= 0)
            {
                out.spans[static_cast<std::size_t>(y)].emplace_back(spanStart, x);
                spanStart = -1;
            }
        }
    }
    return out;
}

void blendOverlay(Frame422& canvas, PreparedOverlay const& overlay)
{
    if (overlay.width != canvas.width || overlay.height != canvas.height)
    {
        return;
    }
    int const cw = canvas.chromaWidth();
    for (int y = 0; y < canvas.height; ++y)
    {
        auto const rowOffset = static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width);
        auto const chromaOffset = static_cast<std::size_t>(y) * static_cast<std::size_t>(cw);
        for (auto const& [x0, x1] : overlay.spans[static_cast<std::size_t>(y)])
        {
            for (int x = x0; x < x1; ++x)
            {
                auto const i = rowOffset + static_cast<std::size_t>(x);
                int const a = overlay.a[i];
                auto& dstY = canvas.y[i];
                dstY = static_cast<std::uint16_t>((overlay.y[i] * a + dstY * (255 - a) + 127) / 255);
                if ((x & 1) == 0 && x / 2 < cw)
                {
                    auto const c = chromaOffset + static_cast<std::size_t>(x / 2);
                    int const a2 = overlay.ca[c];
                    if (a2 != 0)
                    {
                        canvas.cb[c] = static_cast<std::uint16_t>((overlay.cb[c] * a2 + canvas.cb[c] * (255 - a2) + 127) / 255);
                        canvas.cr[c] = static_cast<std::uint16_t>((overlay.cr[c] * a2 + canvas.cr[c] * (255 - a2) + 127) / 255);
                    }
                }
            }
        }
    }
}

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
