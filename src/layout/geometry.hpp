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

// Sizes of a tile's audio bars at this canvas height (§5.7), shared by the overlay that draws
// them and the composer that places the picture beside them.
struct BarMetrics
{
    int margin = 0;
    int gap = 0;
    int barW = 0;
    int barsW = 0;
    int tickW = 0;
    // Room for the dBFS labels when the bars are beside the picture.
    int labelReserve = 0;
};
BarMetrics barMetrics(int tileWidth, int channels, int canvasHeight);

// Width of the strip that bars beside the picture take from the tile (even, so the picture
// keeps v210 alignment); 0 when it would be more than half the tile, and then no bars are drawn.
int barsStripWidth(int tileWidth, int channels, bool scale, int canvasHeight);

// The part of the tile the picture is placed in: the tile, less the strip of audio bars
// beside the picture (audio_bar_position left-beside or right-beside on an input tile).
PixelRect pictureRect(PixelRect const& tile, Tile const& options, int canvasHeight);
Placement placeTile(PixelRect const& tile, int srcWidth, int srcHeight, ScaleMode mode);
} // namespace mv
