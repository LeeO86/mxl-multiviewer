#pragma once

#include <cstdint>

// One 2×2 block of the WebRTC preview (§8.4): planar 10-bit 4:2:2 (Y, Cb, Cr as in Frame422 and
// the CUDA canvas) scaled to 8-bit NV12 by area average. The CPU backend (scaleToNv12) and the CUDA
// kernel (cudaPreviewTile) run this same code, so both backends give the same preview.
#if defined(__CUDACC__)
#define MV_HD __host__ __device__
#else
#define MV_HD
#endif

namespace mv::nv12
{
// [begin, end) of the source samples under destination sample `d` of `dstSize`; at least one.
MV_HD inline void span(int d, int count, int srcSize, int dstSize, int& begin, int& end)
{
    begin = static_cast<int>(static_cast<long long>(d) * srcSize / dstSize);
    end = static_cast<int>(static_cast<long long>(d + count) * srcSize / dstSize);
    if (begin > srcSize - 1)
    {
        begin = srcSize - 1;
    }
    if (end <= begin)
    {
        end = begin + 1;
    }
}

// The rounded mean of `n` 10-bit samples as an 8-bit sample.
MV_HD inline std::uint8_t to8(unsigned sum, unsigned n)
{
    unsigned const v = ((sum + n / 2) / n + 2) >> 2;
    return static_cast<std::uint8_t>(v > 255 ? 255 : v);
}

// Writes luma (2×2) and one Cb/Cr pair of destination block (bx, by): dstW × dstH even.
MV_HD inline void block(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int srcW, int srcH, int dstW, int dstH, int bx, int by,
    std::uint8_t* luma, int lumaPitch, std::uint8_t* chroma, int chromaPitch)
{
    int const cw = srcW / 2;
    for (int dy = 0; dy < 2; ++dy)
    {
        int y0 = 0;
        int y1 = 0;
        span(by * 2 + dy, 1, srcH, dstH, y0, y1);
        for (int dx = 0; dx < 2; ++dx)
        {
            int x0 = 0;
            int x1 = 0;
            span(bx * 2 + dx, 1, srcW, dstW, x0, x1);
            unsigned sum = 0;
            for (int sy = y0; sy < y1; ++sy)
            {
                for (int sx = x0; sx < x1; ++sx)
                {
                    sum += y[sy * srcW + sx];
                }
            }
            luma[(by * 2 + dy) * lumaPitch + bx * 2 + dx] = to8(sum, static_cast<unsigned>((y1 - y0) * (x1 - x0)));
        }
    }
    int y0 = 0;
    int y1 = 0;
    span(by * 2, 2, srcH, dstH, y0, y1);
    int x0 = 0;
    int x1 = 0;
    span(bx * 2, 2, srcW, dstW, x0, x1);
    int const c0 = x0 / 2 < cw ? x0 / 2 : cw - 1;
    int const c1 = (x1 + 1) / 2 > c0 ? ((x1 + 1) / 2 < cw ? (x1 + 1) / 2 : cw) : c0 + 1;
    unsigned sumCb = 0;
    unsigned sumCr = 0;
    for (int sy = y0; sy < y1; ++sy)
    {
        for (int c = c0; c < c1; ++c)
        {
            sumCb += cb[sy * cw + c];
            sumCr += cr[sy * cw + c];
        }
    }
    unsigned const n = static_cast<unsigned>((y1 - y0) * (c1 - c0));
    chroma[by * chromaPitch + bx * 2] = to8(sumCb, n);
    chroma[by * chromaPitch + bx * 2 + 1] = to8(sumCr, n);
}
} // namespace mv::nv12
