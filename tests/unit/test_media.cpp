#include <doctest/doctest.h>

#include "layout/geometry.hpp"
#include "layout/model.hpp"
#include "media/alarm.hpp"
#include "media/audioring.hpp"
#include "media/frame.hpp"
#include "media/jpeg.hpp"
#include "media/overlay.hpp"
#include "media/ppm.hpp"
#include "media/scale.hpp"
#include "media/timebase.hpp"
#include "ops/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace mv;

TEST_CASE("v210 pack unpack is bit exact")
{
    Frame422 frame;
    frame.allocate(96, 8, true);
    std::uint32_t state = 1;
    for (int i = 0; i < frame.width * frame.height; ++i)
    {
        state = state * 1664525u + 1013904223u;
        frame.y[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(state & 0x3ffu);
        if (frame.hasAlpha)
        {
            frame.a[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>((state >> 10) & 0x3ffu);
        }
    }
    for (int i = 0; i < frame.chromaWidth() * frame.height; ++i)
    {
        state = state * 1664525u + 1013904223u;
        frame.cb[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(state & 0x3ffu);
        frame.cr[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>((state >> 4) & 0x3ffu);
    }
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(v210RowBytes(frame.width)) * frame.height);
    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(alpha10RowBytes(frame.width)) * frame.height);
    packV210(frame, packed.data(), 0);
    packAlpha10(frame, alpha.data(), 0);
    Frame422 back;
    back.allocate(frame.width, frame.height, true);
    unpackV210(packed.data(), 0, back);
    unpackAlpha10(alpha.data(), 0, back);
    CHECK(back.y == frame.y);
    CHECK(back.cb == frame.cb);
    CHECK(back.cr == frame.cr);
    CHECK(back.a == frame.a);

    Frame422 narrow;
    narrow.allocate(10, 2, false);
    narrow.fill(100, 200, 300);
    narrow.y[1] = 7;
    narrow.y[9] = 9;
    std::vector<std::uint8_t> narrowPacked(static_cast<std::size_t>(v210RowBytes(10)) * 2);
    packV210(narrow, narrowPacked.data(), 0);
    Frame422 narrowBack;
    narrowBack.allocate(10, 2, false);
    unpackV210(narrowPacked.data(), 0, narrowBack);
    CHECK(narrowBack.y == narrow.y);
    CHECK(narrowBack.cb == narrow.cb);
}

TEST_CASE("alarm luma from packed v210 matches the unpacked frame")
{
    // The CUDA backend does not unpack on the CPU: black and freeze read the packed grain.
    Frame422 frame;
    frame.allocate(1920, 1080, false);
    std::uint32_t state = 7;
    for (auto& value : frame.y)
    {
        state = state * 1664525u + 1013904223u;
        value = static_cast<std::uint16_t>(state & 0x3ffu);
    }
    int const rowBytes = static_cast<int>(v210RowBytes(frame.width));
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(rowBytes) * frame.height);
    packV210(frame, packed.data(), rowBytes);
    for (int i = 0; i < frame.width * frame.height; i += 997)
    {
        CHECK(v210Luma(packed.data(), rowBytes, frame.width, i) == frame.y[static_cast<std::size_t>(i)]);
    }
    CHECK(lumaHash(packed.data(), rowBytes, frame.width, frame.height) == lumaHash(frame));
    // The one-pass sum for the black alarm, also with a width the step does not divide.
    for (int const width : {1920, 100})
    {
        CAPTURE(width);
        Frame422 small;
        small.allocate(width, 37, false);
        for (std::size_t i = 0; i < small.y.size(); ++i)
        {
            small.y[i] = frame.y[i];
        }
        int const smallRow = static_cast<int>(v210RowBytes(width));
        std::vector<std::uint8_t> smallPacked(static_cast<std::size_t>(smallRow) * small.height);
        packV210(small, smallPacked.data(), smallRow);
        std::uint64_t expected = 0;
        int expectedCount = 0;
        for (int i = 0; i < width * small.height; i += 32)
        {
            expected += v210Luma(smallPacked.data(), smallRow, width, i);
            ++expectedCount;
        }
        int count = 0;
        CHECK(v210LumaSum(smallPacked.data(), smallRow, width, small.height, 32, &count) == expected);
        CHECK(count == expectedCount);
        // The copy with both alarm values taken on the way.
        std::vector<std::uint8_t> copy(smallPacked.size(), 0);
        V210Scan scan;
        copyV210Scan(smallPacked.data(), copy.data(), smallRow, width, small.height, 32, scan);
        CHECK(copy == smallPacked);
        CHECK(scan.sum == expected);
        CHECK(scan.count == expectedCount);
        CHECK(scan.hash == lumaHash(smallPacked.data(), smallRow, width, small.height));
    }
}

TEST_CASE("preview from packed v210 matches the unpacked frame")
{
    Frame422 frame;
    frame.allocate(192, 108, false);
    std::uint32_t state = 11;
    for (auto& value : frame.y)
    {
        state = state * 1664525u + 1013904223u;
        value = static_cast<std::uint16_t>(64 + (state & 0x3ffu) % 877);
    }
    for (std::size_t i = 0; i < frame.cb.size(); ++i)
    {
        state = state * 1664525u + 1013904223u;
        frame.cb[i] = static_cast<std::uint16_t>(64 + (state & 0x3ffu) % 897);
        frame.cr[i] = static_cast<std::uint16_t>(64 + ((state >> 10) & 0x3ffu) % 897);
    }
    int const rowBytes = static_cast<int>(v210RowBytes(frame.width));
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(rowBytes) * frame.height);
    packV210(frame, packed.data(), rowBytes);
    CHECK(encodePreviewJpeg(packed.data(), rowBytes, frame.width, frame.height, 64, 60) == encodePreviewJpeg(frame, 64, 60));
}

TEST_CASE("scaler matches bilinear pixel centres and compose keeps tile colours")
{
    Frame422 src;
    src.allocate(4, 2, false);
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            src.y[static_cast<std::size_t>(y * 4 + x)] = static_cast<std::uint16_t>(100 * x + 10 * y);
        }
    }
    std::fill(src.cb.begin(), src.cb.end(), 400);
    std::fill(src.cr.begin(), src.cr.end(), 600);
    Frame422 dst;
    dst.allocate(2, 1, false);
    Placement place;
    place.dst = {0, 0, 2, 1};
    place.srcW = 4;
    place.srcH = 2;
    scaleInto(dst, place, src, false);
    // dst (0,0) centre maps to src ((0.5)*4/2 - 0.5, (0.5)*2/1 - 0.5) = (0.5, 0.5)
    // samples (0,0)=0, (1,0)=100, (0,1)=10, (1,1)=110 → 55
    CHECK(std::abs(static_cast<int>(dst.y[0]) - 55) <= 1);
    CHECK(std::abs(static_cast<int>(dst.cb[0]) - 400) <= 1);

    Frame422 canvas;
    canvas.allocate(96, 54, false);
    auto const layouts = builtinPresets(4);
    Layout const* grid = nullptr;
    for (auto const& layout : layouts)
    {
        if (layout.name == "2x2")
        {
            grid = &layout;
        }
    }
    REQUIRE(grid != nullptr);
    std::vector<Frame422> sources(4);
    int const colours[4] = {200, 400, 600, 800};
    std::vector<ComposeTile> tiles;
    for (int i = 0; i < 4; ++i)
    {
        sources[static_cast<std::size_t>(i)].allocate(96, 54, false);
        sources[static_cast<std::size_t>(i)].fill(static_cast<std::uint16_t>(colours[i]), 512, 512);
        auto const px = rectToPixels(grid->tiles[static_cast<std::size_t>(i)].rect, 96, 54);
        ComposeTile tile;
        tile.place = placeTile(px, 96, 54, ScaleMode::Fit);
        tile.source = &sources[static_cast<std::size_t>(i)];
        tiles.push_back(tile);
    }
    composeTiles(canvas, 64, 512, 512, nullptr, 0, tiles.data(), 4);
    auto sample = [&](int x, int y) { return canvas.y[static_cast<std::size_t>(y * 96 + x)]; };
    CHECK(sample(24, 13) == 200);
    CHECK(sample(72, 13) == 400);
    CHECK(sample(24, 40) == 600);
    CHECK(sample(72, 40) == 800);

    Frame422 wall;
    wall.allocate(96, 96, false);
    Frame422 wide;
    wide.allocate(96, 54, false);
    wide.fill(320, 512, 512);
    ComposeTile letter;
    letter.place = placeTile(PixelRect{0, 0, 96, 96}, 96, 54, ScaleMode::Fit);
    letter.source = &wide;
    composeTiles(wall, 64, 512, 512, nullptr, 0, &letter, 1);
    CHECK(wall.y[0] == 64);
    CHECK(wall.y[static_cast<std::size_t>(48 * 96 + 48)] == 320);

    Frame422 solid;
    solid.allocate(192, 108, false);
    solid.fill(200, 512, 512);
    Frame422 full;
    full.allocate(192, 108, false);
    auto const placed = placeTile(PixelRect{0, 0, 192, 108}, 192, 108, ScaleMode::Fit);
    scaleInto(full, placed, solid, false);
    CHECK(full.y[10 * 192 + 10] == 200);
    CHECK(full.y[54 * 192 + 96] == 200);
    CHECK(full.y[20 * 192 + 24] == 200);
}

TEST_CASE("scaling straight from v210 matches the unpacked scaler within 1")
{
    Frame422 src;
    src.allocate(196, 110, false);
    std::uint32_t state = 7;
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
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(v210RowBytes(src.width)) * src.height);
    packV210(src, packed.data(), 0);
    V210View const view{packed.data(), static_cast<int>(v210RowBytes(src.width)), src.width, src.height};
    // Down 4:1 and odd ratios, a crop, an upscale, and bob for interlaced sources.
    struct Case
    {
        PixelRect dst;
        float x, y, w, h;
        bool bob;
    };
    Case const cases[] = {{{0, 0, 49, 27}, 0, 0, 196, 110, false}, {{3, 2, 37, 23}, 10, 5, 150, 90, false}, {{0, 0, 196, 110}, 0, 0, 196, 110, false},
        {{0, 0, 300, 160}, 20, 10, 100, 60, false}, {{1, 1, 60, 33}, 0, 0, 196, 110, true}};
    for (auto const& c : cases)
    {
        CAPTURE(c.dst.w);
        CAPTURE(c.bob);
        Frame422 a;
        Frame422 b;
        a.allocate(320, 180, false);
        b.allocate(320, 180, false);
        Placement place;
        place.dst = c.dst;
        place.srcX = c.x;
        place.srcY = c.y;
        place.srcW = c.w;
        place.srcH = c.h;
        scaleInto(a, place, src, c.bob);
        scaleV210Into(b, place, view, c.bob);
        int worst = 0;
        for (std::size_t i = 0; i < a.y.size(); ++i)
        {
            worst = std::max(worst, std::abs(static_cast<int>(a.y[i]) - static_cast<int>(b.y[i])));
        }
        for (std::size_t i = 0; i < a.cb.size(); ++i)
        {
            worst = std::max(worst, std::abs(static_cast<int>(a.cb[i]) - static_cast<int>(b.cb[i])));
            worst = std::max(worst, std::abs(static_cast<int>(a.cr[i]) - static_cast<int>(b.cr[i])));
        }
        CHECK(worst <= 1);
    }
    // Flat areas stay exact.
    Frame422 flat;
    flat.allocate(96, 54, false);
    flat.fill(321, 456, 789);
    std::vector<std::uint8_t> flatPacked(static_cast<std::size_t>(v210RowBytes(96)) * 54);
    packV210(flat, flatPacked.data(), 0);
    Frame422 out;
    out.allocate(40, 20, false);
    Placement place;
    place.dst = {0, 0, 40, 20};
    place.srcW = 96;
    place.srcH = 54;
    scaleV210Into(out, place, V210View{flatPacked.data(), static_cast<int>(v210RowBytes(96)), 96, 54}, false);
    CHECK(std::all_of(out.y.begin(), out.y.end(), [](std::uint16_t v) { return v == 321; }));
    CHECK(std::all_of(out.cb.begin(), out.cb.end(), [](std::uint16_t v) { return v == 456; }));
    CHECK(std::all_of(out.cr.begin(), out.cr.end(), [](std::uint16_t v) { return v == 789; }));
}

TEST_CASE("prepared overlay blends exactly like blendStraightRgba")
{
    int const w = 64;
    int const h = 12;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4, 0);
    std::uint32_t state = 3;
    auto next = [&] { return state = state * 1664525u + 1013904223u; };
    for (std::size_t i = 0; i < rgba.size(); i += 4)
    {
        // Mostly clear, some opaque, some partial, as overlays are.
        auto const kind = (next() >> 8) % 4;
        rgba[i] = static_cast<std::uint8_t>(next() >> 24);
        rgba[i + 1] = static_cast<std::uint8_t>(next() >> 24);
        rgba[i + 2] = static_cast<std::uint8_t>(next() >> 24);
        rgba[i + 3] = kind == 0 ? 255 : kind == 1 ? static_cast<std::uint8_t>(next() >> 24) : 0;
    }
    Frame422 a;
    a.allocate(w, h, false);
    for (std::size_t i = 0; i < a.y.size(); ++i)
    {
        a.y[i] = static_cast<std::uint16_t>(64 + i % 800);
    }
    Frame422 b = a;
    blendStraightRgba(a, rgba.data(), w * 4);
    blendOverlay(b, prepareOverlay(rgba.data(), w, h, w * 4));
    CHECK(a.y == b.y);
    CHECK(a.cb == b.cb);
    CHECK(a.cr == b.cr);
}

TEST_CASE("ppm type IIa ballistics")
{
    PpmConfig config;
    PpmMeter meter;
    std::vector<float> ones(480, 1.f);
    meter.process(ones.data(), static_cast<int>(ones.size()), 48000.0, config);
    double const expected = 1.0 - std::exp(-0.010 / config.attackTauSec);
    CHECK(meter.level == doctest::Approx(expected).epsilon(0.02));
    double const before = meter.level;
    std::vector<float> silence(static_cast<std::size_t>(48000 * 2.8), 0.f);
    meter.process(silence.data(), static_cast<int>(silence.size()), 48000.0, config);
    double const ratio = meter.level / before;
    CHECK(ratio == doctest::Approx(std::pow(10.0, -24.0 / 20.0)).epsilon(0.05));
    CHECK(meter.levelDbfs() < -20.0);
}

TEST_CASE("alarm debounce")
{
    Debounce alarm;
    CHECK_FALSE(alarm.update(true, 0, 500, 500));
    CHECK_FALSE(alarm.active);
    CHECK(alarm.update(true, 500, 500, 500));
    CHECK(alarm.active);
    CHECK_FALSE(alarm.update(false, 500, 500, 500));
    CHECK(alarm.active);
    CHECK(alarm.update(false, 1000, 500, 500));
    CHECK_FALSE(alarm.active);
}

TEST_CASE("overlay draws umd pixels")
{
    Overlay overlay;
    overlay.resize(320, 180);
    OverlayTile tile;
    tile.rect = {0, 0, 320, 180};
    tile.umd = true;
    tile.umdText = "CAM 1";
    tile.umdFont = 16;
    tile.tally = 1;
    tile.tallyBorder = true;
    tile.badge = "NOSIG";
    renderOverlay(overlay, {tile});
    int ink = 0;
    for (std::size_t i = 3; i < overlay.rgba.size(); i += 4)
    {
        if (overlay.rgba[i] != 0)
        {
            ++ink;
        }
    }
    CHECK(ink > 20);
    CHECK(overlayUsesBlend2d());
    Overlay word;
    word.resize(160, 64);
    OverlayTile label;
    label.rect = {0, 0, 160, 64};
    label.labelText = "Ag";
    label.umdFont = 36;
    renderOverlay(word, {label});
    int partial = 0;
    bool straightWhite = false;
    for (int y = 0; y < word.height; ++y)
    {
        for (int x = 0; x < word.width; ++x)
        {
            auto const* px = word.rgba.data() + static_cast<std::size_t>((y * word.width + x) * 4);
            if (px[3] > 8 && px[3] < 247 && px[0] > 180 && px[1] > 180 && px[2] > 180)
            {
                ++partial;
                if (px[0] > px[3])
                {
                    straightWhite = true;
                }
            }
        }
    }
    CHECK(partial > 20);
    CHECK(straightWhite);
}

TEST_CASE("format caption does not cover the picture sample")
{
    Overlay overlay;
    overlay.resize(192, 108);
    OverlayTile tile;
    tile.rect = {0, 0, 96, 54};
    tile.formatText = "192x108p50";
    tile.umd = true;
    tile.umdText = "MV In 1";
    tile.umdFont = 8;
    tile.tallyBorder = true;
    tile.tally = 1;
    renderOverlay(overlay, {tile});
    auto at = [&](int x, int y) { return overlay.rgba.data() + static_cast<std::size_t>((y * overlay.width + x) * 4); };
    CHECK(at(24, 20)[3] == 0);
    CHECK(at(48, 28)[3] == 0);
    CHECK(at(180, 100)[3] == 0);
}

TEST_CASE("timecode comes from the TAI index")
{
    int num = 0;
    int den = 1;
    CHECK(parseRateToken("25", num, den));
    CHECK(num == 25);
    CHECK(den == 1);
    CHECK(parseRateToken("30000/1001", num, den));
    CHECK(num == 30000);
    CHECK(den == 1001);
    CHECK(parseRateToken("2997", num, den));
    CHECK(num == 30000);
    auto const text = formatTimecode(1500000000ull, 25, 1);
    CHECK(text == "00:00:01:13");
}

TEST_CASE("background cover scales into the canvas")
{
    Frame422 src;
    src.allocate(4, 2, false);
    src.fill(200, 512, 512);
    Frame422 dst;
    dst.allocate(8, 8, false);
    coverFrame(dst, src);
    CHECK(dst.y[static_cast<std::size_t>(4 * 8 + 4)] == 200);
}

TEST_CASE("analogue clock draws hands and timecode")
{
    Overlay overlay;
    overlay.resize(200, 200);
    OverlayTile tile;
    tile.rect = {0, 0, 200, 200};
    tile.clock = true;
    tile.analogue = true;
    tile.clockHour = 0;
    tile.clockMinute = 0;
    tile.clockSecond = 0;
    tile.timecodeText = "00:00:00:00";
    renderOverlay(overlay, {tile});
    auto ink = [&](int x, int y) {
        auto const* px = overlay.rgba.data() + static_cast<std::size_t>((y * 200 + x) * 4);
        return px[3] != 0;
    };
    CHECK(ink(100, 20));
    CHECK_FALSE(ink(20, 100));
    int textInk = 0;
    for (int y = 170; y < 198; ++y)
    {
        for (int x = 0; x < 160; ++x)
        {
            if (ink(x, y))
            {
                ++textInk;
            }
        }
    }
    CHECK(textInk > 10);
}

TEST_CASE("metrics prefix")
{
    Metrics metrics;
    metrics.inc("output_frames_total", {{"head", "1"}}, 3);
    metrics.observe("compose_seconds", {{"head", "1"}, {"backend", "cpu"}}, 0.004);
    auto const text = metrics.render();
    CHECK(text.find("mxl_multiviewer_output_frames_total") != std::string::npos);
    CHECK(text.find("mxl_multiviewer_compose_seconds_count") != std::string::npos);
}

TEST_CASE("overlay changes cover every changed pixel and nothing else")
{
    int const width = 300;
    int const height = 70;
    std::vector<std::uint8_t> before(static_cast<std::size_t>(width * height * 4), 0);
    auto after = before;
    CHECK(overlayChanges(before, after, width, height).empty());

    // One pixel in the second block row, third block column, and one at the right edge.
    after[static_cast<std::size_t>((20 * width + 130) * 4 + 3)] = 255;
    after[static_cast<std::size_t>((69 * width + 299) * 4)] = 7;
    auto const changes = overlayChanges(before, after, width, height);
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].x == 128);
    CHECK(changes[0].y == 16);
    CHECK(changes[0].w == 64);
    CHECK(changes[0].h == 16);
    CHECK(changes[1].x == 256);
    CHECK(changes[1].y == 64);
    CHECK(changes[1].w == 44);
    CHECK(changes[1].h == 6);

    // Without a previous overlay of the same size, everything is new.
    auto const all = overlayChanges({}, after, width, height);
    REQUIRE(all.size() == 1);
    CHECK(all[0].w == width);
    CHECK(all[0].h == height);
}

namespace
{
std::uint8_t const* pixel(Overlay const& overlay, int x, int y)
{
    return overlay.rgba.data() + static_cast<std::size_t>((y * overlay.width + x) * 4);
}

// Pixels in [x0, x1) × [y0, y1) that pass `test`.
template <typename Test>
int countPixels(Overlay const& overlay, int x0, int y0, int x1, int y1, Test test)
{
    int count = 0;
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            if (test(pixel(overlay, x, y)))
            {
                ++count;
            }
        }
    }
    return count;
}

bool inked(std::uint8_t const* px)
{
    return px[3] != 0;
}

// Width of the ink in a region (0 when empty).
int inkWidth(Overlay const& overlay, int x0, int y0, int x1, int y1)
{
    int left = x1;
    int right = x0 - 1;
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            if (inked(pixel(overlay, x, y)))
            {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    return right >= left ? right - left + 1 : 0;
}
} // namespace

TEST_CASE("audio bars draw zones, PPM scale, peak hold, and clip")
{
    // 1080 canvas: 14 px bars, 6 px margin, meter from y=15 to y=534 (-60..0 dBFS).
    Overlay overlay;
    overlay.resize(1920, 1080);
    OverlayTile tile;
    tile.rect = {0, 0, 960, 540};
    tile.bars = true;
    tile.barChannels = 2;
    tile.ppmDbfs[0] = -6;
    tile.ppmDbfs[1] = -30;
    tile.holdDbfs[1] = -12;
    tile.clip[0] = true;
    renderOverlay(overlay, {tile});
    auto const red = [](std::uint8_t const* px) { return px[3] > 200 && px[0] > 180 && px[1] < 90; };
    auto const amber = [](std::uint8_t const* px) { return px[3] > 200 && px[0] > 180 && px[1] > 130 && px[2] < 90; };
    auto const green = [](std::uint8_t const* px) { return px[3] > 200 && px[0] < 90 && px[1] > 150; };
    // Channel 0 (x 924..937) at -6 dBFS: red above -9, amber to -18, green below.
    CHECK(red(pixel(overlay, 930, 80)));
    CHECK(amber(pixel(overlay, 930, 130)));
    CHECK(green(pixel(overlay, 930, 300)));
    // Clip indicator above channel 0 lit, above channel 1 dark.
    CHECK(pixel(overlay, 930, 9)[0] > 230);
    CHECK(pixel(overlay, 946, 9)[0] < 140);
    // Channel 1 (x 940..953) at -30 dBFS: green low, empty above, peak hold at -12 (y 118).
    CHECK(green(pixel(overlay, 946, 400)));
    CHECK(pixel(overlay, 946, 200)[1] < 60);
    CHECK(pixel(overlay, 946, 118)[0] > 230);
    CHECK(pixel(overlay, 946, 118)[1] > 190);
    // Ticks left of the bars and the dBFS labels left of them.
    CHECK(pixel(overlay, 920, 171)[0] > 150);
    CHECK(countPixels(overlay, 870, 10, 917, 540, [](std::uint8_t const* px) { return px[3] > 100 && px[0] > 150; }) > 40);
    // Nothing outside the bar column.
    CHECK(countPixels(overlay, 0, 0, 860, 540, inked) == 0);

    Overlay unrouted;
    unrouted.resize(1920, 1080);
    tile.audioRouted = false;
    renderOverlay(unrouted, {tile});
    // No fill and no scale without routed audio, only dim bars and a cross.
    CHECK(countPixels(unrouted, 900, 0, 960, 540, red) == 0);
    CHECK(countPixels(unrouted, 870, 10, 917, 540, [](std::uint8_t const* px) { return px[3] > 100 && px[0] > 150; }) == 0);
    CHECK(pixel(unrouted, 930, 450)[3] < 200);
    CHECK(pixel(unrouted, 939, 274)[0] > 150);
}

TEST_CASE("digital clock and label text scale with the tile")
{
    Overlay overlay;
    overlay.resize(1920, 1080);
    OverlayTile small;
    small.rect = {0, 0, 240, 135};
    small.clock = true;
    small.clockText = "12:34:56";
    OverlayTile big = small;
    big.rect = {960, 540, 960, 540};
    OverlayTile label;
    label.rect = {960, 0, 960, 270};
    label.labelText = "Studio 2";
    renderOverlay(overlay, {small, big, label});
    int const smallW = inkWidth(overlay, 0, 0, 240, 135);
    int const bigW = inkWidth(overlay, 960, 540, 1920, 1080);
    CHECK(smallW > 120);
    CHECK(bigW > 576);
    CHECK(bigW > 3 * smallW);
    // Both fit inside their tiles.
    CHECK(smallW < 240);
    CHECK(bigW < 960);
    CHECK(inkWidth(overlay, 960, 0, 1920, 270) > 300);
}

TEST_CASE("slate shows the state and the input in the middle of the tile")
{
    Overlay overlay;
    overlay.resize(960, 540);
    OverlayTile tile;
    tile.rect = {0, 0, 480, 270};
    tile.slate = "NO SIGNAL";
    tile.slateLabel = "MV In 3";
    renderOverlay(overlay, {tile});
    CHECK(countPixels(overlay, 0, 95, 480, 140, inked) > 200);
    CHECK(countPixels(overlay, 0, 145, 480, 178, inked) > 30);
    CHECK(countPixels(overlay, 0, 0, 480, 80, inked) == 0);
    CHECK(countPixels(overlay, 0, 190, 480, 270, inked) == 0);
}

TEST_CASE("alarm border and badge colours follow the alarm level")
{
    // 540 canvas: tally border 4 px, alarm border 2 px inside it.
    Overlay overlay;
    overlay.resize(960, 540);
    OverlayTile silence;
    silence.rect = {0, 0, 480, 270};
    silence.alarm = AlarmLevel::Amber;
    OverlayTile black;
    black.rect = {480, 0, 480, 270};
    black.alarm = AlarmLevel::Red;
    black.badge = "BLACK";
    renderOverlay(overlay, {silence, black});
    auto const* amberEdge = pixel(overlay, 5, 135);
    CHECK(amberEdge[3] > 200);
    CHECK(amberEdge[0] > 200);
    CHECK(amberEdge[1] > 120);
    CHECK(amberEdge[2] < 80);
    auto const* redEdge = pixel(overlay, 485, 135);
    CHECK(redEdge[3] > 200);
    CHECK(redEdge[0] > 200);
    CHECK(redEdge[1] < 80);
    CHECK(countPixels(overlay, 680, 8, 760, 22, [](std::uint8_t const* px) { return px[3] > 200 && px[0] > 150 && px[1] < 80; }) > 20);
    // Nothing in the middle of either tile.
    CHECK(countPixels(overlay, 100, 100, 380, 200, inked) == 0);
    CHECK(countPixels(overlay, 580, 100, 860, 200, inked) == 0);
}

TEST_CASE("tally lamps show LH and RH, the border the combined tally, tally_text the caption background")
{
    // 540 canvas: border 4 px; caption band 36 px from y=234 (the bottom border under its last rows);
    // 18 px lamps at x=4 and x=458, y=243.
    auto const isColour = [](Rgba c) {
        return [c](std::uint8_t const* px) { return px[3] == c.a && std::abs(px[0] - c.r) <= 2 && std::abs(px[1] - c.g) <= 2 && std::abs(px[2] - c.b) <= 2; };
    };
    Rgba const red{220, 32, 32, 255};
    Rgba const green{32, 180, 64, 255};
    Rgba const amber{220, 160, 32, 255};
    Rgba const black{0, 0, 0, 255};
    Rgba const umdBg{0, 0, 0, 192};
    OverlayTile tile;
    tile.rect = {0, 0, 480, 270};
    tile.umd = true;
    tile.umdText = "CAM";
    tile.umdFont = 28;
    tile.umdBg = umdBg;
    tile.tallyBorder = true;
    tile.tallyLamp = true;
    tile.lhTally = 1;
    tile.rhTally = 2;
    tile.textTally = 3;
    tile.tally = 3;
    int const lampArea = 18 * 18;

    Overlay off;
    off.resize(960, 540);
    renderOverlay(off, {tile});
    CHECK(countPixels(off, 4, 243, 22, 261, isColour(red)) == lampArea);
    CHECK(countPixels(off, 458, 243, 476, 261, isColour(green)) == lampArea);
    CHECK(countPixels(off, 0, 100, 4, 140, isColour(amber)) == 4 * 40);
    // tally_text off: the caption keeps umd_bg and the text its colour (white here).
    CHECK(countPixels(off, 200, 236, 440, 262, isColour(umdBg)) == 240 * 26);
    CHECK(countPixels(off, 26, 236, 120, 262, isColour(black)) == 0);

    // tally_text on: the band is the text tally, the text is black, the lamps stay LH and RH.
    tile.textTallyBg = true;
    Overlay on;
    on.resize(960, 540);
    renderOverlay(on, {tile});
    CHECK(countPixels(on, 200, 236, 440, 262, isColour(amber)) == 240 * 26);
    CHECK(countPixels(on, 26, 236, 120, 262, isColour(black)) > 20);
    CHECK(countPixels(on, 4, 243, 22, 261, isColour(red)) == lampArea);
    CHECK(countPixels(on, 458, 243, 476, 261, isColour(green)) == lampArea);

    // Text tally off: tally_text leaves umd_bg. An off lamp is not drawn; the border falls back to RH.
    tile.textTally = 0;
    tile.lhTally = 0;
    tile.tally = 2;
    Overlay plain;
    plain.resize(960, 540);
    renderOverlay(plain, {tile});
    CHECK(countPixels(plain, 200, 236, 440, 262, isColour(umdBg)) == 240 * 26);
    CHECK(countPixels(plain, 4, 243, 22, 261, isColour(umdBg)) == lampArea);
    CHECK(countPixels(plain, 458, 243, 476, 261, isColour(green)) == lampArea);
    CHECK(countPixels(plain, 0, 100, 4, 140, isColour(green)) == 4 * 40);
}

TEST_CASE("audio ring keeps the newest samples and copies by sample index")
{
    AudioRing ring(8);
    std::vector<float> left{1, 2, 3, 4, 5};
    std::vector<float> right{-1, -2, -3, -4, -5};
    ring.push(100, {left.data(), right.data()}, 5);
    CHECK(ring.channels() == 2);
    std::vector<float> out(6, 9.f);
    ring.copy(1, 99, 6, out.data());
    // 99 is not held and stays as it was; 100..104 are the samples.
    CHECK(out == std::vector<float>{9, -1, -2, -3, -4, -5});

    // Continuing past the capacity wraps; only the newest 8 samples remain.
    std::vector<float> more{6, 7, 8, 9, 10, 11};
    ring.push(105, {more.data(), more.data()}, 6);
    std::vector<float> all(11, 0.f);
    ring.copy(0, 100, 11, all.data());
    CHECK(all == std::vector<float>{0, 0, 0, 4, 5, 6, 7, 8, 9, 10, 11});

    // A gap starts over at the new index.
    std::vector<float> later{42, 43};
    ring.push(200, {later.data(), later.data()}, 2);
    std::vector<float> gap(3, 0.f);
    ring.copy(0, 110, 3, gap.data());
    CHECK(gap == std::vector<float>{0, 0, 0});
    ring.copy(0, 199, 3, gap.data());
    CHECK(gap == std::vector<float>{0, 42, 43});

    ring.clear();
    std::vector<float> none(2, 7.f);
    ring.copy(0, 200, 2, none.data());
    CHECK(none == std::vector<float>{7, 7});
}

TEST_CASE("background colours convert to limited-range YCbCr")
{
    auto const black = toYcbcr10(parseHexColor("#000000"));
    CHECK(black.y == 64);
    CHECK(black.cb == 512);
    CHECK(black.cr == 512);
    auto const white = toYcbcr10(parseHexColor("#ffffff"));
    CHECK(white.y == 940);
    CHECK(white.cb == 512);
    CHECK(white.cr == 512);
    CHECK(toYcbcr10(parseHexColor("#101010")).y == 119);
    auto const blue = toYcbcr10(parseHexColor("#0000ff"));
    CHECK(blue.cb > 900);
}
