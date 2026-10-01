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
    int barChannels = 2;
    double ppmDbfs[16] = {};
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
    std::string clockText;
    std::string labelText;
};

void renderOverlay(Overlay& overlay, std::vector<OverlayTile> const& tiles);

// Blend2D is selected at build time. The bitmap path is always available.
bool overlayUsesBlend2d();
} // namespace mv
