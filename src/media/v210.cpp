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

void unpackV210(std::uint8_t const* src, int srcRowBytes, Frame422& dst)
{
    if (srcRowBytes <= 0)
    {
        srcRowBytes = static_cast<int>(v210RowBytes(dst.width));
    }
    int const cw = dst.chromaWidth();
    for (int row = 0; row < dst.height; ++row)
    {
        auto const* line = src + static_cast<std::size_t>(row) * static_cast<std::size_t>(srcRowBytes);
        int x = 0;
        int const groups = (dst.width + 5) / 6;
        for (int group = 0; group < groups; ++group)
        {
            std::uint32_t words[4] = {};
            std::memcpy(words, line + static_cast<std::size_t>(group) * 16u, sizeof(words));
            std::uint16_t yv[6] = {sample10(words[0], 10), sample10(words[1], 0), sample10(words[1], 20), sample10(words[2], 10), sample10(words[3], 0),
                sample10(words[3], 20)};
            std::uint16_t cbv[3] = {sample10(words[0], 0), sample10(words[1], 10), sample10(words[2], 20)};
            std::uint16_t crv[3] = {sample10(words[0], 20), sample10(words[2], 0), sample10(words[3], 10)};
            for (int i = 0; i < 6 && x < dst.width; ++i, ++x)
            {
                dst.y[static_cast<std::size_t>(row * dst.width + x)] = yv[i];
                if ((x & 1) == 0 && (x / 2) < cw)
                {
                    int const c = i / 2;
                    dst.cb[static_cast<std::size_t>(row * cw + x / 2)] = cbv[c];
                    dst.cr[static_cast<std::size_t>(row * cw + x / 2)] = crv[c];
                }
            }
        }
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
        auto* line = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(dstRowBytes);
        std::memset(line, 0, static_cast<std::size_t>(dstRowBytes));
        int const groups = (src.width + 5) / 6;
        for (int group = 0; group < groups; ++group)
        {
            std::uint16_t yv[6] = {};
            std::uint16_t cbv[3] = {};
            std::uint16_t crv[3] = {};
            for (int i = 0; i < 6; ++i)
            {
                int const x = group * 6 + i;
                if (x < src.width)
                {
                    yv[i] = src.y[static_cast<std::size_t>(row * src.width + x)];
                    if ((i % 2) == 0)
                    {
                        int const cx = x / 2;
                        if (cx < cw)
                        {
                            cbv[i / 2] = src.cb[static_cast<std::size_t>(row * cw + cx)];
                            crv[i / 2] = src.cr[static_cast<std::size_t>(row * cw + cx)];
                        }
                    }
                }
            }
            std::uint32_t words[4];
            writeGroup(words, cbv[0], yv[0], crv[0], yv[1], cbv[1], yv[2], crv[1], yv[3], cbv[2], yv[4], crv[2], yv[5]);
            std::memcpy(line + static_cast<std::size_t>(group) * 16u, words, sizeof(words));
        }
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
} // namespace mv
