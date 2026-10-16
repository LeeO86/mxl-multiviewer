#include <doctest/doctest.h>

#include "layout/geometry.hpp"
#include "media/frame.hpp"
#include "media/interlace.hpp"
#include "media/scale.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace mv;

namespace
{
// A packed frame and its two fields, as MXL 218ddaa stores an interlaced flow (one field per grain).
struct Fields
{
    std::vector<std::uint8_t> frame;
    std::vector<std::uint8_t> top;
    std::vector<std::uint8_t> bottom;
};

Fields split(Frame422 const& picture)
{
    int const rowBytes = static_cast<int>(v210RowBytes(picture.width));
    Fields f;
    f.frame.resize(static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(picture.height));
    packV210(picture, f.frame.data(), 0);
    int const rows = picture.height / 2;
    f.top.resize(static_cast<std::size_t>(rowBytes) * rows);
    f.bottom.resize(static_cast<std::size_t>(rowBytes) * rows);
    for (int r = 0; r < rows; ++r)
    {
        std::memcpy(f.top.data() + static_cast<std::size_t>(r) * rowBytes, f.frame.data() + static_cast<std::size_t>(2 * r) * rowBytes, rowBytes);
        std::memcpy(f.bottom.data() + static_cast<std::size_t>(r) * rowBytes, f.frame.data() + static_cast<std::size_t>(2 * r + 1) * rowBytes, rowBytes);
    }
    return f;
}

Frame422 noise(int width, int height)
{
    Frame422 src;
    src.allocate(width, height, false);
    std::uint32_t state = 11;
    auto next = [&] { return state = state * 1664525u + 1013904223u; };
    for (auto& v : src.y)
    {
        v = static_cast<std::uint16_t>(64 + (next() >> 8) % 877);
    }
    for (std::size_t i = 0; i < src.cb.size(); ++i)
    {
        src.cb[i] = static_cast<std::uint16_t>(64 + (next() >> 8) % 897);
        src.cr[i] = static_cast<std::uint16_t>(64 + (next() >> 8) % 897);
    }
    return src;
}
} // namespace

TEST_CASE("an interlaced grain is one field of half the lines")
{
    auto const i = videoGrainLayout(1920, 1080, true, false);
    CHECK(i.fields);
    CHECK(i.rows == 540);
    CHECK(i.fillBytes == 5120u * 540u);
    CHECK(i.keyBytes == 0u);
    CHECK(checkVideoGrain(i, 1920, 1080, 5120u * 540u).empty());
    // 1.4.3 read 1080 lines from a 540-line field grain (SIGSEGV): that grain is too small for a
    // progressive reading, and the check says so instead of reading.
    auto const p = videoGrainLayout(1920, 1080, false, false);
    CHECK(p.rows == 1080);
    CHECK(checkVideoGrain(p, 1920, 1080, 5120u * 540u) == "grain_too_small");
    CHECK(checkVideoGrain(p, 1920, 1080, 5120u * 1080u).empty());
    CHECK(checkVideoGrain(p, 1920, 1080, 0).size() > 0);
    // A larger grain (an older writer that stored whole frames) is read safely: the field lines only.
    CHECK(checkVideoGrain(i, 1920, 1080, 5120u * 1080u).empty());
    CHECK(checkVideoGrain(videoGrainLayout(1920, 1081, true, false), 1920, 1081, 5120u * 1080u) == "odd_interlaced_height");
    CHECK(checkVideoGrain(videoGrainLayout(0, 0, true, false), 0, 0, 1000) == "no_raster");
    // v210a: fill then key, per field; a grain with the fill only is still readable.
    auto const a = videoGrainLayout(1920, 1080, true, true);
    CHECK(a.keyBytes == alpha10RowBytes(1920) * 540u);
    CHECK(checkVideoGrain(a, 1920, 1080, a.fillBytes).empty());
}

TEST_CASE("field order: grain 2k is the first field, top for tff and bottom for bff")
{
    CHECK(fieldLineParity(0, false) == 0);
    CHECK(fieldLineParity(1, false) == 1);
    CHECK(fieldLineParity(2, false) == 0);
    CHECK(fieldLineParity(123457, false) == 1);
    CHECK(fieldLineParity(0, true) == 1);
    CHECK(fieldLineParity(1, true) == 0);
    CHECK(fieldLineParity(123456, true) == 1);
}

TEST_CASE("two fields weave back into the frame, with the same alarm numbers")
{
    auto const picture = noise(198, 112);
    auto const f = split(picture);
    int const rowBytes = static_cast<int>(v210RowBytes(picture.width));
    std::vector<std::uint8_t> woven(f.frame.size(), 0xab);
    V210Scan wovenScan;
    weaveV210Scan(f.top.data(), f.bottom.data(), woven.data(), rowBytes, picture.width, picture.height, 32, wovenScan);
    CHECK(woven == f.frame);
    std::vector<std::uint8_t> copy(f.frame.size());
    V210Scan copyScan;
    copyV210Scan(f.frame.data(), copy.data(), rowBytes, picture.width, picture.height, 32, copyScan);
    CHECK(wovenScan.sum == copyScan.sum);
    CHECK(wovenScan.count == copyScan.count);
    CHECK(wovenScan.hash == copyScan.hash);
    int n = 0;
    CHECK(wovenScan.sum == v210LumaSum(f.frame.data(), rowBytes, picture.width, picture.height, 32, &n));
    CHECK(wovenScan.hash == lumaHash(f.frame.data(), rowBytes, picture.width, picture.height));
    std::vector<std::uint8_t> plain(f.frame.size(), 0);
    weaveRows(f.top.data(), f.bottom.data(), plain.data(), rowBytes, picture.height);
    CHECK(plain == f.frame);
    // The same field twice (no field before it yet): every line pair is that field's line.
    weaveRows(f.top.data(), f.top.data(), plain.data(), rowBytes, picture.height);
    CHECK(std::memcmp(plain.data() + rowBytes, f.top.data(), static_cast<std::size_t>(rowBytes)) == 0);
}

TEST_CASE("bob reads only the newest field's lines, on both scalers")
{
    // Even (top field) lines 100, odd (bottom field) lines 900.
    Frame422 src;
    src.allocate(96, 54, false);
    src.fill(64, 512, 512);
    for (int row = 0; row < src.height; ++row)
    {
        std::fill(src.y.begin() + row * src.width, src.y.begin() + (row + 1) * src.width, static_cast<std::uint16_t>((row & 1) == 0 ? 100 : 900));
    }
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(v210RowBytes(96)) * 54);
    packV210(src, packed.data(), 0);
    V210View const view{packed.data(), static_cast<int>(v210RowBytes(96)), 96, 54};
    for (auto const& [dw, dh] : {std::pair{48, 27}, std::pair{96, 54}, std::pair{150, 100}})
    {
        CAPTURE(dh);
        Placement place;
        place.dst = {0, 0, dw, dh};
        place.srcW = 96;
        place.srcH = 54;
        for (auto const& [bob, want] : {std::pair{Bob::Top, 100}, std::pair{Bob::Bottom, 900}})
        {
            Frame422 a;
            Frame422 b;
            a.allocate(dw, dh, false);
            b.allocate(dw, dh, false);
            scaleInto(a, place, src, bob);
            scaleV210Into(b, place, view, bob);
            CHECK(std::all_of(a.y.begin(), a.y.end(), [&](std::uint16_t v) { return v == want; }));
            CHECK(std::all_of(b.y.begin(), b.y.end(), [&](std::uint16_t v) { return v == want; }));
        }
    }
}

TEST_CASE("a woven 1080i-style frame fills the whole tile, from either field")
{
    // Upper half 800, lower half 200 in frame lines: each field holds half of each, so a reader
    // that took a field for a frame (half height) or read past it would not show this edge.
    Frame422 picture;
    picture.allocate(192, 108, false);
    picture.fill(200, 512, 512);
    std::fill(picture.y.begin(), picture.y.begin() + 54 * 192, static_cast<std::uint16_t>(800));
    auto const f = split(picture);
    int const rowBytes = static_cast<int>(v210RowBytes(192));
    std::vector<std::uint8_t> woven(f.frame.size());
    V210Scan scan;
    weaveV210Scan(f.top.data(), f.bottom.data(), woven.data(), rowBytes, 192, 108, 32, scan);
    for (auto const bob : {Bob::Top, Bob::Bottom})
    {
        Frame422 tile;
        tile.allocate(96, 54, false);
        Placement place;
        place.dst = {0, 0, 96, 54};
        place.srcW = 192;
        place.srcH = 108;
        scaleV210Into(tile, place, V210View{woven.data(), rowBytes, 192, 108}, bob);
        for (int row = 0; row < 54; ++row)
        {
            CAPTURE(row);
            auto const v = tile.y[static_cast<std::size_t>(row) * 96 + 48];
            if (row < 26)
            {
                CHECK(v == 800);
            }
            else if (row > 27)
            {
                CHECK(v == 200);
            }
        }
    }
}
