#pragma once

#include "layout/model.hpp"

namespace mv
{
struct PixelRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Placement
{
    PixelRect dst;
    float srcX = 0;
    float srcY = 0;
    float srcW = 0;
    float srcH = 0;
};

PixelRect rectToPixels(NormRect const& rect, int canvasWidth, int canvasHeight);
Placement placeTile(PixelRect const& tile, int srcWidth, int srcHeight, ScaleMode mode);
} // namespace mv
