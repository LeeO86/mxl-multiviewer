#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "layout/geometry.hpp"
#include "layout/migrate.hpp"
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

TEST_CASE("umd_align is left, centre, or right; a saved tile without it stays left")
{
    auto const body = [](std::string const& align) {
        return "{\"version\":1,\"name\":\"u\",\"tiles\":[{\"id\":\"a\",\"input\":1" + align + ",\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}";
    };
    Layout layout;
    REQUIRE_FALSE(parseLayout(body(""), layout, 4).has_value());
    CHECK(layout.tiles[0].umdAlign == UmdAlign::Left);
    REQUIRE_FALSE(parseLayout(body(",\"umd_align\":\"centre\""), layout, 4).has_value());
    CHECK(layout.tiles[0].umdAlign == UmdAlign::Centre);
    CHECK(layoutToJson(layout).find("\"umd_align\":\"centre\"") != std::string::npos);
    REQUIRE_FALSE(parseLayout(body(",\"umd_align\":\"right\""), layout, 4).has_value());
    CHECK(layout.tiles[0].umdAlign == UmdAlign::Right);
    CHECK(parseLayout(body(",\"umd_align\":\"middle\""), layout, 4).has_value());
    CHECK(parseLayout(body(",\"umd_align\":\"center\""), layout, 4).has_value());
    // A 1.1.x preset with another alignment counts as edited.
    auto const old = legacyPresets(16);
    Layout edited = old[1];
    edited.tiles[0].umdAlign = UmdAlign::Right;
    CHECK_FALSE(sameLayout(old[1], edited));
}

TEST_CASE("alarm display options: border and labels on by default, label positions checked")
{
    auto const body = [](std::string const& options) {
        return "{\"version\":1,\"name\":\"a\",\"tiles\":[{\"id\":\"a\",\"input\":1" + options + ",\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}";
    };
    Layout layout;
    REQUIRE_FALSE(parseLayout(body(""), layout, 4).has_value());
    CHECK(layout.tiles[0].alarmBorder);
    CHECK(layout.tiles[0].alarmLabels);
    CHECK(layout.tiles[0].alarmLabelPosition == AlarmLabelPosition::Top);
    REQUIRE_FALSE(parseLayout(body(",\"alarm_border\":false,\"alarm_labels\":true,\"alarm_label_position\":\"bottom-right\""), layout, 4).has_value());
    CHECK_FALSE(layout.tiles[0].alarmBorder);
    CHECK(layout.tiles[0].alarmLabelPosition == AlarmLabelPosition::BottomRight);
    auto const json = layoutToJson(layout);
    CHECK(json.find("\"alarm_border\":false,\"alarm_labels\":true,\"alarm_label_position\":\"bottom-right\"") != std::string::npos);
    Layout back;
    REQUIRE_FALSE(parseLayout(json, back, 4).has_value());
    CHECK(back.tiles[0].alarmLabelPosition == AlarmLabelPosition::BottomRight);
    for (auto const* position : {"top-left", "top", "top-right", "bottom-left", "bottom", "bottom-right"})
    {
        CHECK_FALSE(parseLayout(body(std::string(",\"alarm_label_position\":\"") + position + "\""), layout, 4).has_value());
    }
    CHECK(parseLayout(body(",\"alarm_label_position\":\"middle\""), layout, 4).has_value());
    auto const old = legacyPresets(16);
    Layout edited = old[1];
    edited.tiles[0].alarmLabels = false;
    CHECK_FALSE(sameLayout(old[1], edited));
}

TEST_CASE("audio_bar_scale is on unless a tile turns it off")
{
    Layout layout;
    REQUIRE_FALSE(parseLayout(R"({"version":1,"name":"s","tiles":[{"id":"a","input":1,"rect":{"x":0,"y":0,"w":1,"h":1}}]})", layout, 4).has_value());
    CHECK(layout.tiles[0].audioBarScale);
    REQUIRE_FALSE(
        parseLayout(R"({"version":1,"name":"s","tiles":[{"id":"a","input":1,"audio_bar_scale":false,"rect":{"x":0,"y":0,"w":1,"h":1}}]})", layout, 4).has_value());
    CHECK_FALSE(layout.tiles[0].audioBarScale);
    CHECK(layoutToJson(layout).find("\"audio_bar_scale\":false") != std::string::npos);
    for (auto const& preset : builtinPresets(16))
    {
        CHECK(preset.tiles[0].audioBarScale);
    }
}

TEST_CASE("image tiles show an http(s) URL or a stored picture")
{
    auto const body = [](std::string const& fields) {
        return "{\"version\":1,\"name\":\"i\",\"tiles\":[{\"id\":\"a\",\"content\":\"image\"" + fields + ",\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}";
    };
    Layout layout;
    REQUIRE_FALSE(parseLayout(body(",\"image_url\":\"https://example.org/logo.png\",\"scale\":\"fill\""), layout, 4).has_value());
    CHECK(layout.tiles[0].content == TileContent::Image);
    CHECK(layout.tiles[0].imageUrl == "https://example.org/logo.png");
    CHECK(layout.tiles[0].scale == ScaleMode::Fill);
    auto const json = layoutToJson(layout);
    CHECK(json.find("\"content\":\"image\"") != std::string::npos);
    CHECK(json.find("\"image_url\":\"https://example.org/logo.png\",\"image_file\":\"\"") != std::string::npos);
    REQUIRE_FALSE(parseLayout(body(",\"image_file\":\"bug.gif\""), layout, 4).has_value());
    CHECK(layout.tiles[0].imageFile == "bug.gif");
    // Not set yet: allowed, the tile says so on the wall.
    CHECK_FALSE(parseLayout(body(""), layout, 4).has_value());
    CHECK(parseLayout(body(",\"image_url\":\"file:///etc/passwd\""), layout, 4).has_value());
    CHECK(parseLayout(body(",\"image_url\":\"ftp://example.org/a.png\""), layout, 4).has_value());
    CHECK(parseLayout(body(",\"image_file\":\"../config.json\""), layout, 4).has_value());
    CHECK(parseLayout(body(",\"image_url\":\"https://example.org/a.png\",\"image_file\":\"a.png\""), layout, 4).has_value());
    // Layouts without image tiles look as before; a 1.1.x preset with a URL counts as edited.
    auto const old = legacyPresets(16);
    Layout edited = old[1];
    edited.tiles[0].imageUrl = "https://example.org/a.png";
    CHECK_FALSE(sameLayout(old[1], edited));
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

TEST_CASE("audio bars stay within the 16 metered channels")
{
    auto const body = [](int first, int channels) {
        return "{\"version\":1,\"name\":\"b\",\"tiles\":[{\"id\":\"a\",\"input\":1,\"audio_bar_first\":" + std::to_string(first) +
               ",\"audio_bar_channels\":" + std::to_string(channels) + ",\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}";
    };
    Layout layout;
    CHECK_FALSE(parseLayout(body(14, 2), layout, 4).has_value());
    CHECK(parseLayout(body(15, 2), layout, 4).has_value());
    CHECK(parseLayout(body(40, 1), layout, 4).has_value());
    CHECK(parseLayout(body(0, 17), layout, 4).has_value());
}

TEST_CASE("layouts an older release saved load with repairs")
{
    // 1.1.x stored zones in any order, any bar channel, and kept unknown clock values out of
    // the file only by accident of its own writer; a hand-edited file may still have them.
    auto const body = std::string("{\"version\":1,\"active\":\"old\",\"layouts\":[{\"version\":1,\"name\":\"old\",\"tiles\":["
                                  "{\"id\":\"a\",\"input\":1,\"zone_green\":-6,\"zone_amber\":-20,\"audio_bar_first\":40,\"rect\":{\"x\":0,\"y\":0,\"w\":0.5,\"h\":1}},"
                                  "{\"id\":\"c\",\"content\":\"clock\",\"clock_style\":\"sundial\",\"clock_zone\":\"mars\",\"rect\":{\"x\":0.5,\"y\":0,\"w\":0.5,\"h\":1}}]}]}");
    LayoutBook strict;
    CHECK(parseBook(body, strict, 4).has_value());
    LayoutBook book;
    std::vector<std::string> repairs;
    REQUIRE_FALSE(parseBook(body, book, 4, &repairs).has_value());
    CHECK(repairs.size() == 4);
    auto const& tile = book.layouts[0].tiles[0];
    CHECK(tile.zoneGreen == doctest::Approx(-20));
    CHECK(tile.zoneAmber == doctest::Approx(-6));
    CHECK(tile.audioBarFirst + tile.audioBarChannels <= 16);
    CHECK(book.layouts[0].tiles[1].clockStyle == ClockStyle::Digital);
    CHECK(book.layouts[0].tiles[1].clockZone == ClockZone::Utc);
    CHECK_FALSE(validateLayout(book.layouts[0], 4).has_value());
}

TEST_CASE("the 1.1.x presets are today's presets without audio bars")
{
    for (int inputs : {1, 4, 9, 16, 32})
    {
        auto const old = legacyPresets(inputs);
        auto now = builtinPresets(inputs);
        REQUIRE(old.size() == now.size());
        for (auto& layout : now)
        {
            for (auto& tile : layout.tiles)
            {
                tile.audioBars = false;
            }
        }
        for (std::size_t i = 0; i < old.size(); ++i)
        {
            CHECK(sameLayout(old[i], now[i]));
        }
    }
}

TEST_CASE("unedited 1.1.x presets get today's defaults once, edited ones stay")
{
    LayoutBook book;
    book.presetRevision = 1;
    book.layouts = legacyPresets(16);
    // Through JSON, as a 1.1.x file stores the rects: six significant digits.
    LayoutBook loaded;
    REQUIRE_FALSE(parseBook(bookToJson(book), loaded, 16).has_value());
    CHECK(loaded.presetRevision == 1);
    loaded.layouts[3].tiles[0].umdText = "CAM 1"; // an edited 4x4
    Layout copy = loaded.layouts[1];
    copy.name = "my wall"; // an unedited 2x2 under another name
    loaded.layouts.push_back(copy);
    auto const migrated = migratePresets(loaded, 16);
    CHECK(migrated == std::vector<std::string>{"1", "2x2", "3x3", "5x5", "2+8", "1+5", "1+7", "2+6"});
    CHECK(loaded.layouts[1].tiles[0].audioBars);
    CHECK_FALSE(loaded.layouts[3].tiles[0].audioBars);
    CHECK(loaded.layouts[3].tiles[0].umdText == "CAM 1");
    CHECK_FALSE(loaded.layouts.back().tiles[0].audioBars);
    CHECK(loaded.presetRevision == kPresetRevision);
    // A second pass changes nothing, even if bars are switched off again by hand.
    loaded.layouts[1].tiles[0].audioBars = false;
    CHECK(migratePresets(loaded, 16).empty());
    CHECK_FALSE(loaded.layouts[1].tiles[0].audioBars);
    // A file written with another MV_MAX_INPUTS is still recognised.
    LayoutBook fewer;
    fewer.presetRevision = 1;
    fewer.layouts = legacyPresets(8);
    CHECK(migratePresets(fewer, 16).size() == 9);
    CHECK(fewer.layouts[3].tiles.size() == 16);
    // A book without preset_revision is from 1.1.x.
    LayoutBook bare;
    REQUIRE_FALSE(parseBook(R"({"version":1,"active":"1","layouts":[]})", bare, 16).has_value());
    CHECK(bare.presetRevision == 1);
}

TEST_CASE("the book keeps the layout chosen for each head")
{
    LayoutBook book = defaultBook(16, "2x2");
    book.heads = {{1, "3x3"}, {2, "4x4"}};
    LayoutBook back;
    REQUIRE_FALSE(parseBook(bookToJson(book), back, 16).has_value());
    CHECK(back.heads == book.heads);
    CHECK(back.presetRevision == kPresetRevision);
}

TEST_CASE("tally_text defaults from the layout and a tile can override it")
{
    auto const body = [](std::string const& layoutValue) {
        return "{\"version\":1,\"name\":\"t\"" + layoutValue +
               ",\"tiles\":["
               "{\"id\":\"a\",\"input\":1,\"rect\":{\"x\":0,\"y\":0,\"w\":0.25,\"h\":1}},"
               "{\"id\":\"b\",\"input\":1,\"tally_text\":null,\"rect\":{\"x\":0.25,\"y\":0,\"w\":0.25,\"h\":1}},"
               "{\"id\":\"c\",\"input\":1,\"tally_text\":true,\"rect\":{\"x\":0.5,\"y\":0,\"w\":0.25,\"h\":1}},"
               "{\"id\":\"d\",\"input\":1,\"tally_text\":false,\"rect\":{\"x\":0.75,\"y\":0,\"w\":0.25,\"h\":1}}]}";
    };
    auto const shown = [](Layout const& layout) {
        std::string out;
        for (auto const& tile : layout.tiles)
        {
            out += tallyTextOn(layout, tile) ? '1' : '0';
        }
        return out;
    };
    // Off unless the layout (the head that shows it) or the tile turns it on.
    Layout off;
    REQUIRE_FALSE(parseLayout(body(""), off, 4).has_value());
    CHECK_FALSE(off.tallyText);
    CHECK_FALSE(off.tiles[0].tallyText.has_value());
    CHECK_FALSE(off.tiles[1].tallyText.has_value());
    CHECK(shown(off) == "0010");
    Layout on;
    REQUIRE_FALSE(parseLayout(body(",\"tally_text\":true"), on, 4).has_value());
    CHECK(on.tallyText);
    CHECK(shown(on) == "1110");

    // Saved as written: the layout value, each override, and null for "follow the layout".
    auto const json = layoutToJson(on);
    CHECK(json.find("\"background\":\"#101010\",\"tally_text\":true") != std::string::npos);
    CHECK(json.find("\"tally_lamp\":true,\"tally_text\":null") != std::string::npos);
    off.name = "u";
    LayoutBook book;
    book.active = "t";
    book.layouts = {on, off};
    LayoutBook back;
    REQUIRE_FALSE(parseBook(bookToJson(book), back, 4).has_value());
    CHECK(shown(back.layouts[0]) == "1110");
    CHECK(shown(back.layouts[1]) == "0010");
    CHECK_FALSE(back.layouts[0].tiles[0].tallyText.has_value());
    CHECK(back.layouts[0].tiles[3].tallyText == false);

    // The API rejects other values; a layout file is repaired.
    Layout bad;
    CHECK(parseLayout(body(",\"tally_text\":\"yes\""), bad, 4).has_value());
    auto const badTile = std::string("{\"version\":1,\"name\":\"x\",\"tiles\":[{\"id\":\"a\",\"tally_text\":1,\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}");
    CHECK(parseLayout(badTile, bad, 4).has_value());
    std::vector<std::string> repairs;
    REQUIRE_FALSE(parseLayout(badTile, bad, 4, &repairs).has_value());
    CHECK(repairs.size() == 1);
    CHECK_FALSE(bad.tiles[0].tallyText.has_value());

    // Presets leave it off; a 1.1.x preset with it set counts as edited.
    for (auto const& layout : builtinPresets(16))
    {
        CHECK(shown(layout).find('1') == std::string::npos);
    }
    auto const old = legacyPresets(16);
    Layout edited = old[1];
    edited.tallyText = true;
    CHECK_FALSE(sameLayout(old[1], edited));
    edited = old[1];
    edited.tiles[0].tallyText = false;
    CHECK_FALSE(sameLayout(old[1], edited));
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

TEST_CASE("audio bars beside the picture take a strip of the tile; over the picture they do not")
{
    // 1080 canvas, 960 px tile, 2 channels: margins 6 + 6, bars 2 × 14 + 2, gap 2, tick 4, labels 28.
    CHECK(barsStripWidth(960, 2, true, 1080) == 76);
    CHECK(barsStripWidth(960, 2, false, 1080) == 44);
    for (int channels = 1; channels <= 16; ++channels)
    {
        for (int const height : {540, 720, 1080, 2160})
        {
            CHECK(barsStripWidth(960, channels, true, height) % 2 == 0);
        }
    }
    Tile tile;
    tile.audioBars = true;
    tile.audioBarPosition = BarsPosition::RightBeside;
    PixelRect const px{960, 0, 960, 540};
    auto const right = pictureRect(px, tile, 1080);
    CHECK(right.x == 960);
    CHECK(right.w == 884);
    CHECK(right.h == 540);
    tile.audioBarPosition = BarsPosition::LeftBeside;
    auto const left = pictureRect(px, tile, 1080);
    CHECK(left.x == 1036);
    CHECK(left.w == 884);
    // A 16:9 source fits into the rest, letterboxed; the strip stays free.
    auto const placed = placeTile(left, 1920, 1080, ScaleMode::Fit);
    CHECK(placed.dst.x >= 1036);
    CHECK(placed.dst.x % 2 == 0);
    CHECK(placed.dst.w == 884);
    CHECK(placed.dst.h == 497);
    // Over the picture, bars off, other content, or a tile too narrow for the strip: the whole tile.
    for (auto const position : {BarsPosition::Left, BarsPosition::Right, BarsPosition::Overlay})
    {
        tile.audioBarPosition = position;
        CHECK(pictureRect(px, tile, 1080).w == 960);
    }
    tile.audioBarPosition = BarsPosition::RightBeside;
    tile.audioBars = false;
    CHECK(pictureRect(px, tile, 1080).w == 960);
    tile.audioBars = true;
    tile.content = TileContent::Clock;
    CHECK(pictureRect(px, tile, 1080).w == 960);
    tile.content = TileContent::Input;
    CHECK(barsStripWidth(100, 2, true, 1080) == 0);
    CHECK(pictureRect(PixelRect{0, 0, 100, 56}, tile, 1080).w == 100);

    // The positions parse and round-trip; older layouts keep left, right, and overlay.
    Layout layout;
    REQUIRE_FALSE(parseLayout(R"({"version":1,"name":"b","tiles":[{"id":"a","input":1,"audio_bar_position":"left-beside","rect":{"x":0,"y":0,"w":1,"h":1}}]})", layout, 4)
                      .has_value());
    CHECK(layout.tiles[0].audioBarPosition == BarsPosition::LeftBeside);
    CHECK(layoutToJson(layout).find("\"audio_bar_position\":\"left-beside\"") != std::string::npos);
    CHECK(parseLayout(R"({"version":1,"name":"b","tiles":[{"id":"a","input":1,"audio_bar_position":"beside","rect":{"x":0,"y":0,"w":1,"h":1}}]})", layout, 4).has_value());
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
