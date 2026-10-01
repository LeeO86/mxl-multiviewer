#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mv
{
struct NormRect
{
    double x = 0;
    double y = 0;
    double w = 1;
    double h = 1;
};

enum class TileContent
{
    Input,
    Clock,
    Label,
    Empty
};
enum class ScaleMode
{
    Fit,
    Fill
};
enum class UmdSource
{
    Is04,
    Manual,
    Tsl
};
enum class UmdPosition
{
    TopInside,
    TopOutside,
    BottomInside,
    BottomOutside
};
enum class BarsPosition
{
    Left,
    Right,
    Overlay
};
enum class ClockStyle
{
    Digital,
    Analogue
};
enum class ClockZone
{
    Tai,
    Utc,
    Local
};

struct Tile
{
    std::string id;
    TileContent content = TileContent::Input;
    int input = 1;
    NormRect rect;
    int z = 0;
    ScaleMode scale = ScaleMode::Fit;
    bool umd = true;
    UmdSource umdSource = UmdSource::Is04;
    std::string umdText;
    UmdPosition umdPosition = UmdPosition::BottomInside;
    int umdFont = 28;
    std::string umdBg = "#000000c0";
    bool tallyBorder = true;
    bool tallyLamp = true;
    bool audioBars = false;
    int audioBarChannels = 2;
    int audioBarFirst = 0;
    BarsPosition audioBarPosition = BarsPosition::Right;
    double zoneGreen = -18;
    double zoneAmber = -9;
    bool formatLabel = true;
    bool latency = false;
    bool safeArea = false;
    bool centre = false;
    std::vector<std::string> aspectMarkers;
    ClockStyle clockStyle = ClockStyle::Digital;
    ClockZone clockZone = ClockZone::Utc;
    std::string timecodeRate;
    std::string labelText;
};

struct Layout
{
    int version = 1;
    std::string name;
    std::string background = "#101010";
    std::vector<Tile> tiles;
};

struct LayoutBook
{
    int version = 1;
    std::string active = "2x2";
    std::vector<Layout> layouts;
};

std::vector<Layout> builtinPresets(int maxInputs);
LayoutBook defaultBook(int maxInputs, std::string const& active);

// Returns an error string on failure.
std::optional<std::string> validateLayout(Layout const& layout, int maxInputs);
std::optional<std::string> parseLayout(std::string const& json, Layout& out, int maxInputs);
std::string layoutToJson(Layout const& layout);
std::optional<std::string> parseBook(std::string const& json, LayoutBook& out, int maxInputs);
std::string bookToJson(LayoutBook const& book);

char const* contentName(TileContent content);
char const* scaleName(ScaleMode mode);
} // namespace mv
