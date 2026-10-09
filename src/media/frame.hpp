#pragma once

#include <cstdint>
#include <vector>

namespace mv
{
struct Frame422
{
    int width = 0;
    int height = 0;
    bool hasAlpha = false;
    std::vector<std::uint16_t> y;
    std::vector<std::uint16_t> cb;
    std::vector<std::uint16_t> cr;
    std::vector<std::uint16_t> a;

    void allocate(int w, int h, bool alpha);
    void fill(std::uint16_t yValue, std::uint16_t cbValue, std::uint16_t crValue);
    [[nodiscard]] int chromaWidth() const
    {
        return width / 2;
    }
};

constexpr std::uint32_t v210RowBytes(int width)
{
    return static_cast<std::uint32_t>(((width + 47) / 48) * 128);
}

constexpr std::uint32_t alpha10RowBytes(int width)
{
    return static_cast<std::uint32_t>(((width + 2) / 3) * 4);
}

void unpackV210(std::uint8_t const* src, int srcRowBytes, Frame422& dst);
void packV210(Frame422 const& src, std::uint8_t* dst, int dstRowBytes);
// One line: `width` luma and width/2 chroma samples; packV210Line zeroes the line up to rowBytes.
void unpackV210Line(std::uint8_t const* line, int width, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr);
void packV210Line(std::uint16_t const* y, std::uint16_t const* cb, std::uint16_t const* cr, int width, std::uint8_t* line, int rowBytes);
void unpackAlpha10(std::uint8_t const* src, int srcRowBytes, Frame422& dst);
void packAlpha10(Frame422 const& src, std::uint8_t* dst, int dstRowBytes);

// Freeze hash (§6.3): luma of every second line summed per block of a 32×18 grid, hashed.
std::uint64_t lumaHash(Frame422 const& frame);

// The same luma samples read straight from packed v210, for frames the CPU does not
// unpack (CUDA backend): `index` is row-major like Frame422::y.
std::uint16_t v210Luma(std::uint8_t const* src, int rowBytes, int width, int index);
// Cb and Cr of chroma sample `cx` (pixel pair) in row `row`.
void v210Chroma(std::uint8_t const* src, int rowBytes, int cx, int row, std::uint16_t& cb, std::uint16_t& cr);
// lumaHash() of a packed frame.
std::uint64_t lumaHash(std::uint8_t const* v210, int rowBytes, int width, int height);
// Sum (and count) of v210Luma() at index 0, step, 2·step, … < width·height, in one pass.
std::uint64_t v210LumaSum(std::uint8_t const* src, int rowBytes, int width, int height, int step, int* count);
// Copies a packed frame line by line and, while each line is in cache, takes the black-alarm
// sum (v210LumaSum with sumStep) and the freeze hash (lumaHash) from it.
struct V210Scan
{
    std::uint64_t sum = 0;
    int count = 0;
    std::uint64_t hash = 0;
};
void copyV210Scan(std::uint8_t const* src, std::uint8_t* dst, int rowBytes, int width, int height, int sumStep, V210Scan& scan);
} // namespace mv
