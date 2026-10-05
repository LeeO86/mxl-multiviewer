#pragma once

#include "layout/geometry.hpp"
#include "media/frame.hpp"

#include <utility>
#include <vector>

namespace mv
{
// Bilinear scale of src into dst. Sample positions are pixel centres.
// bob reads only even source lines (field 0) while still covering the full frame height.
void scaleInto(Frame422& dst, Placement const& place, Frame422 const& src, bool bob);

// Limited-range black (Y=64, Cb=Cr=512) then each tile in order.
void composeTiles(Frame422& canvas, std::uint16_t bgY, std::uint16_t bgCb, std::uint16_t bgCr, Frame422 const* background, int backgroundCount,
    struct ComposeTile const* tiles, int tileCount);

struct ComposeTile
{
    Placement place;
    Frame422 const* source = nullptr;
    bool bob = false;
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
void scaleV210Into(Frame422& dst, Placement const& place, V210View const& src, bool bob);

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
