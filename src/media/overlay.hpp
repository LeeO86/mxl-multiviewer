#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "layout/geometry.hpp"
#include "layout/model.hpp"

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
    void line(int x0, int y0, int x1, int y1, Rgba color);

    // Set by renderOverlay while a Blend2D context owns the frame. Null on the bitmap path.
    void* blContext = nullptr;
};

struct OverlayTile
{
    PixelRect rect;
    bool umd = false;
    std::string umdText;
    UmdPosition umdPosition = UmdPosition::BottomInside;
    int umdFont = 28;
    Rgba umdBg{0, 0, 0, 192};
    Rgba umdFg{255, 255, 255, 255};
    int tally = 0;
    bool tallyBorder = false;
    bool tallyLamp = false;
    bool bars = false;
    bool showRms = false;
    int barChannels = 2;
    double ppmDbfs[16] = {};
    double rmsDbfs[16] = {};
    bool clip[16] = {};
    BarsPosition barsPosition = BarsPosition::Right;
    double zoneGreen = -18;
    double zoneAmber = -9;
    std::string formatText;
    std::string latencyText;
    bool safeArea = false;
    bool centre = false;
    std::vector<std::string> aspectMarkers;
    std::string badge;
    bool clock = false;
    bool analogue = false;
    int clockHour = 0;
    int clockMinute = 0;
    int clockSecond = 0;
    std::string clockText;
    std::string timecodeText;
    std::string labelText;
};

void renderOverlay(Overlay& overlay, std::vector<OverlayTile> const& tiles);

// True when this binary rasterises the overlay with Blend2D. The 8×8 path remains for MV_WITH_BLEND2D=OFF.
bool overlayUsesBlend2d();

// Areas where two RGBA overlays of the same size differ, as merged runs of 64×16-pixel
// blocks. Most of an overlay is transparent and only meters, clocks and texts change,
// so the GPU copy only needs these areas (a 2160p overlay is 33 MB).
std::vector<PixelRect> overlayChanges(std::vector<std::uint8_t> const& before, std::vector<std::uint8_t> const& after, int width, int height);
} // namespace mv
