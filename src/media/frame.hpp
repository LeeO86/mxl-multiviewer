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
void unpackAlpha10(std::uint8_t const* src, int srcRowBytes, Frame422& dst);
void packAlpha10(Frame422 const& src, std::uint8_t* dst, int dstRowBytes);

std::uint64_t lumaHash(Frame422 const& frame);
} // namespace mv
