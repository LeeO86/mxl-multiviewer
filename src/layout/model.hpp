#pragma once

#include <map>
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
enum class UmdAlign
{
    Left,
    Centre,
    Right
};
// Where a tile's alarm labels stack (§6.3): from that corner or edge into the tile.
enum class AlarmLabelPosition
{
    TopLeft,
    Top,
    TopRight,
    BottomLeft,
    Bottom,
    BottomRight
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
    UmdAlign umdAlign = UmdAlign::Left;
    int umdFont = 28;
    std::string umdBg = "#000000c0";
    bool tallyBorder = true;
    bool tallyLamp = true;
    // Text tally as the UMD background. Unset follows the layout's `tally_text`.
    std::optional<bool> tallyText;
    bool audioBars = false;
    bool audioBarRms = false;
    // The PPM scale (ticks and dBFS labels) beside the bars.
    bool audioBarScale = true;
    int audioBarChannels = 2;
    int audioBarFirst = 0;
    BarsPosition audioBarPosition = BarsPosition::Right;
    double zoneGreen = -18;
    double zoneAmber = -9;
    // Alarm display (§6.3): the alarm border, and a label per active alarm.
    bool alarmBorder = true;
    bool alarmLabels = true;
    AlarmLabelPosition alarmLabelPosition = AlarmLabelPosition::Top;
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
    // Default `tally_text` of the tiles. A head gets it by showing this layout (§6.2).
    bool tallyText = false;
    std::vector<Tile> tiles;
};

// The tile's `tally_text`, or the layout's when the tile leaves it unset.
bool tallyTextOn(Layout const& layout, Tile const& tile);

// Revision of the built-in preset defaults: 1 is 1.1.x (no audio bars), 2 is 1.2 (bars on
// input tiles). A book file without `preset_revision` is revision 1 (layout/migrate.hpp).
inline constexpr int kPresetRevision = 2;

struct LayoutBook
{
    int version = 1;
    std::string active = "2x2";
    std::vector<Layout> layouts;
    // The layout last chosen for each output head (1-based), kept across restarts.
    std::map<int, std::string> heads;
    // The layout a head starts on ("use as start layout"); beats `heads` at the next start.
    std::map<int, std::string> startLayouts;
    int presetRevision = kPresetRevision;
};

std::vector<Layout> builtinPresets(int maxInputs);
LayoutBook defaultBook(int maxInputs, std::string const& active);

// Returns an error string on failure.
std::optional<std::string> validateLayout(Layout const& layout, int maxInputs);
// With `repairs`, values an older release accepted (audio zones out of order or range, bars
// past channel 16, unknown clock style or zone) are corrected and each correction is
// described there. Without it (the API) they are rejected.
std::optional<std::string> parseLayout(std::string const& json, Layout& out, int maxInputs, std::vector<std::string>* repairs = nullptr);
std::string layoutToJson(Layout const& layout);
std::optional<std::string> parseBook(std::string const& json, LayoutBook& out, int maxInputs, std::vector<std::string>* repairs = nullptr);
std::string bookToJson(LayoutBook const& book);

char const* contentName(TileContent content);
char const* scaleName(ScaleMode mode);
} // namespace mv
