#include <doctest/doctest.h>

#include "layout/geometry.hpp"
#include "layout/model.hpp"
#include "media/alarm.hpp"
#include "media/frame.hpp"
#include "media/overlay.hpp"
#include "media/ppm.hpp"
#include "media/scale.hpp"
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
