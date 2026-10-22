#pragma once

#include "layout/geometry.hpp"
#include "media/frame.hpp"

#include <utility>
#include <vector>

namespace mv
{
// Interlaced sources (a frame woven from two fields): the scaler reads only the lines of one
// field (bob), placed at their own height in the frame, so a tile shows the newest field without
// combing. Off reads every line.
enum class Bob : int
{
    Off = -1,
    Top = 0,    // even lines 0, 2, …
    Bottom = 1, // odd lines 1, 3, …
};

// Bilinear scale of src into dst. Sample positions are pixel centres. A source with a key
// (v210a) is blended over what dst holds (luma and chroma). A dst with alpha is a layer instead:
// it gets the straight samples and the key, and blitTile() blends it later.
void scaleInto(Frame422& dst, Placement const& place, Frame422 const& src, Bob bob);

// A tile image onto the canvas at dst (its top-left corner, even x). An image with alpha (a
// layer from scaleInto) is laid over what the canvas holds there: key 0 keeps the canvas, 1023
// takes the tile, values between mix them (chroma by the key of the pair's first pixel, as on
// CUDA). An image without alpha is copied.
void blitTile(Frame422& canvas, PixelRect const& dst, Frame422 const& tile);

// Limited-range black (Y=64, Cb=Cr=512) then each tile in order.
void composeTiles(Frame422& canvas, std::uint16_t bgY, std::uint16_t bgCb, std::uint16_t bgCr, Frame422 const* background, int backgroundCount,
    struct ComposeTile const* tiles, int tileCount);

struct ComposeTile
{
    Placement place;
    Frame422 const* source = nullptr;
    Bob bob = Bob::Off;
};

// A packed v210 picture without alpha.
struct V210View
{
    std::uint8_t const* data = nullptr;
    int rowBytes = 0;
    int width = 0;
    int height = 0;
};

// scaleInto() straight from packed v210: the same sample positions, weights in 1/1024 steps
// (within 1 of the float result, exact on flat areas), and only the source lines the
// placement touches are unpacked.
void scaleV210Into(Frame422& dst, Placement const& place, V210View const& src, Bob bob);

void blendStraightRgba(Frame422& canvas, std::uint8_t const* rgba, int stride);

// blendStraightRgba() split in two: prepareOverlay() converts the overlay once (when it
// changes) and blendOverlay() blends it into each frame with integers, only where it is visible.
// Together they give the same samples as blendStraightRgba().
struct PreparedOverlay
{
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> y, cb, cr;
    std::vector<std::uint8_t> a, ca;               // luma alpha per pixel, chroma alpha per pair
    std::vector<std::vector<std::pair<int, int>>> spans; // per row: [x0, x1) with something to blend
};
PreparedOverlay prepareOverlay(std::uint8_t const* rgba, int width, int height, int stride);
void blendOverlay(Frame422& canvas, PreparedOverlay const& overlay);

// Scale src with fill (cover) into an already-allocated dst. Limited-range black shows only if the source is empty.
void coverFrame(Frame422& dst, Frame422 const& src);
} // namespace mv
