#include "layout/model.hpp"

#include "media/image.hpp"
#include "util/fetch.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>
#include <utility>

namespace mv
{
namespace
{
char const* contentToString(TileContent content)
{
    switch (content)
    {
    case TileContent::Clock:
        return "clock";
    case TileContent::Label:
        return "label";
    case TileContent::Empty:
        return "empty";
    case TileContent::Image:
        return "image";
    case TileContent::Input:
        return "input";
    }
    return "input";
}

std::optional<TileContent> contentFromString(std::string const& text)
{
    if (text == "input")
    {
        return TileContent::Input;
    }
    if (text == "clock")
    {
        return TileContent::Clock;
    }
    if (text == "label")
    {
        return TileContent::Label;
    }
    if (text == "empty")
    {
        return TileContent::Empty;
    }
    if (text == "image")
    {
        return TileContent::Image;
    }
    return std::nullopt;
}

char const* scaleToString(ScaleMode mode)
{
    return mode == ScaleMode::Fill ? "fill" : "fit";
}

std::optional<ScaleMode> scaleFromString(std::string const& text)
{
    if (text == "fit")
    {
        return ScaleMode::Fit;
    }
    if (text == "fill")
    {
        return ScaleMode::Fill;
    }
    return std::nullopt;
}

char const* umdSourceToString(UmdSource source)
{
    switch (source)
    {
    case UmdSource::Manual:
        return "manual";
    case UmdSource::Tsl:
        return "tsl";
    case UmdSource::Is04:
        return "is04";
    }
    return "is04";
}

std::optional<UmdSource> umdSourceFromString(std::string const& text)
{
    if (text == "is04")
    {
        return UmdSource::Is04;
    }
    if (text == "manual")
    {
        return UmdSource::Manual;
    }
    if (text == "tsl")
    {
        return UmdSource::Tsl;
    }
    return std::nullopt;
}

char const* umdPosToString(UmdPosition pos)
{
    switch (pos)
    {
    case UmdPosition::TopInside:
        return "top-inside";
    case UmdPosition::TopOutside:
        return "top-outside";
    case UmdPosition::BottomOutside:
        return "bottom-outside";
    case UmdPosition::BottomInside:
        return "bottom-inside";
    }
    return "bottom-inside";
}

std::optional<UmdPosition> umdPosFromString(std::string const& text)
{
    if (text == "top-inside")
    {
        return UmdPosition::TopInside;
    }
    if (text == "top-outside")
    {
        return UmdPosition::TopOutside;
    }
    if (text == "bottom-inside")
    {
        return UmdPosition::BottomInside;
    }
    if (text == "bottom-outside")
    {
        return UmdPosition::BottomOutside;
    }
    return std::nullopt;
}

char const* umdAlignToString(UmdAlign align)
{
    return align == UmdAlign::Centre ? "centre" : align == UmdAlign::Right ? "right" : "left";
}

std::optional<UmdAlign> umdAlignFromString(std::string const& text)
{
    if (text == "left")
    {
        return UmdAlign::Left;
    }
    if (text == "centre")
    {
        return UmdAlign::Centre;
    }
    if (text == "right")
    {
        return UmdAlign::Right;
    }
    return std::nullopt;
}

constexpr std::pair<AlarmLabelPosition, char const*> kAlarmLabelPositions[] = {{AlarmLabelPosition::TopLeft, "top-left"}, {AlarmLabelPosition::Top, "top"},
    {AlarmLabelPosition::TopRight, "top-right"}, {AlarmLabelPosition::BottomLeft, "bottom-left"}, {AlarmLabelPosition::Bottom, "bottom"},
    {AlarmLabelPosition::BottomRight, "bottom-right"}};

char const* alarmLabelPositionToString(AlarmLabelPosition pos)
{
    for (auto const& [value, name] : kAlarmLabelPositions)
    {
        if (value == pos)
        {
            return name;
        }
    }
    return "top";
}

std::optional<AlarmLabelPosition> alarmLabelPositionFromString(std::string const& text)
{
    for (auto const& [value, name] : kAlarmLabelPositions)
    {
        if (text == name)
        {
            return value;
        }
    }
    return std::nullopt;
}

char const* barsToString(BarsPosition pos)
{
    switch (pos)
    {
    case BarsPosition::Left:
        return "left";
    case BarsPosition::Overlay:
        return "overlay";
    case BarsPosition::LeftBeside:
        return "left-beside";
    case BarsPosition::RightBeside:
        return "right-beside";
    case BarsPosition::Right:
        return "right";
    }
    return "right";
}

std::optional<BarsPosition> barsFromString(std::string const& text)
{
    if (text == "left")
    {
        return BarsPosition::Left;
    }
    if (text == "right")
    {
        return BarsPosition::Right;
    }
    if (text == "overlay")
    {
        return BarsPosition::Overlay;
    }
    if (text == "left-beside")
    {
        return BarsPosition::LeftBeside;
    }
    if (text == "right-beside")
    {
        return BarsPosition::RightBeside;
    }
    return std::nullopt;
}

std::optional<ClockStyle> clockStyleFromString(std::string const& text)
{
    if (text == "digital")
    {
        return ClockStyle::Digital;
    }
    // "analog" is accepted as the US spelling of "analogue".
    if (text == "analogue" || text == "analog")
    {
        return ClockStyle::Analogue;
    }
    return std::nullopt;
}

std::optional<ClockZone> clockZoneFromString(std::string const& text)
{
    if (text == "utc")
    {
        return ClockZone::Utc;
    }
    if (text == "tai")
    {
        return ClockZone::Tai;
    }
    if (text == "local")
    {
        return ClockZone::Local;
    }
    return std::nullopt;
}

std::string quoted(std::string const& text)
{
    return "\"" + jsonEscape(text) + "\"";
}

bool rectOk(NormRect const& rect)
{
    auto const finite = [](double v) { return std::isfinite(v); };
    if (!finite(rect.x) || !finite(rect.y) || !finite(rect.w) || !finite(rect.h))
    {
        return false;
    }
    if (rect.x < -1e-9 || rect.y < -1e-9 || rect.w <= 0 || rect.h <= 0)
    {
        return false;
    }
    if (rect.x + rect.w > 1.0 + 1e-6 || rect.y + rect.h > 1.0 + 1e-6)
    {
        return false;
    }
    return true;
}

double num(picojson::object const& obj, char const* key, double fallback)
{
    auto const it = obj.find(key);
    if (it == obj.end() || !it->second.is<double>())
    {
        return fallback;
    }
    return it->second.get<double>();
}

bool flag(picojson::object const& obj, char const* key, bool fallback)
{
    auto const it = obj.find(key);
    if (it == obj.end() || !it->second.is<bool>())
    {
        return fallback;
    }
    return it->second.get<bool>();
}

std::string str(picojson::object const& obj, char const* key, std::string const& fallback = {})
{
    auto const it = obj.find(key);
    if (it == obj.end() || !it->second.is<std::string>())
    {
        return fallback;
    }
    return it->second.get<std::string>();
}

Tile gridTile(int index, int cols, int rows)
{
    int const col = index % cols;
    int const row = index / cols;
    Tile tile;
    tile.id = "t" + std::to_string(index + 1);
    tile.content = TileContent::Input;
    tile.input = index + 1;
    tile.z = index;
    tile.rect = {static_cast<double>(col) / cols, static_cast<double>(row) / rows, 1.0 / cols, 1.0 / rows};
    return tile;
}

Layout makeGrid(std::string name, int cols, int rows, int maxInputs)
{
    Layout layout;
    layout.name = std::move(name);
    int const count = std::min(cols * rows, maxInputs);
    for (int i = 0; i < count; ++i)
    {
        layout.tiles.push_back(gridTile(i, cols, rows));
    }
    return layout;
}
} // namespace

char const* contentName(TileContent content)
{
    return contentToString(content);
}

char const* scaleName(ScaleMode mode)
{
    return scaleToString(mode);
}

bool tallyTextOn(Layout const& layout, Tile const& tile)
{
    return tile.tallyText.value_or(layout.tallyText);
}

std::vector<Layout> builtinPresets(int maxInputs)
{
    std::vector<Layout> layouts;
    layouts.push_back(makeGrid("1", 1, 1, maxInputs));
    layouts.push_back(makeGrid("2x2", 2, 2, maxInputs));
    layouts.push_back(makeGrid("3x3", 3, 3, maxInputs));
    layouts.push_back(makeGrid("4x4", 4, 4, maxInputs));
    layouts.push_back(makeGrid("5x5", 5, 5, maxInputs));

    if (maxInputs >= 1)
    {
        Layout twoPlusEight;
        twoPlusEight.name = "2+8";
        for (int i = 0; i < std::min(2, maxInputs); ++i)
        {
            Tile tile;
            tile.id = "t" + std::to_string(i + 1);
            tile.input = i + 1;
            tile.z = i;
            tile.rect = {0, i * 0.5, 0.5, 0.5};
            twoPlusEight.tiles.push_back(tile);
        }
        for (int i = 0; i < 8 && (i + 2) < maxInputs; ++i)
        {
            Tile tile;
            tile.id = "t" + std::to_string(i + 3);
            tile.input = i + 3;
            tile.z = i + 2;
            int const col = i % 2;
            int const row = i / 2;
            tile.rect = {0.5 + col * 0.25, row * 0.25, 0.25, 0.25};
            twoPlusEight.tiles.push_back(tile);
        }
        layouts.push_back(twoPlusEight);

        Layout onePlusFive;
        onePlusFive.name = "1+5";
        Tile main;
        main.id = "t1";
        main.input = 1;
        main.rect = {0, 0, 2.0 / 3.0, 1};
        onePlusFive.tiles.push_back(main);
        for (int i = 0; i < 5 && (i + 1) < maxInputs; ++i)
        {
            Tile tile;
            tile.id = "t" + std::to_string(i + 2);
            tile.input = i + 2;
            tile.z = i + 1;
            tile.rect = {2.0 / 3.0, i / 5.0, 1.0 / 3.0, 0.2};
            onePlusFive.tiles.push_back(tile);
        }
        layouts.push_back(onePlusFive);

        Layout onePlusSeven;
        onePlusSeven.name = "1+7";
        Tile big = main;
        big.rect = {0, 0, 0.75, 0.75};
        onePlusSeven.tiles.push_back(big);
        int next = 2;
        for (int row = 0; row < 4 && next <= maxInputs; ++row, ++next)
        {
            Tile tile;
            tile.id = "t" + std::to_string(next);
            tile.input = next;
            tile.z = next;
            tile.rect = {0.75, row * 0.25, 0.25, 0.25};
            onePlusSeven.tiles.push_back(tile);
        }
        for (int col = 0; col < 3 && next <= maxInputs; ++col, ++next)
        {
            Tile tile;
            tile.id = "t" + std::to_string(next);
            tile.input = next;
            tile.z = next;
            tile.rect = {col * 0.25, 0.75, 0.25, 0.25};
            onePlusSeven.tiles.push_back(tile);
        }
        layouts.push_back(onePlusSeven);

        Layout twoPlusSix;
        twoPlusSix.name = "2+6";
        for (int i = 0; i < 2 && i < maxInputs; ++i)
        {
            Tile tile;
            tile.id = "t" + std::to_string(i + 1);
            tile.input = i + 1;
            tile.z = i;
            tile.rect = {i * 0.5, 0, 0.5, 0.5};
            twoPlusSix.tiles.push_back(tile);
        }
        for (int i = 0; i < 6 && (i + 2) < maxInputs; ++i)
        {
            Tile tile;
            tile.id = "t" + std::to_string(i + 3);
            tile.input = i + 3;
            tile.z = i + 2;
            tile.rect = {i / 6.0, 0.5, 1.0 / 6.0, 0.5};
            twoPlusSix.tiles.push_back(tile);
        }
        layouts.push_back(twoPlusSix);
    }
    // Presets show audio bars (two channels from the first, on the right). A tile that
    // leaves out `audio_bars` still parses as false, so saved layouts keep their value.
    for (auto& layout : layouts)
    {
        for (auto& tile : layout.tiles)
        {
            tile.audioBars = tile.content == TileContent::Input;
        }
    }
    return layouts;
}

LayoutBook defaultBook(int maxInputs, std::string const& active)
{
    LayoutBook book;
    book.active = active;
    book.layouts = builtinPresets(maxInputs);
    bool found = false;
    for (auto const& layout : book.layouts)
    {
        if (layout.name == active)
        {
            found = true;
        }
    }
    if (!found && !book.layouts.empty())
    {
        book.active = book.layouts.front().name;
    }
    return book;
}

std::optional<std::string> validateLayout(Layout const& layout, int maxInputs)
{
    if (layout.version != 1)
    {
        return "layout version must be 1";
    }
    if (layout.name.empty())
    {
        return "layout name is empty";
    }
    std::set<std::string> ids;
    for (auto const& tile : layout.tiles)
    {
        if (tile.id.empty() || !ids.insert(tile.id).second)
        {
            return "tile ids must be unique and non-empty";
        }
        if (!rectOk(tile.rect))
        {
            return "tile " + tile.id + " rect is outside the canvas";
        }
        if (tile.content == TileContent::Input && (tile.input < 1 || tile.input > maxInputs))
        {
            return "tile " + tile.id + " input is out of range";
        }
        // 16 channels per input are metered (§5.7).
        if (tile.audioBarChannels < 1 || tile.audioBarFirst < 0 || tile.audioBarFirst + tile.audioBarChannels > 16)
        {
            return "tile " + tile.id + " audio bars must stay within channels 1-16";
        }
        if (tile.umdFont < 8 || tile.umdFont > 200)
        {
            return "tile " + tile.id + " font size is invalid";
        }
        if (!(tile.zoneGreen >= -60 && tile.zoneGreen <= tile.zoneAmber && tile.zoneAmber <= 0))
        {
            return "tile " + tile.id + " audio zones must satisfy -60 <= zone_green <= zone_amber <= 0";
        }
        for (auto const& marker : tile.aspectMarkers)
        {
            if (marker != "16:9" && marker != "4:3" && marker != "1:1" && marker != "9:16")
            {
                return "tile " + tile.id + " has an unknown aspect marker";
            }
        }
        if (!tile.imageUrl.empty() && !httpUrl(tile.imageUrl))
        {
            return "tile " + tile.id + " image_url must be an http or https URL";
        }
        if (!tile.imageFile.empty() && !imageName(tile.imageFile))
        {
            return "tile " + tile.id + " image_file is not a stored picture name";
        }
        if (tile.content == TileContent::Image && !tile.imageUrl.empty() && !tile.imageFile.empty())
        {
            return "tile " + tile.id + " sets image_url and image_file; an image tile shows one of them";
        }
    }
    return std::nullopt;
}

std::optional<std::string> parseLayout(std::string const& body, Layout& out, int maxInputs, std::vector<std::string>* repairs)
{
    std::string error;
    auto const root = json::parse(body, error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        return "layout JSON is invalid";
    }
    auto const& obj = root.get<picojson::object>();
    Layout layout;
    layout.version = static_cast<int>(num(obj, "version", 1));
    layout.name = str(obj, "name");
    layout.background = str(obj, "background", "#101010");
    if (auto const it = obj.find("tally_text"); it != obj.end() && !it->second.is<bool>())
    {
        if (repairs == nullptr)
        {
            return "layout tally_text must be true or false";
        }
        repairs->push_back("layout " + layout.name + ": tally_text set to false");
    }
    layout.tallyText = flag(obj, "tally_text", false);
    auto const tiles = obj.find("tiles");
    if (tiles == obj.end() || !tiles->second.is<picojson::array>())
    {
        return "layout tiles must be an array";
    }
    for (auto const& item : tiles->second.get<picojson::array>())
    {
        if (!item.is<picojson::object>())
        {
            return "tile is not an object";
        }
        auto const& tileObj = item.get<picojson::object>();
        Tile tile;
        tile.id = str(tileObj, "id");
        auto const content = contentFromString(str(tileObj, "content", "input"));
        if (!content)
        {
            return "tile content is invalid";
        }
        tile.content = *content;
        tile.input = static_cast<int>(num(tileObj, "input", 1));
        tile.z = static_cast<int>(num(tileObj, "z", 0));
        auto const rectIt = tileObj.find("rect");
        if (rectIt == tileObj.end() || !rectIt->second.is<picojson::object>())
        {
            return "tile rect is missing";
        }
        auto const& rect = rectIt->second.get<picojson::object>();
        tile.rect = {num(rect, "x", 0), num(rect, "y", 0), num(rect, "w", 0), num(rect, "h", 0)};
        auto const scale = scaleFromString(str(tileObj, "scale", "fit"));
        if (!scale)
        {
            return "tile scale is invalid";
        }
        tile.scale = *scale;
        tile.umd = flag(tileObj, "umd", true);
        auto const source = umdSourceFromString(str(tileObj, "umd_source", "is04"));
        if (!source)
        {
            return "umd_source is invalid";
        }
        tile.umdSource = *source;
        tile.umdText = str(tileObj, "umd_text");
        auto const pos = umdPosFromString(str(tileObj, "umd_position", "bottom-inside"));
        if (!pos)
        {
            return "umd_position is invalid";
        }
        tile.umdPosition = *pos;
        auto const align = umdAlignFromString(str(tileObj, "umd_align", "left"));
        if (!align)
        {
            return "umd_align is invalid";
        }
        tile.umdAlign = *align;
        tile.umdFont = static_cast<int>(num(tileObj, "umd_font", 28));
        tile.umdBg = str(tileObj, "umd_bg", "#000000c0");
        tile.tallyBorder = flag(tileObj, "tally_border", true);
        tile.tallyLamp = flag(tileObj, "tally_lamp", true);
        // true or false overrides the layout; null or missing follows it.
        if (auto const it = tileObj.find("tally_text"); it != tileObj.end() && it->second.is<bool>())
        {
            tile.tallyText = it->second.get<bool>();
        }
        else if (it != tileObj.end() && !it->second.is<picojson::null>())
        {
            if (repairs == nullptr)
            {
                return "tally_text must be true, false, or null";
            }
            repairs->push_back("layout " + layout.name + " tile " + tile.id + ": tally_text follows the layout");
        }
        tile.audioBars = flag(tileObj, "audio_bars", false);
        tile.audioBarRms = flag(tileObj, "audio_bar_rms", false);
        tile.audioBarScale = flag(tileObj, "audio_bar_scale", true);
        tile.audioBarChannels = static_cast<int>(num(tileObj, "audio_bar_channels", 2));
        tile.audioBarFirst = static_cast<int>(num(tileObj, "audio_bar_first", 0));
        auto const bars = barsFromString(str(tileObj, "audio_bar_position", "right"));
        if (!bars)
        {
            return "audio_bar_position is invalid";
        }
        tile.audioBarPosition = *bars;
        tile.zoneGreen = num(tileObj, "zone_green", -18);
        tile.zoneAmber = num(tileObj, "zone_amber", -9);
        if (repairs != nullptr)
        {
            auto const where = "layout " + layout.name + " tile " + tile.id + ": ";
            if (!(tile.zoneGreen >= -60 && tile.zoneGreen <= tile.zoneAmber && tile.zoneAmber <= 0))
            {
                double const a = std::isfinite(tile.zoneGreen) ? std::clamp(tile.zoneGreen, -60.0, 0.0) : -18.0;
                double const b = std::isfinite(tile.zoneAmber) ? std::clamp(tile.zoneAmber, -60.0, 0.0) : -9.0;
                tile.zoneGreen = std::min(a, b);
                tile.zoneAmber = std::max(a, b);
                repairs->push_back(where + "audio zones set to " + std::to_string(static_cast<int>(tile.zoneGreen)) + " / " +
                                   std::to_string(static_cast<int>(tile.zoneAmber)));
            }
            if (tile.audioBarChannels < 1 || tile.audioBarFirst < 0 || tile.audioBarFirst + tile.audioBarChannels > 16)
            {
                tile.audioBarChannels = std::clamp(tile.audioBarChannels, 1, 16);
                tile.audioBarFirst = std::clamp(tile.audioBarFirst, 0, 16 - tile.audioBarChannels);
                repairs->push_back(where + "audio bars set to channels " + std::to_string(tile.audioBarFirst + 1) + "-" +
                                   std::to_string(tile.audioBarFirst + tile.audioBarChannels));
            }
        }
        tile.alarmBorder = flag(tileObj, "alarm_border", true);
        tile.alarmLabels = flag(tileObj, "alarm_labels", true);
        auto const labels = alarmLabelPositionFromString(str(tileObj, "alarm_label_position", "top"));
        if (!labels)
        {
            return "alarm_label_position is invalid";
        }
        tile.alarmLabelPosition = *labels;
        tile.formatLabel = flag(tileObj, "format_label", true);
        tile.latency = flag(tileObj, "latency", false);
        tile.safeArea = flag(tileObj, "safe_area", false);
        tile.centre = flag(tileObj, "centre", false);
        if (auto const markers = tileObj.find("aspect_markers"); markers != tileObj.end() && markers->second.is<picojson::array>())
        {
            for (auto const& marker : markers->second.get<picojson::array>())
            {
                if (marker.is<std::string>())
                {
                    tile.aspectMarkers.push_back(marker.get<std::string>());
                }
            }
        }
        auto const style = clockStyleFromString(str(tileObj, "clock_style", "digital"));
        if (!style && repairs == nullptr)
        {
            return "clock_style is invalid";
        }
        if (!style)
        {
            repairs->push_back("layout " + layout.name + " tile " + tile.id + ": unknown clock_style, digital");
        }
        tile.clockStyle = style.value_or(ClockStyle::Digital);
        auto const zone = clockZoneFromString(str(tileObj, "clock_zone", "utc"));
        if (!zone && repairs == nullptr)
        {
            return "clock_zone is invalid";
        }
        if (!zone)
        {
            repairs->push_back("layout " + layout.name + " tile " + tile.id + ": unknown clock_zone, utc");
        }
        tile.clockZone = zone.value_or(ClockZone::Utc);
        tile.timecodeRate = str(tileObj, "timecode_rate");
        tile.labelText = str(tileObj, "label_text");
        tile.imageUrl = str(tileObj, "image_url");
        tile.imageFile = str(tileObj, "image_file");
        layout.tiles.push_back(std::move(tile));
    }
    if (auto const problem = validateLayout(layout, maxInputs))
    {
        return problem;
    }
    out = std::move(layout);
    return std::nullopt;
}

std::string layoutToJson(Layout const& layout)
{
    std::ostringstream out;
    out << "{\"version\":1,\"name\":" << quoted(layout.name) << ",\"background\":" << quoted(layout.background)
        << ",\"tally_text\":" << (layout.tallyText ? "true" : "false") << ",\"tiles\":[";
    for (std::size_t i = 0; i < layout.tiles.size(); ++i)
    {
        auto const& tile = layout.tiles[i];
        if (i != 0)
        {
            out << ',';
        }
        out << "{\"id\":" << quoted(tile.id) << ",\"content\":\"" << contentToString(tile.content) << "\",\"input\":" << tile.input << ",\"z\":" << tile.z
            << ",\"rect\":{\"x\":" << tile.rect.x << ",\"y\":" << tile.rect.y << ",\"w\":" << tile.rect.w << ",\"h\":" << tile.rect.h << "}"
            << ",\"scale\":\"" << scaleToString(tile.scale) << "\",\"umd\":" << (tile.umd ? "true" : "false") << ",\"umd_source\":\""
            << umdSourceToString(tile.umdSource) << "\",\"umd_text\":" << quoted(tile.umdText) << ",\"umd_position\":\"" << umdPosToString(tile.umdPosition)
            << "\",\"umd_align\":\"" << umdAlignToString(tile.umdAlign) << "\",\"umd_font\":" << tile.umdFont << ",\"umd_bg\":" << quoted(tile.umdBg) << ",\"tally_border\":" << (tile.tallyBorder ? "true" : "false")
            << ",\"tally_lamp\":" << (tile.tallyLamp ? "true" : "false") << ",\"tally_text\":" << (!tile.tallyText ? "null" : *tile.tallyText ? "true" : "false")
            << ",\"audio_bars\":" << (tile.audioBars ? "true" : "false")
            << ",\"audio_bar_rms\":" << (tile.audioBarRms ? "true" : "false") << ",\"audio_bar_scale\":" << (tile.audioBarScale ? "true" : "false")
            << ",\"audio_bar_channels\":" << tile.audioBarChannels << ",\"audio_bar_first\":" << tile.audioBarFirst << ",\"audio_bar_position\":\""
            << barsToString(tile.audioBarPosition) << "\",\"zone_green\":" << tile.zoneGreen << ",\"zone_amber\":" << tile.zoneAmber
            << ",\"alarm_border\":" << (tile.alarmBorder ? "true" : "false") << ",\"alarm_labels\":" << (tile.alarmLabels ? "true" : "false")
            << ",\"alarm_label_position\":\"" << alarmLabelPositionToString(tile.alarmLabelPosition) << '"'
            << ",\"format_label\":" << (tile.formatLabel ? "true" : "false") << ",\"latency\":" << (tile.latency ? "true" : "false")
            << ",\"safe_area\":" << (tile.safeArea ? "true" : "false") << ",\"centre\":" << (tile.centre ? "true" : "false") << ",\"aspect_markers\":[";
        for (std::size_t m = 0; m < tile.aspectMarkers.size(); ++m)
        {
            if (m != 0)
            {
                out << ',';
            }
            out << quoted(tile.aspectMarkers[m]);
        }
        out << "],\"clock_style\":\"" << (tile.clockStyle == ClockStyle::Analogue ? "analogue" : "digital") << "\",\"clock_zone\":\""
            << (tile.clockZone == ClockZone::Tai ? "tai" : tile.clockZone == ClockZone::Local ? "local" : "utc") << "\",\"timecode_rate\":"
            << quoted(tile.timecodeRate) << ",\"label_text\":" << quoted(tile.labelText) << ",\"image_url\":" << quoted(tile.imageUrl)
            << ",\"image_file\":" << quoted(tile.imageFile) << "}";
    }
    out << "]}";
    return out.str();
}

std::optional<std::string> parseBook(std::string const& body, LayoutBook& out, int maxInputs, std::vector<std::string>* repairs)
{
    std::string error;
    auto const root = json::parse(body, error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        return "layout book JSON is invalid";
    }
    auto const& obj = root.get<picojson::object>();
    LayoutBook book;
    book.version = static_cast<int>(num(obj, "version", 1));
    book.active = str(obj, "active", "2x2");
    book.presetRevision = static_cast<int>(num(obj, "preset_revision", 1));
    for (auto const& [field, target] : {std::pair{"heads", &book.heads}, std::pair{"start_layouts", &book.startLayouts}})
    {
        if (auto const heads = obj.find(field); heads != obj.end() && heads->second.is<picojson::object>())
        {
            for (auto const& [key, value] : heads->second.get<picojson::object>())
            {
                int const head = std::atoi(key.c_str());
                if (head >= 1 && head <= 3 && value.is<std::string>())
                {
                    (*target)[head] = value.get<std::string>();
                }
            }
        }
    }
    auto const layouts = obj.find("layouts");
    if (layouts == obj.end() || !layouts->second.is<picojson::array>())
    {
        return "layouts must be an array";
    }
    std::set<std::string> names;
    for (auto const& item : layouts->second.get<picojson::array>())
    {
        Layout layout;
        if (auto const problem = parseLayout(item.serialize(), layout, maxInputs, repairs))
        {
            return problem;
        }
        if (!names.insert(layout.name).second)
        {
            return "duplicate layout name " + layout.name;
        }
        book.layouts.push_back(std::move(layout));
    }
    out = std::move(book);
    return std::nullopt;
}

std::string bookToJson(LayoutBook const& book)
{
    std::ostringstream out;
    auto const heads = [&](std::map<int, std::string> const& names) {
        out << '{';
        bool first = true;
        for (auto const& [head, name] : names)
        {
            out << (first ? "" : ",") << '"' << head << "\":" << quoted(name);
            first = false;
        }
        out << '}';
    };
    out << "{\"version\":1,\"active\":" << quoted(book.active) << ",\"preset_revision\":" << book.presetRevision << ",\"heads\":";
    heads(book.heads);
    out << ",\"start_layouts\":";
    heads(book.startLayouts);
    out << ",\"layouts\":[";
    for (std::size_t i = 0; i < book.layouts.size(); ++i)
    {
        if (i != 0)
        {
            out << ',';
        }
        out << layoutToJson(book.layouts[i]);
    }
    out << "]}";
    return out.str();
}
} // namespace mv
