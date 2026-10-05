#include "media/frame.hpp"

#include <algorithm>
#include <cstring>

namespace mv
{
namespace
{
std::uint16_t sample10(std::uint32_t word, int shift)
{
    return static_cast<std::uint16_t>((word >> shift) & 0x3ffu);
}

void writeGroup(std::uint32_t* dst, std::uint16_t cb0, std::uint16_t y0, std::uint16_t cr0, std::uint16_t y1, std::uint16_t cb1, std::uint16_t y2,
    std::uint16_t cr1, std::uint16_t y3, std::uint16_t cb2, std::uint16_t y4, std::uint16_t cr2, std::uint16_t y5)
{
    dst[0] = (cb0 & 0x3ffu) | (static_cast<std::uint32_t>(y0 & 0x3ffu) << 10) | (static_cast<std::uint32_t>(cr0 & 0x3ffu) << 20);
    dst[1] = (y1 & 0x3ffu) | (static_cast<std::uint32_t>(cb1 & 0x3ffu) << 10) | (static_cast<std::uint32_t>(y2 & 0x3ffu) << 20);
    dst[2] = (cr1 & 0x3ffu) | (static_cast<std::uint32_t>(y3 & 0x3ffu) << 10) | (static_cast<std::uint32_t>(cb2 & 0x3ffu) << 20);
    dst[3] = (y4 & 0x3ffu) | (static_cast<std::uint32_t>(cr2 & 0x3ffu) << 10) | (static_cast<std::uint32_t>(y5 & 0x3ffu) << 20);
}
} // namespace

void Frame422::allocate(int w, int h, bool alpha)
{
    width = w;
    height = h;
    hasAlpha = alpha;
    y.assign(static_cast<std::size_t>(w * h), 0);
    cb.assign(static_cast<std::size_t>((w / 2) * h), 512);
    cr.assign(static_cast<std::size_t>((w / 2) * h), 512);
    if (alpha)
    {
        a.assign(static_cast<std::size_t>(w * h), 1023);
    }
    else
    {
        a.clear();
    }
}

void Frame422::fill(std::uint16_t yValue, std::uint16_t cbValue, std::uint16_t crValue)
{
    std::fill(y.begin(), y.end(), yValue);
    std::fill(cb.begin(), cb.end(), cbValue);
    std::fill(cr.begin(), cr.end(), crValue);
    if (hasAlpha)
    {
        std::fill(a.begin(), a.end(), static_cast<std::uint16_t>(1023));
    }
}

void unpackV210Line(std::uint8_t const* line, int width, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr)
{
    // Whole 6-pixel groups without per-pixel branches; the per-pixel loop only for a last
    // partial group.
    int const full = width / 6;
    for (int group = 0; group < full; ++group)
    {
        std::uint32_t w[4];
        std::memcpy(w, line + static_cast<std::size_t>(group) * 16u, sizeof(w));
        std::uint16_t* yy = y + group * 6;
        std::uint16_t* bb = cb + group * 3;
        std::uint16_t* rr = cr + group * 3;
        yy[0] = sample10(w[0], 10);
        yy[1] = sample10(w[1], 0);
        yy[2] = sample10(w[1], 20);
        yy[3] = sample10(w[2], 10);
        yy[4] = sample10(w[3], 0);
        yy[5] = sample10(w[3], 20);
        bb[0] = sample10(w[0], 0);
        bb[1] = sample10(w[1], 10);
        bb[2] = sample10(w[2], 20);
        rr[0] = sample10(w[0], 20);
        rr[1] = sample10(w[2], 0);
        rr[2] = sample10(w[3], 10);
    }
    int const cw = width / 2;
    int x = full * 6;
    if (x < width)
    {
        std::uint32_t words[4] = {};
        std::memcpy(words, line + static_cast<std::size_t>(full) * 16u, sizeof(words));
        std::uint16_t yv[6] = {sample10(words[0], 10), sample10(words[1], 0), sample10(words[1], 20), sample10(words[2], 10), sample10(words[3], 0),
            sample10(words[3], 20)};
        std::uint16_t cbv[3] = {sample10(words[0], 0), sample10(words[1], 10), sample10(words[2], 20)};
        std::uint16_t crv[3] = {sample10(words[0], 20), sample10(words[2], 0), sample10(words[3], 10)};
        for (int i = 0; i < 6 && x < width; ++i, ++x)
        {
            y[x] = yv[i];
            if ((x & 1) == 0 && (x / 2) < cw)
            {
                cb[x / 2] = cbv[i / 2];
                cr[x / 2] = crv[i / 2];
            }
        }
    }
}

void packV210Line(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int width, std::uint8_t* line, int rowBytes)
{
    int const full = width / 6;
    for (int group = 0; group < full; ++group)
    {
        std::uint16_t const* yy = y + group * 6;
        std::uint16_t const* bb = cb + group * 3;
        std::uint16_t const* rr = cr + group * 3;
        std::uint32_t words[4];
        writeGroup(words, bb[0], yy[0], rr[0], yy[1], bb[1], yy[2], rr[1], yy[3], bb[2], yy[4], rr[2], yy[5]);
        std::memcpy(line + static_cast<std::size_t>(group) * 16u, words, sizeof(words));
    }
    // A last partial group and the padding up to rowBytes are zero, as before.
    std::size_t const done = static_cast<std::size_t>(full) * 16u;
    if (done < static_cast<std::size_t>(rowBytes))
    {
        std::memset(line + done, 0, static_cast<std::size_t>(rowBytes) - done);
    }
    int const cw = width / 2;
    if (full * 6 < width)
    {
        std::uint16_t yv[6] = {};
        std::uint16_t cbv[3] = {};
        std::uint16_t crv[3] = {};
        for (int i = 0; i < 6; ++i)
        {
            int const x = full * 6 + i;
            if (x < width)
            {
                yv[i] = y[x];
                if ((i % 2) == 0 && x / 2 < cw)
                {
                    cbv[i / 2] = cb[x / 2];
                    crv[i / 2] = cr[x / 2];
                }
            }
        }
        std::uint32_t words[4];
        writeGroup(words, cbv[0], yv[0], crv[0], yv[1], cbv[1], yv[2], crv[1], yv[3], cbv[2], yv[4], crv[2], yv[5]);
        std::memcpy(line + done, words, sizeof(words));
    }
}

void unpackV210(std::uint8_t const* src, int srcRowBytes, Frame422& dst)
{
    if (srcRowBytes <= 0)
    {
        srcRowBytes = static_cast<int>(v210RowBytes(dst.width));
    }
    int const cw = dst.chromaWidth();
    for (int row = 0; row < dst.height; ++row)
    {
        unpackV210Line(src + static_cast<std::size_t>(row) * static_cast<std::size_t>(srcRowBytes), dst.width,
            dst.y.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(dst.width), dst.cb.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(cw),
            dst.cr.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(cw));
    }
}

void packV210(Frame422 const& src, std::uint8_t* dst, int dstRowBytes)
{
    if (dstRowBytes <= 0)
    {
        dstRowBytes = static_cast<int>(v210RowBytes(src.width));
    }
    int const cw = src.chromaWidth();
    for (int row = 0; row < src.height; ++row)
    {
        packV210Line(src.y.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(src.width),
            src.cb.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(cw), src.cr.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(cw),
            src.width, dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(dstRowBytes), dstRowBytes);
    }
}

void unpackAlpha10(std::uint8_t const* src, int srcRowBytes, Frame422& dst)
{
    if (!dst.hasAlpha)
    {
        dst.a.assign(static_cast<std::size_t>(dst.width * dst.height), 1023);
        dst.hasAlpha = true;
    }
    if (srcRowBytes <= 0)
    {
        srcRowBytes = static_cast<int>(alpha10RowBytes(dst.width));
    }
    for (int row = 0; row < dst.height; ++row)
    {
        auto const* line = src + static_cast<std::size_t>(row) * static_cast<std::size_t>(srcRowBytes);
        for (int x = 0; x < dst.width; ++x)
        {
            int const wordIndex = x / 3;
            int const shift = (x % 3) * 10;
            std::uint32_t word = 0;
            std::memcpy(&word, line + static_cast<std::size_t>(wordIndex) * 4u, 4);
            dst.a[static_cast<std::size_t>(row * dst.width + x)] = sample10(word, shift);
        }
    }
}

void packAlpha10(Frame422 const& src, std::uint8_t* dst, int dstRowBytes)
{
    if (dstRowBytes <= 0)
    {
        dstRowBytes = static_cast<int>(alpha10RowBytes(src.width));
    }
    for (int row = 0; row < src.height; ++row)
    {
        auto* line = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(dstRowBytes);
        std::memset(line, 0, static_cast<std::size_t>(dstRowBytes));
        int const words = (src.width + 2) / 3;
        for (int wordIndex = 0; wordIndex < words; ++wordIndex)
        {
            std::uint32_t word = 0;
            for (int s = 0; s < 3; ++s)
            {
                int const x = wordIndex * 3 + s;
                std::uint16_t value = 1023;
                if (x < src.width && src.hasAlpha && !src.a.empty())
                {
                    value = src.a[static_cast<std::size_t>(row * src.width + x)];
                }
                word |= static_cast<std::uint32_t>(value & 0x3ffu) << (s * 10);
            }
            std::memcpy(line + static_cast<std::size_t>(wordIndex) * 4u, &word, 4);
        }
    }
}

std::uint64_t lumaHash(Frame422 const& frame)
{
    std::uint64_t hash = 14695981039346656037ull;
    int const step = std::max(1, (frame.width * frame.height) / 4096);
    for (int i = 0; i < frame.width * frame.height; i += step)
    {
        hash ^= frame.y[static_cast<std::size_t>(i)];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint16_t v210Luma(std::uint8_t const* src, int rowBytes, int width, int index)
{
    int const row = index / width;
    int const x = index % width;
    std::uint32_t words[4];
    std::memcpy(words, src + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes) + static_cast<std::size_t>(x / 6) * 16u, sizeof(words));
    switch (x % 6)
    {
    case 0:
        return sample10(words[0], 10);
    case 1:
        return sample10(words[1], 0);
    case 2:
        return sample10(words[1], 20);
    case 3:
        return sample10(words[2], 10);
    case 4:
        return sample10(words[3], 0);
    default:
        return sample10(words[3], 20);
    }
}

void v210Chroma(std::uint8_t const* src, int rowBytes, int cx, int row, std::uint16_t& cb, std::uint16_t& cr)
{
    std::uint32_t words[4];
    std::memcpy(words, src + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes) + static_cast<std::size_t>(cx / 3) * 16u, sizeof(words));
    switch (cx % 3)
    {
    case 0:
        cb = sample10(words[0], 0);
        cr = sample10(words[0], 20);
        break;
    case 1:
        cb = sample10(words[1], 10);
        cr = sample10(words[2], 0);
        break;
    default:
        cb = sample10(words[2], 20);
        cr = sample10(words[3], 10);
        break;
    }
}

void copyV210Scan(std::uint8_t const* src, std::uint8_t* dst, int rowBytes, int width, int height, int sumStep, V210Scan& scan)
{
    scan.sum = 0;
    scan.count = 0;
    scan.hash = 14695981039346656037ull;
    long long const total = static_cast<long long>(width) * height;
    long long const hashStep = std::max(1LL, total / 4096);
    long long nextSum = 0;
    long long nextHash = 0;
    auto const luma = [](std::uint8_t const* line, int x) {
        static constexpr int kWord[6] = {0, 1, 1, 2, 3, 3};
        static constexpr int kShift[6] = {10, 0, 20, 10, 0, 20};
        std::uint32_t word = 0;
        std::memcpy(&word, line + static_cast<std::size_t>(x / 6) * 16u + static_cast<std::size_t>(kWord[x % 6]) * 4u, sizeof(word));
        return sample10(word, kShift[x % 6]);
    };
    for (int row = 0; row < height; ++row)
    {
        auto* line = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes);
        std::memcpy(line, src + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes), static_cast<std::size_t>(rowBytes));
        long long const first = static_cast<long long>(row) * width;
        long long const end = first + width;
        for (; nextSum < end; nextSum += sumStep)
        {
            scan.sum += luma(line, static_cast<int>(nextSum - first));
            ++scan.count;
        }
        for (; nextHash < end; nextHash += hashStep)
        {
            scan.hash ^= luma(line, static_cast<int>(nextHash - first));
            scan.hash *= 1099511628211ull;
        }
    }
}

std::uint64_t v210LumaSum(std::uint8_t const* src, int rowBytes, int width, int height, int step, int* count)
{
    std::uint64_t sum = 0;
    int n = 0;
    int row = 0;
    int x = 0;
    for (long long i = 0; i < static_cast<long long>(width) * height; i += step)
    {
        auto const* group = src + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes) + static_cast<std::size_t>(x / 6) * 16u;
        std::uint32_t word = 0;
        static constexpr int kWord[6] = {0, 1, 1, 2, 3, 3};
        static constexpr int kShift[6] = {10, 0, 20, 10, 0, 20};
        std::memcpy(&word, group + kWord[x % 6] * 4, sizeof(word));
        sum += sample10(word, kShift[x % 6]);
        ++n;
        x += step;
        while (x >= width)
        {
            x -= width;
            ++row;
        }
    }
    if (count != nullptr)
    {
        *count = n;
    }
    return sum;
}

std::uint64_t lumaHash(std::uint8_t const* v210, int rowBytes, int width, int height)
{
    std::uint64_t hash = 14695981039346656037ull;
    int const step = std::max(1, (width * height) / 4096);
    for (int i = 0; i < width * height; i += step)
    {
        hash ^= v210Luma(v210, rowBytes, width, i);
        hash *= 1099511628211ull;
    }
    return hash;
}
} // namespace mv
