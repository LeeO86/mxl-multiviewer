#include "media/publisher.hpp"

#include "media/cuda_compose.hpp"
#include "util/logging.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#if defined(MV_WITH_CUDA)
#include <libavutil/hwcontext_cuda.h>
#endif
}

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <mutex>
#include <thread>

namespace mv
{
namespace
{
// CBR; at the default 5 fps that is 800 kbit per picture of the 1920×1080 mosaic.
constexpr std::int64_t kBitrate = 4'000'000;

std::string avError(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

// FFmpeg's errors go to this process's log, and the last one explains a failed open
// (for NVENC: no libnvidia-encode, no capable GPU).
std::mutex gAvMu;
std::string gAvLast;

void avLog(void* context, int level, char const* format, va_list args)
{
    if (level > AV_LOG_WARNING)
    {
        return;
    }
    char line[1024] = {};
    int prefix = 1;
    av_log_format_line2(context, level, format, args, line, sizeof(line), &prefix);
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    {
        text.pop_back();
    }
    if (text.empty())
    {
        return;
    }
    if (level <= AV_LOG_ERROR)
    {
        std::lock_guard lock{gAvMu};
        gAvLast = text;
    }
    logMessage(level <= AV_LOG_ERROR ? LogLevel::Warn : LogLevel::Debug, "ffmpeg", {{"message", text}});
}

std::string lastAvError(int code)
{
    std::lock_guard lock{gAvMu};
    auto text = gAvLast.empty() ? avError(code) : gAvLast + " (" + avError(code) + ")";
    gAvLast.clear();
    return text;
}
} // namespace

struct PreviewPublisher::Impl
{
    PreviewMosaic const& mosaic;
    std::string url;
    int fps;
    bool cudaContext;
    std::function<void(double)> observe;
    std::thread thread;
    std::mutex runMu;
    std::condition_variable wake;
    bool stopping = false;
    mutable std::mutex statusMu;
    PreviewStatus status;

    AVBufferRef* device = nullptr;
    AVCodecContext* enc = nullptr;
    AVFormatContext* out = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    bool nvencFailed = false;
    bool idr = true;

    Impl(PreviewMosaic const& mosaicIn, std::string urlIn, int fpsIn, bool cudaIn)
        : mosaic(mosaicIn)
        , url(std::move(urlIn))
        , fps(std::max(1, fpsIn))
        , cudaContext(cudaIn)
    {
    }

    void setState(std::string const& state, std::string const& error)
    {
        std::lock_guard lock{statusMu};
        if (state == "error" && status.error != error)
        {
            logWarn("preview_publish_error", {{"url", url}, {"error", error}});
        }
        else if (state == "publishing" && status.state != "publishing")
        {
            logInfo("preview_publishing", {{"url", url}, {"encoder", status.encoder}});
        }
        status.state = state;
        status.error = error;
    }

    // The process's CUDA context for NVENC, so it does not open a second one on the GPU.
    void attachDevice(AVCodecContext* ctx)
    {
#if defined(MV_WITH_CUDA)
        if (device == nullptr && cudaContext)
        {
            void* context = nullptr;
            void* stream = nullptr;
            if (cudaPreviewContext(&context, &stream))
            {
                device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
                auto* hw = static_cast<AVCUDADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx);
                hw->cuda_ctx = static_cast<CUcontext>(context);
                hw->stream = static_cast<CUstream>(stream);
                if (av_hwdevice_ctx_init(device) < 0)
                {
                    av_buffer_unref(&device);
                }
            }
        }
        if (device != nullptr)
        {
            ctx->hw_device_ctx = av_buffer_ref(device);
        }
#else
        (void)ctx;
#endif
    }

    bool openEncoder()
    {
        std::string reason = "no H.264 encoder in this FFmpeg build";
        for (std::string const name : {"h264_nvenc", "libx264"})
        {
            bool const nvenc = name == "h264_nvenc";
            if (nvenc && nvencFailed)
            {
                continue;
            }
            AVCodec const* codec = avcodec_find_encoder_by_name(name.c_str());
            if (codec == nullptr)
            {
                if (nvenc)
                {
                    nvencFailed = true;
                    logWarn("preview_nvenc_unavailable", {{"reason", "h264_nvenc is not in this FFmpeg build"}, {"fallback", "x264"}});
                }
                continue;
            }
            AVCodecContext* ctx = avcodec_alloc_context3(codec);
            ctx->width = kMosaicWidth;
            ctx->height = kMosaicHeight;
            ctx->time_base = AVRational{1, fps};
            ctx->framerate = AVRational{fps, 1};
            ctx->pix_fmt = AV_PIX_FMT_NV12;
            ctx->color_range = AVCOL_RANGE_MPEG;
            ctx->colorspace = AVCOL_SPC_BT709;
            ctx->color_primaries = AVCOL_PRI_BT709;
            ctx->color_trc = AVCOL_TRC_BT709;
            ctx->bit_rate = kBitrate;
            ctx->rc_max_rate = kBitrate;
            ctx->rc_buffer_size = static_cast<int>(kBitrate / fps * 2);
            // One second GOP: a new WHEP viewer has a picture within a second. No B-frames.
            ctx->gop_size = fps;
            ctx->max_b_frames = 0;
            // SPS and PPS in the RTSP SDP; MediaMTX repeats them for WebRTC readers.
            ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            AVDictionary* options = nullptr;
            av_dict_set(&options, "forced-idr", "1", 0);
            if (nvenc)
            {
                // The settings of mxl-webrtc-monitor and the FlowXer engine: CBR, fastest preset, ultra-low latency.
                attachDevice(ctx);
                av_dict_set(&options, "preset", "p1", 0);
                av_dict_set(&options, "tune", "ull", 0);
                av_dict_set(&options, "rc", "cbr", 0);
                av_dict_set(&options, "zerolatency", "1", 0);
                av_dict_set(&options, "delay", "0", 0);
            }
            else
            {
                av_dict_set(&options, "preset", "ultrafast", 0);
                av_dict_set(&options, "tune", "zerolatency", 0);
            }
            int const rc = avcodec_open2(ctx, codec, &options);
            av_dict_free(&options);
            if (rc < 0)
            {
                reason = name + ": " + lastAvError(rc);
                avcodec_free_context(&ctx);
                if (nvenc)
                {
                    nvencFailed = true;
                    logWarn("preview_nvenc_unavailable", {{"reason", reason}, {"fallback", "x264"}});
                }
                continue;
            }
            enc = ctx;
            frame = av_frame_alloc();
            frame->format = AV_PIX_FMT_NV12;
            frame->width = kMosaicWidth;
            frame->height = kMosaicHeight;
            if (av_frame_get_buffer(frame, 0) < 0)
            {
                closeEncoder();
                setState("error", "no memory for a preview frame");
                return false;
            }
            packet = packet != nullptr ? packet : av_packet_alloc();
            {
                std::lock_guard lock{statusMu};
                status.encoder = nvenc ? "nvenc" : "x264";
            }
            logInfo("preview_encoder", {{"encoder", nvenc ? "nvenc" : "x264"}, {"fps", std::to_string(fps)}, {"bitrate", std::to_string(kBitrate)}});
            return true;
        }
        setState("error", reason);
        return false;
    }

    void closeEncoder()
    {
        avcodec_free_context(&enc);
        av_frame_free(&frame);
    }

    bool openOutput()
    {
        AVFormatContext* ctx = nullptr;
        int rc = avformat_alloc_output_context2(&ctx, nullptr, "rtsp", url.c_str());
        if (rc < 0 || ctx == nullptr)
        {
            setState("error", "rtsp output: " + lastAvError(rc));
            return false;
        }
        AVStream* stream = avformat_new_stream(ctx, nullptr);
        avcodec_parameters_from_context(stream->codecpar, enc);
        stream->time_base = enc->time_base;
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        // Socket timeout in microseconds: a stalled MediaMTX holds this thread for 2 s at most.
        av_dict_set(&options, "timeout", "2000000", 0);
        rc = avformat_write_header(ctx, &options);
        av_dict_free(&options);
        if (rc < 0)
        {
            setState("error", lastAvError(rc));
            avformat_free_context(ctx);
            return false;
        }
        out = ctx;
        idr = true;
        return true;
    }

    void closeOutput()
    {
        if (out != nullptr)
        {
            // The RTSP muxer sends TEARDOWN in the trailer, also after an error.
            av_write_trailer(out);
            avformat_free_context(out);
            out = nullptr;
        }
    }

    // Copy, encode, send one picture. False when the encoder or the connection failed.
    bool publish(std::int64_t pts)
    {
        if (av_frame_make_writable(frame) < 0)
        {
            setState("error", "preview frame is not writable");
            return false;
        }
        mosaic.copy(frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1]);
        frame->pts = pts;
        // A new connection starts with an IDR picture.
        frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        idr = false;
        int rc = avcodec_send_frame(enc, frame);
        if (rc < 0)
        {
            setState("error", "encode: " + lastAvError(rc));
            closeOutput();
            closeEncoder();
            return false;
        }
        std::uint64_t sent = 0;
        while ((rc = avcodec_receive_packet(enc, packet)) == 0)
        {
            packet->stream_index = 0;
            av_packet_rescale_ts(packet, enc->time_base, out->streams[0]->time_base);
            rc = av_interleaved_write_frame(out, packet);
            av_packet_unref(packet);
            if (rc < 0)
            {
                setState("error", "rtsp write: " + lastAvError(rc));
                closeOutput();
                return false;
            }
            ++sent;
        }
        if (sent > 0)
        {
            {
                std::lock_guard lock{statusMu};
                status.frames += sent;
            }
            setState("publishing", "");
        }
        return true;
    }

    void loop()
    {
        pthread_setname_np(pthread_self(), "mv-preview");
        using Clock = std::chrono::steady_clock;
        auto const period = std::chrono::nanoseconds(1'000'000'000LL / fps);
        auto const start = Clock::now();
        auto next = start;
        Clock::time_point retryEncoder{};
        Clock::time_point retryOutput{};
        std::int64_t lastPts = -1;
        for (;;)
        {
            next += period;
            if (next < Clock::now())
            {
                next = Clock::now() + period;
            }
            {
                std::unique_lock lock{runMu};
                if (wake.wait_until(lock, next, [this] { return stopping; }))
                {
                    break;
                }
            }
            auto const now = Clock::now();
            if (enc == nullptr)
            {
                if (now < retryEncoder)
                {
                    continue;
                }
                if (!openEncoder())
                {
                    retryEncoder = now + std::chrono::seconds(5);
                    continue;
                }
            }
            if (out == nullptr)
            {
                if (now < retryOutput)
                {
                    continue;
                }
                if (!openOutput())
                {
                    retryOutput = now + std::chrono::seconds(2);
                    continue;
                }
            }
            // Timestamps follow the clock, so a skipped picture does not slow the stream down.
            std::int64_t const pts = std::max(lastPts + 1, static_cast<std::int64_t>((now - start) / period));
            lastPts = pts;
            auto const began = Clock::now();
            if (!publish(pts))
            {
                retryOutput = now + std::chrono::seconds(1);
                continue;
            }
            if (observe)
            {
                observe(std::chrono::duration<double>(Clock::now() - began).count());
            }
        }
        closeOutput();
        closeEncoder();
        av_packet_free(&packet);
        av_buffer_unref(&device);
    }
};

PreviewPublisher::PreviewPublisher(PreviewMosaic const& mosaic, std::string url, int fps, bool cudaContext)
    : impl_(std::make_unique<Impl>(mosaic, std::move(url), fps, cudaContext))
{
    static std::once_flag logging;
    std::call_once(logging, [] { av_log_set_callback(avLog); });
}

PreviewPublisher::~PreviewPublisher()
{
    stop();
}

void PreviewPublisher::start(std::function<void(double)> observe)
{
    impl_->observe = std::move(observe);
    impl_->thread = std::thread([this] { impl_->loop(); });
}

void PreviewPublisher::stop()
{
    {
        std::lock_guard lock{impl_->runMu};
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
}

PreviewStatus PreviewPublisher::status() const
{
    std::lock_guard lock{impl_->statusMu};
    return impl_->status;
}
} // namespace mv
