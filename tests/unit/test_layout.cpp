#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "layout/geometry.hpp"
#include "layout/model.hpp"

using namespace mv;

TEST_CASE("presets cover the canvas without leaving it")
{
    auto const layouts = builtinPresets(16);
    std::vector<std::string> names;
    for (auto const& layout : layouts)
    {
        names.push_back(layout.name);
        CHECK_FALSE(validateLayout(layout, 16).has_value());
        for (auto const& tile : layout.tiles)
        {
            CHECK(tile.rect.x >= 0);
            CHECK(tile.rect.y >= 0);
            CHECK(tile.rect.x + tile.rect.w <= 1.0 + 1e-6);
            CHECK(tile.rect.y + tile.rect.h <= 1.0 + 1e-6);
        }
    }
    CHECK(std::find(names.begin(), names.end(), "2x2") != names.end());
    CHECK(std::find(names.begin(), names.end(), "2+6") != names.end());
    CHECK(std::find(names.begin(), names.end(), "1+7") != names.end());
    CHECK(std::find(names.begin(), names.end(), "5x5") != names.end());
    auto const two = *std::find_if(layouts.begin(), layouts.end(), [](Layout const& layout) { return layout.name == "2x2"; });
    CHECK(two.tiles.size() == 4);
    CHECK(two.tiles[1].rect.x == doctest::Approx(0.5));
}

TEST_CASE("layout json round trip and rejection")
{
    auto const book = defaultBook(16, "2x2");
    Layout parsed;
    auto const body = layoutToJson(book.layouts[1]);
    CHECK_FALSE(parseLayout(body, parsed, 16).has_value());
    CHECK(parsed.name == book.layouts[1].name);
    CHECK(parsed.tiles.size() == book.layouts[1].tiles.size());
    Layout rms;
    CHECK_FALSE(parseLayout("{\"version\":1,\"name\":\"rms\",\"tiles\":[{\"id\":\"a\",\"content\":\"input\",\"input\":1,\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1},\"audio_bars\":true,\"audio_bar_rms\":true}]}", rms, 4).has_value());
    CHECK(rms.tiles[0].audioBarRms);
    CHECK(layoutToJson(rms).find("\"audio_bar_rms\":true") != std::string::npos);
    Layout bad;
    CHECK(parseLayout("{\"version\":1,\"name\":\"x\",\"tiles\":[{\"id\":\"a\",\"content\":\"input\",\"input\":99,\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}", bad, 4)
              .has_value());
    CHECK(parseLayout("{\"version\":1,\"name\":\"x\",\"tiles\":[{\"id\":\"a\",\"rect\":{\"x\":0,\"y\":0,\"w\":1.2,\"h\":1}}]}", bad, 4).has_value());
}

TEST_CASE("tile geometry snaps to even pixels and letterboxes")
{
    auto const full = rectToPixels({0, 0, 0.5, 0.5}, 1920, 1080);
    CHECK(full.x == 0);
    CHECK(full.y == 0);
    CHECK(full.w == 960);
    CHECK(full.h == 540);
    auto const right = rectToPixels({0.5, 0, 0.5, 1}, 1920, 1080);
    CHECK(right.x == 960);
    CHECK((right.x % 2) == 0);
    PixelRect tile{0, 0, 96, 96};
    auto const fit = placeTile(tile, 96, 54, ScaleMode::Fit);
    CHECK(fit.dst.w == 96);
    CHECK(fit.dst.h == 54);
    CHECK(fit.dst.y == 21);
    auto const fill = placeTile(tile, 96, 54, ScaleMode::Fill);
    CHECK(fill.dst.w == 96);
    CHECK(fill.dst.h == 96);
    CHECK(fill.srcW < 96.f);
    CHECK(fill.srcH == doctest::Approx(54.f));
}
