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
    // Presets show two audio bars from the first channel on the right of every input tile.
    for (auto const& layout : layouts)
    {
        for (auto const& tile : layout.tiles)
        {
            CHECK(tile.audioBars);
            CHECK(tile.audioBarChannels == 2);
            CHECK(tile.audioBarFirst == 0);
            CHECK(tile.audioBarPosition == BarsPosition::Right);
        }
    }
    // A saved tile without the key keeps the old default.
    Layout saved;
    CHECK_FALSE(parseLayout("{\"version\":1,\"name\":\"s\",\"tiles\":[{\"id\":\"a\",\"input\":1,\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}", saved, 4).has_value());
    CHECK_FALSE(saved.tiles[0].audioBars);
}

TEST_CASE("layout json escapes quotes and backslashes")
{
    Layout layout;
    layout.name = R"(wall "A" \ 1)";
    Tile caption;
    caption.id = R"(t"1)";
    caption.umdSource = UmdSource::Manual;
    caption.umdText = "CAM \"1\" \\ left\ttab";
    caption.rect = {0, 0, 0.5, 1};
    Tile label;
    label.id = "t2";
    label.content = TileContent::Label;
    label.labelText = R"(C:\feed "B")";
    label.rect = {0.5, 0, 0.5, 1};
    layout.tiles = {caption, label};
    Layout parsed;
    REQUIRE_FALSE(parseLayout(layoutToJson(layout), parsed, 4).has_value());
    CHECK(parsed.name == layout.name);
    CHECK(parsed.tiles[0].id == caption.id);
    CHECK(parsed.tiles[0].umdText == caption.umdText);
    CHECK(parsed.tiles[1].labelText == label.labelText);

    LayoutBook book;
    book.active = layout.name;
    book.layouts = {layout};
    LayoutBook back;
    REQUIRE_FALSE(parseBook(bookToJson(book), back, 4).has_value());
    CHECK(back.active == layout.name);
    CHECK(back.layouts[0].tiles[0].umdText == caption.umdText);
}

TEST_CASE("clock style accepts analog and rejects unknown values")
{
    auto const body = [](std::string const& style, std::string const& zone) {
        return "{\"version\":1,\"name\":\"c\",\"tiles\":[{\"id\":\"c\",\"content\":\"clock\",\"clock_style\":\"" + style + "\",\"clock_zone\":\"" + zone +
               "\",\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}";
    };
    Layout layout;
    REQUIRE_FALSE(parseLayout(body("analog", "local"), layout, 4).has_value());
    CHECK(layout.tiles[0].clockStyle == ClockStyle::Analogue);
    CHECK(layout.tiles[0].clockZone == ClockZone::Local);
    CHECK(layoutToJson(layout).find("\"clock_style\":\"analogue\"") != std::string::npos);
    REQUIRE_FALSE(parseLayout(body("digital", "tai"), layout, 4).has_value());
    CHECK(layout.tiles[0].clockStyle == ClockStyle::Digital);
    CHECK(layout.tiles[0].clockZone == ClockZone::Tai);
    CHECK(parseLayout(body("sundial", "utc"), layout, 4).has_value());
    CHECK(parseLayout(body("digital", "mars"), layout, 4).has_value());
}

TEST_CASE("audio zones must rise towards full scale")
{
    auto const body = [](int green, int amber) {
        return "{\"version\":1,\"name\":\"z\",\"tiles\":[{\"id\":\"a\",\"input\":1,\"zone_green\":" + std::to_string(green) + ",\"zone_amber\":" +
               std::to_string(amber) + ",\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}";
    };
    Layout layout;
    CHECK_FALSE(parseLayout(body(-20, -10), layout, 4).has_value());
    CHECK(layout.tiles[0].zoneGreen == doctest::Approx(-20));
    CHECK(parseLayout(body(-9, -18), layout, 4).has_value());
    CHECK(parseLayout(body(-18, 3), layout, 4).has_value());
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
