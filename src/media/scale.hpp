#pragma once

#include "layout/geometry.hpp"
#include "media/frame.hpp"

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

void blendStraightRgba(Frame422& canvas, std::uint8_t const* rgba, int stride);
} // namespace mv
