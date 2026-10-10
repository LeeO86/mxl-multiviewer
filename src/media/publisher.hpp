#pragma once

#include "media/preview.hpp"

#include <functional>
#include <memory>
#include <string>

namespace mv
{
// The WebRTC preview's encoder (§8.4). Its own thread copies the mosaic at MV_PREVIEW_FPS, encodes
// it once as H.264 (NVENC; libx264 only when NVENC cannot be opened, which is logged) and publishes
// it over RTSP/TCP to MediaMTX, which serves WHEP and HLS. The compositor threads never wait for it.
// Needs FFmpeg (libavcodec, libavformat); built into mxl-multiviewer only.
class PreviewPublisher
{
public:
    // `cudaContext`: NVENC works in the compositor's CUDA context (CUDA backend).
    PreviewPublisher(PreviewMosaic const& mosaic, std::string url, int fps, bool cudaContext);
    ~PreviewPublisher();
    PreviewPublisher(PreviewPublisher const&) = delete;
    PreviewPublisher& operator=(PreviewPublisher const&) = delete;

    // `observe` gets the seconds this thread spent on each published frame (copy, encode, send).
    void start(std::function<void(double)> observe);
    void stop();
    [[nodiscard]] PreviewStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mv
