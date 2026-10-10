#pragma once

#include "config/config.hpp"
#include "media/frame.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mv
{
// The WebRTC preview (§8.4): every head in one 1920×1080 canvas, encoded once as H.264.
inline constexpr int kMosaicWidth = 1920;
inline constexpr int kMosaicHeight = 1080;

struct PreviewRegion
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// Where head `head` (1-based) of `heads` is in the mosaic: the whole canvas for one head, else
// its quarter (2×2, reading order). The head's raster keeps its aspect inside that cell (centred,
// even values), so a 16:9 head is the whole cell.
PreviewRegion mosaicRegion(int head, int heads, VideoFormat const& format);

// What runs for the preview. Never both: MV_PREVIEW_MODE=jpeg encodes /preview.jpg per head and
// publishes nothing; webrtc builds the mosaic, encodes and publishes it, and encodes no JPEG.
struct PreviewPlan
{
    bool jpeg = true;
    bool webrtc = false;
    // webrtc without PREVIEW_PUBLISH_URL: this process starts its own MediaMTX.
    bool ownMediamtx = false;
    // The MediaMTX path of the mosaic, <PREVIEW_PATH_PREFIX>/heads.
    std::string path;
    // rtsp://<PREVIEW_PUBLISH_URL or 127.0.0.1:MEDIAMTX_RTSP_PORT>/<path>.
    std::string publishUrl;
};
PreviewPlan previewPlan(Config const& cfg);

// The playback URLs of the mosaic: <PREVIEW_WHEP_URL>/<path>/whep and <PREVIEW_HLS_URL>/<path>/index.m3u8,
// or the own MediaMTX on NMOS_HOST_ADDRESS when that setting is empty (the page then uses its own host name).
std::string previewWhepUrl(Config const& cfg);
std::string previewHlsUrl(Config const& cfg);

// Scales a planar 10-bit 4:2:2 picture to width × height NV12 (8-bit 4:2:0: the luma rows, then
// the interleaved CbCr rows), by area average. width and height are even.
void scaleToNv12(Frame422 const& src, int width, int height, std::uint8_t* nv12);

// The mosaic as NV12 in host memory. Head threads put their picture at the preview rate; the
// encoder thread copies the whole canvas. Areas no head covers are black.
class PreviewMosaic
{
public:
    explicit PreviewMosaic(int heads);

    // `nv12` is the head's picture at mosaicRegion(head, heads, format) size.
    void put(int head, VideoFormat const& format, std::uint8_t const* nv12);
    void copy(std::uint8_t* luma, int lumaPitch, std::uint8_t* chroma, int chromaPitch) const;

private:
    int heads_;
    mutable std::mutex mu_;
    std::vector<std::uint8_t> nv12_;
    std::vector<PreviewRegion> regions_;
};

// State of the mosaic's RTSP publish (§8.4), as /statusz and the metrics show it.
struct PreviewStatus
{
    // "connecting" until MediaMTX took the stream, "publishing" while packets go out, "error"
    // after a failure (with `error`) until it publishes again.
    std::string state = "connecting";
    std::string error;
    // "nvenc" or "x264" once an encoder is open.
    std::string encoder;
    std::uint64_t frames = 0;
    // The built-in MediaMTX (own mode).
    bool mediamtxRunning = false;
    std::uint64_t mediamtxRestarts = 0;
};
} // namespace mv
