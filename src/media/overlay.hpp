#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "layout/geometry.hpp"
#include "layout/model.hpp"
#include "media/image.hpp"

namespace mv
{
struct Rgba
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

Rgba parseHexColor(std::string const& text, Rgba fallback = {0, 0, 0, 255});

// 10-bit limited-range YCbCr of an RGB colour, with the overlay blend's coefficients
// (prepareOverlay). Alpha is ignored. Used for the layout background colour.
struct Ycbcr10
{
    std::uint16_t y = 64;
    std::uint16_t cb = 512;
    std::uint16_t cr = 512;
};
Ycbcr10 toYcbcr10(Rgba color);

struct Overlay
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    void resize(int w, int h);
    void clear();
    void fillRect(int x, int y, int w, int h, Rgba color);
    void strokeRect(int x, int y, int w, int h, int thickness, Rgba color);
    void text(int x, int y, std::string const& value, int pixelSize, Rgba color);
    // Advance width of `value` as text() draws it at this size.
    [[nodiscard]] int textWidth(std::string const& value, int pixelSize) const;
    void line(int x0, int y0, int x1, int y1, Rgba color, double width = 1.6);
    // A w × h picture of premultiplied BGRA pixels (ScaledImage) with its top-left at (x, y).
    void image(int x, int y, int w, int h, std::uint32_t const* prgb);

    // Set by renderOverlay while a Blend2D context owns the frame. Null on the bitmap path.
    void* blContext = nullptr;
};

// A meter value below the bottom of the scale (no audio).
inline constexpr double kSilentDbfs = -120.0;

inline constexpr std::array<double, 16> silentMeters()
{
    std::array<double, 16> values{};
    for (auto& value : values)
    {
        value = kSilentDbfs;
    }
    return values;
}

// Alarm colour of a tile (SPECIFICATION.md §6.3): red for no signal, black, freeze and
// clip; amber for silence and format mismatch.
enum class AlarmLevel
{
    None,
    Red,
    Amber
};

// One alarm label: the alarm's name in its colour.
struct AlarmBadge
{
    std::string text;
    AlarmLevel level = AlarmLevel::Red;
};

struct OverlayTile
{
    PixelRect rect;
    bool umd = false;
    std::string umdText;
    UmdPosition umdPosition = UmdPosition::BottomInside;
    UmdAlign umdAlign = UmdAlign::Left;
    int umdFont = 28;
    Rgba umdBg{0, 0, 0, 192};
    Rgba umdFg{255, 255, 255, 255};
    // TSL colours (0 off, 1 red, 2 green, 3 amber): `tally` is the border (text, else RH,
    // else LH), the left lamp shows LH, the right lamp RH. With `textTallyBg` the UMD
    // background shows the text tally while it is not off.
    int tally = 0;
    int lhTally = 0;
    int rhTally = 0;
    int textTally = 0;
    bool textTallyBg = false;
    bool tallyBorder = false;
    bool tallyLamp = false;
    // Audio bars (§5.7). Only input tiles set this.
    bool bars = false;
    // False when the input's audio leg is not routed: dim, empty bars with a strike.
    bool audioRouted = true;
    bool showRms = false;
    // The PPM scale (ticks and dBFS labels) beside the bars.
    bool barScale = true;
    int barChannels = 2;
    std::array<double, 16> ppmDbfs = silentMeters();
    std::array<double, 16> holdDbfs = silentMeters();
    std::array<double, 16> rmsDbfs = silentMeters();
    std::array<bool, 16> clip{};
    BarsPosition barsPosition = BarsPosition::Right;
    double zoneGreen = -18;
    double zoneAmber = -9;
    std::string formatText;
    std::string latencyText;
    bool safeArea = false;
    bool centre = false;
    std::vector<std::string> aspectMarkers;
    // The alarm border's colour (None: no border), and the labels stacked at `badgePosition`.
    AlarmLevel alarm = AlarmLevel::None;
    std::vector<AlarmBadge> badges;
    AlarmLabelPosition badgePosition = AlarmLabelPosition::Top;
    // Text over the black tile after MV_HOLD_MS (§6.4): NO SIGNAL, NOT ROUTED or WAITING,
    // with the input label under it.
    std::string slate;
    std::string slateLabel;
    bool clock = false;
    bool analogue = false;
    int clockHour = 0;
    int clockMinute = 0;
    int clockSecond = 0;
    std::string clockText;
    std::string timecodeText;
    std::string labelText;
    // Image tiles: the picture scaled to the tile, and the frame to show (animations).
    std::shared_ptr<ScaledImage const> image;
    std::size_t imageFrame = 0;
};

// The active alarms of an input (§6.3).
struct ActiveAlarms
{
    bool noSignal = false;
    bool black = false;
    bool freeze = false;
    bool clip = false;
    bool silence = false;
    bool format = false;
};

// The alarm border and labels of an input tile, as its options say (`alarm_border`,
// `alarm_labels`, `alarm_label_position`): red for no signal, black, freeze, and clip, amber
// for silence and format; one label per active alarm, most severe first. A tile with a
// slate (set `item.slate` first) gets no labels: the slate says what is wrong.
void showAlarms(OverlayTile& item, Tile const& tile, ActiveAlarms const& alarms);

// PPM scale marks in dBFS (§5.7), and the meter range they cover.
inline constexpr std::array<int, 8> kPpmMarks{0, -6, -12, -18, -24, -36, -48, -60};
inline constexpr double kMeterFloorDbfs = -60.0;

void renderOverlay(Overlay& overlay, std::vector<OverlayTile> const& tiles);

// True when this binary rasterises the overlay with Blend2D. The 8×8 path remains for MV_WITH_BLEND2D=OFF.
bool overlayUsesBlend2d();

// Areas where two RGBA overlays of the same size differ, as merged runs of 64×16-pixel
// blocks. Most of an overlay is transparent and only meters, clocks and texts change,
// so the GPU copy only needs these areas (a 2160p overlay is 33 MB).
std::vector<PixelRect> overlayChanges(std::vector<std::uint8_t> const& before, std::vector<std::uint8_t> const& after, int width, int height);
} // namespace mv
