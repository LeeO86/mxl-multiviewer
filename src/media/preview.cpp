#include "media/preview.hpp"

#include "media/nv12scale.hpp"

#include <algorithm>
#include <cstring>

namespace mv
{
PreviewRegion mosaicRegion(int head, int heads, VideoFormat const& format)
{
    PreviewRegion cell{0, 0, kMosaicWidth, kMosaicHeight};
    if (heads > 1)
    {
        cell.w = kMosaicWidth / 2;
        cell.h = kMosaicHeight / 2;
        cell.x = ((head - 1) % 2) * cell.w;
        cell.y = ((head - 1) / 2) * cell.h;
    }
    if (format.width <= 0 || format.height <= 0)
    {
        return cell;
    }
    // Fit, keeping the aspect: the full width when the raster is at least as wide as the cell.
    int w = cell.w;
    int h = static_cast<int>(static_cast<long long>(cell.w) * format.height / format.width);
    if (h > cell.h)
    {
        h = cell.h;
        w = static_cast<int>(static_cast<long long>(cell.h) * format.width / format.height);
    }
    w = std::max(2, w & ~1);
    h = std::max(2, h & ~1);
    return {cell.x + ((cell.w - w) / 2 & ~1), cell.y + ((cell.h - h) / 2 & ~1), w, h};
}

PreviewPlan previewPlan(Config const& cfg)
{
    PreviewPlan plan;
    plan.webrtc = cfg.previewMode == "webrtc";
    plan.jpeg = !plan.webrtc;
    plan.ownMediamtx = plan.webrtc && cfg.previewPublishUrl.empty();
    plan.path = cfg.previewPathPrefix + "/heads";
    auto const base = cfg.previewPublishUrl.empty() ? "rtsp://127.0.0.1:" + std::to_string(cfg.mediamtxRtspPort) : cfg.previewPublishUrl;
    plan.publishUrl = base + "/" + plan.path;
    return plan;
}

namespace
{
std::string ownHost(Config const& cfg)
{
    return cfg.nmosHostAddress.empty() ? std::string("127.0.0.1") : cfg.nmosHostAddress;
}
} // namespace

std::string previewWhepUrl(Config const& cfg)
{
    auto const base = cfg.previewWhepUrl.empty() ? "http://" + ownHost(cfg) + ":" + std::to_string(cfg.mediamtxWhepPort) : cfg.previewWhepUrl;
    return base + "/" + previewPlan(cfg).path + "/whep";
}

std::string previewHlsUrl(Config const& cfg)
{
    auto const base = cfg.previewHlsUrl.empty() ? "http://" + ownHost(cfg) + ":" + std::to_string(cfg.mediamtxHlsPort) : cfg.previewHlsUrl;
    return base + "/" + previewPlan(cfg).path + "/index.m3u8";
}

void scaleToNv12(Frame422 const& src, int width, int height, std::uint8_t* nv12)
{
    std::uint8_t* chroma = nv12 + static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (int by = 0; by < height / 2; ++by)
    {
        for (int bx = 0; bx < width / 2; ++bx)
        {
            nv12::block(src.y.data(), src.cb.data(), src.cr.data(), src.width, src.height, width, height, bx, by, nv12, width, chroma, width);
        }
    }
}

PreviewMosaic::PreviewMosaic(int heads)
    : heads_(heads)
    , nv12_(static_cast<std::size_t>(kMosaicWidth) * kMosaicHeight * 3 / 2, 128)
    , regions_(static_cast<std::size_t>(std::max(1, heads)))
{
    // Limited-range black: Y 16, Cb and Cr 128.
    std::fill(nv12_.begin(), nv12_.begin() + static_cast<std::ptrdiff_t>(kMosaicWidth) * kMosaicHeight, std::uint8_t{16});
}

void PreviewMosaic::put(int head, VideoFormat const& format, std::uint8_t const* nv12)
{
    if (head < 1 || head > heads_ || nv12 == nullptr)
    {
        return;
    }
    auto const region = mosaicRegion(head, heads_, format);
    std::uint8_t* luma = nv12_.data();
    std::uint8_t* chroma = luma + static_cast<std::size_t>(kMosaicWidth) * kMosaicHeight;
    std::lock_guard lock{mu_};
    auto& last = regions_[static_cast<std::size_t>(head - 1)];
    if (last.w != region.w || last.h != region.h || last.x != region.x || last.y != region.y)
    {
        // A new raster of this head: the old picture's edges must not stay around the new one.
        for (int y = last.y; y < last.y + last.h; ++y)
        {
            std::memset(luma + y * kMosaicWidth + last.x, 16, static_cast<std::size_t>(last.w));
        }
        for (int y = last.y / 2; y < (last.y + last.h) / 2; ++y)
        {
            std::memset(chroma + y * kMosaicWidth + last.x, 128, static_cast<std::size_t>(last.w));
        }
        last = region;
    }
    for (int y = 0; y < region.h; ++y)
    {
        std::memcpy(luma + (region.y + y) * kMosaicWidth + region.x, nv12 + static_cast<std::size_t>(y) * region.w, static_cast<std::size_t>(region.w));
    }
    std::uint8_t const* tileChroma = nv12 + static_cast<std::size_t>(region.w) * region.h;
    for (int y = 0; y < region.h / 2; ++y)
    {
        std::memcpy(chroma + (region.y / 2 + y) * kMosaicWidth + region.x, tileChroma + static_cast<std::size_t>(y) * region.w,
            static_cast<std::size_t>(region.w));
    }
}

void PreviewMosaic::copy(std::uint8_t* luma, int lumaPitch, std::uint8_t* chroma, int chromaPitch) const
{
    std::uint8_t const* srcLuma = nv12_.data();
    std::uint8_t const* srcChroma = srcLuma + static_cast<std::size_t>(kMosaicWidth) * kMosaicHeight;
    std::lock_guard lock{mu_};
    for (int y = 0; y < kMosaicHeight; ++y)
    {
        std::memcpy(luma + static_cast<std::ptrdiff_t>(y) * lumaPitch, srcLuma + static_cast<std::size_t>(y) * kMosaicWidth, kMosaicWidth);
    }
    for (int y = 0; y < kMosaicHeight / 2; ++y)
    {
        std::memcpy(chroma + static_cast<std::ptrdiff_t>(y) * chromaPitch, srcChroma + static_cast<std::size_t>(y) * kMosaicWidth, kMosaicWidth);
    }
}
} // namespace mv
