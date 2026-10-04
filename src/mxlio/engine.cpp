#include "mxlio/engine.hpp"

#include "control/tsl.hpp"
#include "media/alarm.hpp"
#include "domain/scan.hpp"
#include "layout/geometry.hpp"
#include "media/cuda_compose.hpp"
#include "media/flowtext.hpp"
#include "media/jpeg.hpp"
#include "media/overlay.hpp"
#include "media/ppm.hpp"
#include "media/scale.hpp"
#include "media/timebase.hpp"
#include "nmos/ids.hpp"
#include "util/httpclient.hpp"
#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace mv
{
namespace
{
// How long an input waits for its next grain before it republishes its state.
constexpr std::uint64_t kGrainWaitNs = 50'000'000;

struct Route
{
    bool enable = false;
    std::string domainId;
    std::string flowId;
    std::string senderId;
};

struct SavedGrain
{
    // CUDA backend: `gpu` (uploaded and unpacked on the device), `frame` only when the
    // upload failed. CPU backend: `frame`.
    std::shared_ptr<Frame422> frame;
    std::shared_ptr<CudaFrame const> gpu;
    std::uint64_t index = 0;
    std::uint64_t origin = 0;
    int width = 0;
    int height = 0;
    int rateNum = 0;
    int rateDen = 1;
    bool interlaced = false;
    bool alpha = false;
    std::string mediaType;
};

struct AudioWindow
{
    std::vector<std::vector<float>> channels;
    std::uint64_t first = 0;
};

struct Snap
{
    std::vector<SavedGrain> grains;
    AudioWindow audio;
    std::string videoState = "not_routed";
    std::string audioState = "not_routed";
    std::string videoReason;
    std::string audioReason;
    std::string senderId;
    std::string audioSender;
    std::string label;
    std::uint64_t grainsRead = 0;
    std::uint64_t resyncs = 0;
    std::array<double, 16> ppm{};
    std::array<double, 16> rms{};
    std::array<bool, 16> clip{};
    bool alarmNoSignal = false;
    bool alarmBlack = false;
    bool alarmFreeze = false;
    bool alarmSilence = false;
    bool alarmClip = false;
    bool alarmFormat = false;
};

struct FlowMeta
{
    int width = 0;
    int height = 0;
    int rateNum = 25;
    int rateDen = 1;
    int channels = 0;
    bool interlaced = false;
    bool alpha = false;
    std::string mediaType;
    std::string label;
};

FlowMeta parseFlow(std::string const& body)
{
    FlowMeta meta;
    std::string error;
    auto const root = json::parse(body, error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        return meta;
    }
    auto const& obj = root.get<picojson::object>();
    auto num = [&](char const* key, double fallback) {
        auto const it = obj.find(key);
        if (it == obj.end() || !it->second.is<double>())
        {
            return fallback;
        }
        return it->second.get<double>();
    };
    meta.width = static_cast<int>(num("frame_width", 0));
    meta.height = static_cast<int>(num("frame_height", 0));
    meta.channels = static_cast<int>(num("channel_count", 0));
    meta.mediaType = json::fieldString(root, "media_type").value_or("");
    meta.label = json::fieldString(root, "label").value_or("");
    meta.alpha = meta.mediaType == "video/v210a";
    meta.interlaced = json::fieldString(root, "interlace_mode").value_or("").find("interlaced") == 0;
    if (obj.count("grain_rate") != 0 && obj.at("grain_rate").is<picojson::object>())
    {
        auto const& rate = obj.at("grain_rate").get<picojson::object>();
        if (rate.count("numerator") != 0 && rate.at("numerator").is<double>())
        {
            meta.rateNum = static_cast<int>(rate.at("numerator").get<double>());
        }
        if (rate.count("denominator") != 0 && rate.at("denominator").is<double>())
        {
            meta.rateDen = std::max(1, static_cast<int>(rate.at("denominator").get<double>()));
        }
    }
    return meta;
}

std::string readFlowDef(mxlInstance instance, std::string const& flowId)
{
    std::size_t size = 0;
    mxlGetFlowDef(instance, flowId.c_str(), nullptr, &size);
    if (size == 0)
    {
        size = 8192;
    }
    std::string buffer(size, '\0');
    if (mxlGetFlowDef(instance, flowId.c_str(), buffer.data(), &size) != MXL_STATUS_OK)
    {
        return {};
    }
    buffer.resize(std::strlen(buffer.c_str()));
    return buffer;
}

void pushAudio(AudioWindow& window, mxlWrappedMultiBufferSlice const& slice, std::size_t count, std::uint64_t endIndex)
{
    int const channels = static_cast<int>(std::min<std::size_t>(slice.count, 64));
    if (channels <= 0 || count == 0)
    {
        return;
    }
    std::uint64_t const begin = endIndex >= count ? endIndex - count : 0;
    if (window.channels.size() != static_cast<std::size_t>(channels))
    {
        window.channels.assign(static_cast<std::size_t>(channels), {});
        window.first = begin;
    }
    if (window.channels[0].empty())
    {
        window.first = begin;
    }
    std::uint64_t const haveEnd = window.first + window.channels[0].size();
    if (begin > haveEnd)
    {
        window.channels.assign(static_cast<std::size_t>(channels), {});
        window.first = begin;
    }
    std::size_t const skip = begin > window.first ? static_cast<std::size_t>(begin - window.first) : 0;
    for (int ch = 0; ch < channels; ++ch)
    {
        auto& dst = window.channels[static_cast<std::size_t>(ch)];
        if (dst.size() < skip)
        {
            dst.resize(skip, 0.f);
        }
        std::size_t filled = 0;
        for (int frag = 0; frag < 2 && filled < count; ++frag)
        {
            auto const* pointer = static_cast<char const*>(slice.base.fragments[frag].pointer);
            if (pointer == nullptr || slice.base.fragments[frag].size == 0)
            {
                continue;
            }
            auto const* src = reinterpret_cast<float const*>(pointer + static_cast<std::size_t>(ch) * slice.stride);
            std::size_t const available = slice.base.fragments[frag].size / sizeof(float);
            std::size_t const take = std::min(available, count - filled);
            if (dst.size() < skip + filled + take)
            {
                dst.resize(skip + filled + take, 0.f);
            }
            std::memcpy(dst.data() + skip + filled, src, take * sizeof(float));
            filled += take;
        }
    }
    std::size_t const maxSamples = 48000;
    if (!window.channels.empty() && window.channels[0].size() > maxSamples)
    {
        std::size_t const drop = window.channels[0].size() - maxSamples;
        for (auto& channel : window.channels)
        {
            channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(std::min(drop, channel.size())));
        }
        window.first += drop;
    }
}

void blit(Frame422& canvas, PixelRect const& dst, Frame422 const& tile)
{
    int const cw = canvas.chromaWidth();
    int const tw = tile.chromaWidth();
    for (int y = 0; y < tile.height && dst.y + y < canvas.height; ++y)
    {
        for (int x = 0; x < tile.width && dst.x + x < canvas.width; ++x)
        {
            canvas.y[static_cast<std::size_t>((dst.y + y) * canvas.width + dst.x + x)] = tile.y[static_cast<std::size_t>(y * tile.width + x)];
            if (((dst.x + x) & 1) == 0 && (x / 2) < tw && (dst.x + x) / 2 < cw)
            {
                canvas.cb[static_cast<std::size_t>((dst.y + y) * cw + (dst.x + x) / 2)] = tile.cb[static_cast<std::size_t>(y * tw + x / 2)];
                canvas.cr[static_cast<std::size_t>((dst.y + y) * cw + (dst.x + x) / 2)] = tile.cr[static_cast<std::size_t>(y * tw + x / 2)];
            }
        }
    }
}

bool allowedRate(int num, int den)
{
    double const hz = static_cast<double>(num) / std::max(1, den);
    return hz >= 23.0 && hz <= 60.5;
}

Rgba tallyRgba(int tally)
{
    switch (tally)
    {
    case 1:
        return {220, 32, 32, 255};
    case 2:
        return {32, 180, 64, 255};
    case 3:
        return {220, 160, 32, 255};
    default:
        return {255, 255, 255, 255};
    }
}
} // namespace

struct Engine::Impl
{
    Config config;
    RuntimeModel& runtime;
    LayoutBookStore& layouts;
    Metrics& metrics;
    std::atomic<bool> run{false};
    std::string domainId;
    std::mutex routeMu;
    std::vector<Route> videoRoutes;
    std::vector<Route> audioRoutes;
    std::vector<std::shared_ptr<Snap>> snaps;
    std::mutex snapMu;
    std::vector<std::thread> threads;
    std::function<void(int, std::string const&, std::string const&, VideoFormat const&)> onFlow;
    int tslUdp = -1;
    int tslListen = -1;
    bool loadingRoutes = false;
    Frame422 background;
    // Input threads read it per grain; a head turns it off when CUDA keeps failing.
    std::atomic<bool> useCuda{false};
    // Per head, the overlay thread's latest finished drawing (straight RGBA).
    struct OverlayFrame
    {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;
        std::uint64_t version = 0;
        // CUDA backend: the same overlay on the device, uploaded by the overlay thread.
        std::shared_ptr<CudaOverlay const> device;
    };
    std::vector<std::shared_ptr<OverlayFrame const>> overlays;
    std::mutex overlayMu;

    explicit Impl(Config cfg, RuntimeModel& runtimeIn, LayoutBookStore& layoutsIn, Metrics& metricsIn)
        : config(std::move(cfg))
        , runtime(runtimeIn)
        , layouts(layoutsIn)
        , metrics(metricsIn)
    {
        videoRoutes.resize(static_cast<std::size_t>(config.maxInputs));
        audioRoutes.resize(static_cast<std::size_t>(config.maxInputs));
        snaps.resize(static_cast<std::size_t>(config.maxInputs));
        for (auto& snap : snaps)
        {
            snap = std::make_shared<Snap>();
            snap->ppm.fill(-120);
            snap->rms.fill(-120);
        }
    }

    Route videoRoute(int input)
    {
        std::lock_guard lock{routeMu};
        return videoRoutes[static_cast<std::size_t>(input - 1)];
    }

    Route audioRoute(int input)
    {
        std::lock_guard lock{routeMu};
        return audioRoutes[static_cast<std::size_t>(input - 1)];
    }

    std::shared_ptr<Snap> snap(int input)
    {
        std::lock_guard lock{snapMu};
        return snaps[static_cast<std::size_t>(input - 1)];
    }

    void publish(int input, std::shared_ptr<Snap> next)
    {
        std::lock_guard lock{snapMu};
        snaps[static_cast<std::size_t>(input - 1)] = std::move(next);
    }

    void ensureDomain()
    {
        if (isMirrorDomain(config.outputDomainDir))
        {
            throw ConfigError("output domain is a fabrics mirror");
        }
        std::filesystem::create_directories(config.outputDomainDir);
        if (isMirrorDomain(config.outputDomainDir))
        {
            throw ConfigError("output domain is a fabrics mirror");
        }
        auto const ids = makeNmosIds(config.nmosSeed);
        domainId = config.outputDomainId.empty() ? ids.domain : config.outputDomainId;
        auto const defPath = std::filesystem::path(config.outputDomainDir) / "domain_def.json";
        if (!std::filesystem::exists(defPath))
        {
            std::ofstream out(defPath);
            out << "{\"id\":\"" << domainId << "\",\"label\":\"mxl-multiviewer\",\"description\":\"Multiviewer output domain\"}\n";
        }
        else if (auto const existing = readDomainId(config.outputDomainDir))
        {
            if (*existing != domainId)
            {
                logError("domain_id_mismatch", {{"path", defPath.string()}, {"file", *existing}, {"configured", domainId}});
            }
            domainId = *existing;
        }
        auto const options = std::filesystem::path(config.outputDomainDir) / "options.json";
        if (!std::filesystem::exists(options))
        {
            std::ofstream out(options);
            out << "{\"urn:x-mxl:option:history_duration/v1.0\":" << config.historyNs << "}\n";
        }
    }

    void readerMain(int input)
    {
        mxlInstance instance = nullptr;
        mxlFlowReader video = nullptr;
        mxlFlowReader audio = nullptr;
        // CUDA locks the reader's grain memory; unlock it before the reader unmaps it.
        auto const releaseVideo = [&] {
            cudaReleaseHostMemory();
            mxlReleaseFlowReader(instance, video);
        };
        std::string openKey;
        FlowMeta meta;
        std::string metaKey;
        int backoff = 250;
        AlarmSet alarms;
        PpmMeter meters[16];
        std::string prevState;
        auto const publishState = [&](std::shared_ptr<Snap> const& snap) {
            InputView view;
            view.index = input;
            view.video.enable = videoRoute(input).enable;
            view.video.domainId = videoRoute(input).domainId;
            view.video.flowId = videoRoute(input).flowId;
            view.video.senderId = videoRoute(input).senderId;
            view.video.state = snap->videoState;
            view.video.reason = snap->videoReason;
            view.video.label = snap->label;
            view.video.grains = snap->grainsRead;
            view.video.resyncs = snap->resyncs;
            view.audio.enable = audioRoute(input).enable;
            view.audio.domainId = audioRoute(input).domainId;
            view.audio.flowId = audioRoute(input).flowId;
            view.audio.senderId = audioRoute(input).senderId;
            view.audio.state = snap->audioState;
            view.audio.reason = snap->audioReason;
            view.audio.channels = static_cast<int>(snap->audio.channels.size());
            if (!snap->grains.empty())
            {
                auto const& grain = snap->grains.back();
                view.video.width = grain.width;
                view.video.height = grain.height;
                view.video.rateNum = grain.rateNum;
                view.video.rateDen = grain.rateDen;
                view.video.interlaced = grain.interlaced;
                view.video.format = formatLabel(grain.width, grain.height, grain.rateNum, grain.rateDen, grain.interlaced);
                auto const now = mxlGetTime();
                if (now > grain.origin)
                {
                    view.video.latencyMs = static_cast<double>(now - grain.origin) / 1e6;
                }
            }
            view.ppmDbfs = snap->ppm;
            view.rmsDbfs = snap->rms;
            view.clip = snap->clip;
            view.alarmNoSignal = snap->alarmNoSignal;
            view.alarmBlack = snap->alarmBlack;
            view.alarmFreeze = snap->alarmFreeze;
            view.alarmSilence = snap->alarmSilence;
            view.alarmClip = snap->alarmClip;
            view.alarmFormat = snap->alarmFormat;
            runtime.setInput(view);
            if (prevState != snap->videoState)
            {
                if (!prevState.empty())
                {
                    metrics.set("input_state", {{"input", std::to_string(input)}, {"kind", "video"}, {"state", prevState}}, 0);
                }
                metrics.set("input_state", {{"input", std::to_string(input)}, {"kind", "video"}, {"state", snap->videoState}}, 1);
                prevState = snap->videoState;
            }
            publish(input, snap);
        };

        while (run.load())
        {
            auto route = videoRoute(input);
            auto const sound = audioRoute(input);
            auto next = std::make_shared<Snap>(*snap(input));
            if (!route.enable || route.flowId.empty() || route.domainId.empty())
            {
                next->videoState = "not_routed";
                next->videoReason.clear();
                if (video != nullptr)
                {
                    releaseVideo();
                    video = nullptr;
                }
                if (audio != nullptr)
                {
                    mxlReleaseFlowReader(instance, audio);
                    audio = nullptr;
                }
                if (instance != nullptr)
                {
                    mxlDestroyInstance(instance);
                    instance = nullptr;
                }
                openKey.clear();
                publishState(next);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            auto const domain = resolveDomain(config.scanPath, route.domainId);
            if (!domain)
            {
                next->videoState = "waiting";
                next->videoReason = "domain_not_found";
                publishState(next);
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                backoff = std::min(5000, backoff * 2);
                continue;
            }
            auto const key = domain->path + "|" + route.flowId;
            if (instance == nullptr || openKey.substr(0, domain->path.size()) != domain->path)
            {
                if (video != nullptr)
                {
                    releaseVideo();
                    video = nullptr;
                }
                if (audio != nullptr)
                {
                    mxlReleaseFlowReader(instance, audio);
                    audio = nullptr;
                }
                if (instance != nullptr)
                {
                    mxlDestroyInstance(instance);
                }
                instance = mxlCreateInstance(domain->path.c_str(), nullptr);
                openKey.clear();
            }
            if (instance == nullptr)
            {
                next->videoState = "waiting";
                next->videoReason = "domain_not_found";
                publishState(next);
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                backoff = std::min(5000, backoff * 2);
                continue;
            }
            if (openKey != key)
            {
                if (video != nullptr)
                {
                    releaseVideo();
                    video = nullptr;
                }
                if (mxlCreateFlowReader(instance, route.flowId.c_str(), nullptr, &video) != MXL_STATUS_OK)
                {
                    next->videoState = "waiting";
                    next->videoReason = "flow_not_found";
                    publishState(next);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                    backoff = std::min(5000, backoff * 2);
                    continue;
                }
                openKey = key;
                backoff = 250;
            }
            if (sound.enable && !sound.flowId.empty())
            {
                auto const audioDomain = resolveDomain(config.scanPath, sound.domainId.empty() ? route.domainId : sound.domainId);
                if (audioDomain && audioDomain->path == domain->path && audio == nullptr)
                {
                    if (mxlCreateFlowReader(instance, sound.flowId.c_str(), nullptr, &audio) != MXL_STATUS_OK)
                    {
                        audio = nullptr;
                        next->audioState = "waiting";
                        next->audioReason = "flow_not_found";
                    }
                }
            }
            else if (audio != nullptr)
            {
                mxlReleaseFlowReader(instance, audio);
                audio = nullptr;
                next->audioState = "not_routed";
            }

            // A flow definition never changes for a flow id: parse it once per opened flow.
            if (metaKey != openKey)
            {
                meta = parseFlow(readFlowDef(instance, route.flowId));
                metaKey = meta.width > 0 ? openKey : std::string{};
            }
            mxlFlowInfo info{};
            if (mxlFlowReaderGetInfo(video, &info) != MXL_STATUS_OK)
            {
                next->videoState = "waiting";
                next->videoReason = "flow_not_found";
                publishState(next);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            mxlGrainInfo grain{};
            std::uint8_t* payload = nullptr;
            mxlStatus status = MXL_STATUS_OK;
            if (!next->grains.empty() && next->grains.back().index >= info.runtime.headIndex)
            {
                // Caught up: block until the next grain is committed instead of polling the head.
                status = mxlFlowReaderGetGrain(video, next->grains.back().index + 1, kGrainWaitNs, &grain, &payload);
            }
            else
            {
                status = mxlFlowReaderGetGrainNonBlocking(video, info.runtime.headIndex, &grain, &payload);
            }
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
            {
                ++next->resyncs;
                metrics.inc("input_resyncs_total", {{"input", std::to_string(input)}});
                status = mxlFlowReaderGetGrainNonBlocking(video, info.runtime.headIndex, &grain, &payload);
            }
            if (status == MXL_ERR_TIMEOUT)
            {
                publishState(next);
                continue;
            }
            if (status != MXL_STATUS_OK || payload == nullptr || (grain.flags & MXL_GRAIN_FLAG_INVALID) != 0)
            {
                if (next->grains.empty())
                {
                    next->videoState = "no_signal";
                }
                publishState(next);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (!next->grains.empty() && next->grains.back().index == grain.index)
            {
                publishState(next);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            int const width = meta.width > 0 ? meta.width : 1920;
            int const height = meta.height > 0 ? meta.height : 1080;
            int const rowBytes = static_cast<int>(v210RowBytes(width));
            auto const fillBytes = static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(height);
            std::uint8_t const* alphaKey = meta.alpha && grain.grainSize > fillBytes ? payload + fillBytes : nullptr;
            SavedGrain saved;
            if (useCuda.load())
            {
                // Straight from the MXL grain to the GPU; the CPU neither copies nor unpacks it.
                saved.gpu = cudaUploadFrame(payload, rowBytes, alphaKey, static_cast<int>(alpha10RowBytes(width)), width, height);
            }
            if (saved.gpu == nullptr)
            {
                auto frame = std::make_shared<Frame422>();
                frame->allocate(width, height, meta.alpha);
                unpackV210(payload, rowBytes, *frame);
                if (alphaKey != nullptr)
                {
                    unpackAlpha10(alphaKey, static_cast<int>(alpha10RowBytes(width)), *frame);
                }
                saved.frame = std::move(frame);
            }
            saved.index = grain.index;
            mxlRational rate{meta.rateNum, meta.rateDen};
            saved.origin = mxlIndexToTimestamp(&rate, grain.index);
            saved.width = width;
            saved.height = height;
            saved.rateNum = meta.rateNum;
            saved.rateDen = meta.rateDen;
            saved.interlaced = meta.interlaced;
            saved.alpha = meta.alpha;
            saved.mediaType = meta.mediaType;
            next->grains.push_back(saved);
            if (next->grains.size() > 4)
            {
                next->grains.erase(next->grains.begin());
            }
            ++next->grainsRead;
            metrics.inc("grains_read_total", {{"input", std::to_string(input)}});
            next->videoState = "running";
            next->videoReason.clear();
            next->label = meta.label.empty() ? next->label : meta.label;
            next->senderId = route.senderId;
            if (audio != nullptr)
            {
                mxlRational audioRate{48000, 1};
                std::uint64_t const sampleEnd = mxlTimestampToIndex(&audioRate, saved.origin) + 1;
                std::size_t maxRead = 0;
                mxlFlowReaderGetMaxReadLengthSamples(audio, &maxRead);
                std::size_t const count = std::min<std::size_t>(maxRead == 0 ? 960 : maxRead, 2000);
                mxlWrappedMultiBufferSlice slice{};
                if (count > 0 && mxlFlowReaderGetSamplesNonBlocking(audio, sampleEnd, count, &slice) == MXL_STATUS_OK)
                {
                    pushAudio(next->audio, slice, count, sampleEnd);
                    next->audioState = "running";
                    PpmConfig ppm;
                    ppm.clipLinear = config.clipLinear;
                    for (std::size_t ch = 0; ch < next->audio.channels.size() && ch < 16; ++ch)
                    {
                        auto const& samples = next->audio.channels[ch];
                        int const take = std::min<int>(static_cast<int>(samples.size()), 960);
                        meters[ch].process(samples.data() + samples.size() - static_cast<std::size_t>(take), take, 48000.0, ppm);
                        next->ppm[ch] = meters[ch].levelDbfs();
                        next->rms[ch] = meters[ch].rmsDbfs();
                        next->clip[ch] = meters[ch].clip;
                    }
                }
            }
            auto const nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            double sum = 0;
            int samples = 0;
            // On the CUDA backend the CPU has no unpacked frame: read the same luma
            // samples from the packed grain.
            for (int i = 0; i < width * height; i += 32)
            {
                sum += saved.frame ? saved.frame->y[static_cast<std::size_t>(i)] : v210Luma(payload, rowBytes, width, i);
                ++samples;
            }
            bool const black = samples > 0 && (sum / samples) <= config.blackY;
            auto const hash = saved.frame ? lumaHash(*saved.frame) : lumaHash(payload, rowBytes, width, height);
            bool const freeze = alarms.haveHash && hash == alarms.lastHash;
            alarms.lastHash = hash;
            alarms.haveHash = true;
            bool silence = false;
            if (audio != nullptr)
            {
                silence = true;
                for (double dbfs : next->ppm)
                {
                    if (dbfs > config.silenceDbfs)
                    {
                        silence = false;
                    }
                }
            }
            bool clip = false;
            for (bool flag : next->clip)
            {
                clip = clip || flag;
            }
            bool const formatBad = width > 3840 || height > 2160 || !allowedRate(meta.rateNum, meta.rateDen) ||
                                   (meta.mediaType != "video/v210" && meta.mediaType != "video/v210a" && !meta.mediaType.empty());
            auto bump = [&](char const* name, Debounce& debounce, bool raw, bool& flag) {
                if (debounce.update(raw, nowMs, config.alarmDebounceMs, config.alarmClearMs) && debounce.active)
                {
                    metrics.inc("alarms_total", {{"input", std::to_string(input)}, {"name", name}});
                }
                flag = debounce.active;
                metrics.set("alarms", {{"input", std::to_string(input)}, {"name", name}}, flag ? 1 : 0);
            };
            bump("no_signal", alarms.noSignal, false, next->alarmNoSignal);
            bump("black", alarms.black, black, next->alarmBlack);
            bump("freeze", alarms.freeze, freeze, next->alarmFreeze);
            bump("silence", alarms.silence, silence, next->alarmSilence);
            bump("clip", alarms.clip, clip, next->alarmClip);
            bump("format_mismatch", alarms.formatMismatch, formatBad, next->alarmFormat);
            publishState(next);
        }
        if (video != nullptr && instance != nullptr)
        {
            releaseVideo();
        }
        if (audio != nullptr && instance != nullptr)
        {
            mxlReleaseFlowReader(instance, audio);
        }
        if (instance != nullptr)
        {
            mxlDestroyInstance(instance);
        }
    }

    // The overlay items of one head (UMD, tally, meters, alarms, clocks), from input
    // snapshots and runtime state only, so the overlay thread can build them.
    std::vector<OverlayTile> overlayTiles(std::vector<Tile> const& tiles, VideoFormat const& format)
    {
        std::vector<OverlayTile> drawn;
        for (auto const& tile : tiles)
        {
            OverlayTile item;
            item.rect = rectToPixels(tile.rect, format.width, format.height);
            item.umd = tile.umd && tile.content == TileContent::Input;
            auto const current = tile.content == TileContent::Input ? snap(tile.input) : nullptr;
            auto const viewIn = tile.content == TileContent::Input ? runtime.inputs()[static_cast<std::size_t>(tile.input - 1)] : InputView{};
            if (tile.umdSource == UmdSource::Manual)
            {
                item.umdText = tile.umdText;
            }
            else if (tile.umdSource == UmdSource::Tsl)
            {
                item.umdText = viewIn.tslText.empty() ? tile.umdText : viewIn.tslText;
            }
            else if (current != nullptr)
            {
                item.umdText = current->label.empty() ? ("MV In " + std::to_string(tile.input)) : current->label;
            }
            item.umdPosition = tile.umdPosition;
            item.umdFont = std::max(8, tile.umdFont * format.height / 1080);
            item.umdBg = parseHexColor(tile.umdBg, {0, 0, 0, 192});
            item.tally = viewIn.tally;
            item.tallyBorder = tile.tallyBorder;
            item.tallyLamp = tile.tallyLamp;
            item.umdFg = tallyRgba(item.tally);
            item.bars = tile.audioBars;
            item.showRms = tile.audioBarRms;
            item.barChannels = tile.audioBarChannels;
            item.barsPosition = tile.audioBarPosition;
            item.zoneGreen = tile.zoneGreen;
            item.zoneAmber = tile.zoneAmber;
            if (current != nullptr)
            {
                for (int c = 0; c < 16; ++c)
                {
                    int const src = tile.audioBarFirst + c;
                    item.ppmDbfs[c] = src < 16 ? current->ppm[static_cast<std::size_t>(src)] : -120;
                    item.rmsDbfs[c] = src < 16 ? current->rms[static_cast<std::size_t>(src)] : -120;
                    item.clip[c] = src < 16 && current->clip[static_cast<std::size_t>(src)];
                }
                if (tile.formatLabel && !current->grains.empty())
                {
                    auto const& grain = current->grains.back();
                    item.formatText = formatLabel(grain.width, grain.height, grain.rateNum, grain.rateDen, grain.interlaced);
                }
                if (tile.latency && !current->grains.empty())
                {
                    auto const now = mxlGetTime();
                    auto const origin = current->grains.back().origin;
                    item.latencyText = std::to_string(static_cast<int>((now > origin ? now - origin : 0) / 1000000ull)) + " ms";
                }
                if (current->alarmNoSignal)
                {
                    item.badge = "NO SIGNAL";
                }
                else if (current->alarmBlack)
                {
                    item.badge = "BLACK";
                }
                else if (current->alarmFreeze)
                {
                    item.badge = "FREEZE";
                }
                else if (current->alarmClip)
                {
                    item.badge = "CLIP";
                }
                else if (current->alarmSilence)
                {
                    item.badge = "SILENCE";
                }
                else if (current->alarmFormat)
                {
                    item.badge = "FORMAT";
                }
            }
            item.safeArea = tile.safeArea;
            item.centre = tile.centre;
            item.aspectMarkers = tile.aspectMarkers;
            if (tile.content == TileContent::Label)
            {
                item.labelText = tile.labelText;
            }
            if (tile.content == TileContent::Clock)
            {
                item.clock = true;
                item.analogue = tile.clockStyle == ClockStyle::Analogue;
                std::uint64_t const clockNs = tile.clockZone == ClockZone::Utc ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) : mxlGetTime();
                std::time_t const sec = static_cast<std::time_t>(clockNs / 1000000000ull);
                std::tm tm{};
                if (tile.clockZone == ClockZone::Local)
                {
                    localtime_r(&sec, &tm);
                }
                else
                {
                    gmtime_r(&sec, &tm);
                }
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
                item.clockText = buf;
                item.clockHour = tm.tm_hour;
                item.clockMinute = tm.tm_min;
                item.clockSecond = tm.tm_sec;
                int rateNum = 0;
                int rateDen = 1;
                if (parseRateToken(tile.timecodeRate, rateNum, rateDen))
                {
                    item.timecodeText = formatTimecode(mxlGetTime(), rateNum, rateDen);
                }
            }
            drawn.push_back(std::move(item));
        }
        return drawn;
    }

    std::shared_ptr<OverlayFrame const> overlayOf(int head)
    {
        std::lock_guard<std::mutex> lock(overlayMu);
        return overlays[static_cast<std::size_t>(head - 1)];
    }

    // Redraws one head's overlay at MV_OVERLAY_HZ on its own thread. Drawing took up to
    // 15 ms per redraw on the output thread and made frames late; compose now only
    // takes the latest finished overlay.
    void overlayMain(int head)
    {
        Overlay overlay;
        std::uint64_t version = 0;
        std::shared_ptr<OverlayFrame const> last;
        auto due = std::chrono::steady_clock::now();
        while (run.load())
        {
            auto const format = runtime.headFormat(head);
            auto const layout = layouts.layout(runtime.headLayout(head));
            std::vector<Tile> tiles = layout ? layout->tiles : std::vector<Tile>{};
            std::sort(tiles.begin(), tiles.end(), [](Tile const& a, Tile const& b) { return a.z < b.z; });
            if (overlay.width != format.width || overlay.height != format.height)
            {
                overlay.resize(format.width, format.height);
            }
            overlay.clear();
            renderOverlay(overlay, overlayTiles(tiles, format));
            auto frame = std::make_shared<OverlayFrame>();
            frame->rgba = overlay.rgba;
            frame->width = overlay.width;
            frame->height = overlay.height;
            frame->version = ++version;
            if (useCuda.load())
            {
                // Only the areas that changed go over PCIe; compose then never waits for overlay copies.
                std::vector<CudaRect> changes;
                for (auto const& area : overlayChanges(last ? last->rgba : std::vector<std::uint8_t>{}, frame->rgba, frame->width, frame->height))
                {
                    changes.push_back({area.x, area.y, area.w, area.h});
                }
                frame->device = cudaUploadOverlay(last ? last->device : nullptr, frame->rgba.data(), frame->width, frame->height, changes.data(),
                    static_cast<int>(changes.size()));
            }
            last = frame;
            {
                std::lock_guard<std::mutex> lock(overlayMu);
                overlays[static_cast<std::size_t>(head - 1)] = std::move(frame);
            }
            due += std::chrono::milliseconds(1000 / std::max(1, config.overlayHz));
            auto const now = std::chrono::steady_clock::now();
            if (due < now)
            {
                due = now;
            }
            std::this_thread::sleep_until(due);
        }
    }

    void headMain(int head)
    {
        auto instance = mxlCreateInstance(config.outputDomainDir.c_str(), nullptr);
        if (instance == nullptr)
        {
            logError("output_domain_open_failed", {{"head", std::to_string(head)}});
            return;
        }
        mxlGarbageCollectFlows(instance);
        auto format = runtime.headFormat(head);
        std::string videoFlow = makeNmosIds(config.nmosSeed).videoFlow(head, format.token());
        std::string audioFlow = makeNmosIds(config.nmosSeed).audioFlow(head, runtime.headAudioChannels(head));
        mxlFlowWriter videoWriter = nullptr;
        mxlFlowWriter audioWriter = nullptr;
        auto openWriters = [&] {
            if (videoWriter != nullptr)
            {
                // CUDA locks the writer's grain memory; unlock it before the writer unmaps it.
                cudaReleaseHostMemory();
                mxlReleaseFlowWriter(instance, videoWriter);
                videoWriter = nullptr;
            }
            if (audioWriter != nullptr)
            {
                mxlReleaseFlowWriter(instance, audioWriter);
                audioWriter = nullptr;
            }
            format = runtime.headFormat(head);
            videoFlow = makeNmosIds(config.nmosSeed).videoFlow(head, format.token());
            int const channels = runtime.headAudioChannels(head);
            audioFlow = makeNmosIds(config.nmosSeed).audioFlow(head, channels);
            auto const videoDef = videoFlowDefinition(videoFlow, "MV Out " + std::to_string(head) + " Video", "MV Out " + std::to_string(head) + ":Video",
                format.width, format.height, format.rateNum, format.rateDen);
            if (mxlCreateFlowWriter(instance, videoDef.c_str(), "{\"maxCommitBatchSizeHint\":1}", &videoWriter, nullptr, nullptr) != MXL_STATUS_OK)
            {
                videoWriter = nullptr;
            }
            if (channels == 2 || channels == 16)
            {
                auto const audioDef = audioFlowDefinition(audioFlow, "MV Out " + std::to_string(head) + " Audio", "MV Out " + std::to_string(head) + ":Audio", channels);
                if (mxlCreateFlowWriter(instance, audioDef.c_str(), "{\"maxCommitBatchSizeHint\":480}", &audioWriter, nullptr, nullptr) != MXL_STATUS_OK)
                {
                    audioWriter = nullptr;
                }
            }
            if (onFlow)
            {
                onFlow(head, videoFlow, audioWriter != nullptr ? audioFlow : std::string{}, format);
            }
        };
        openWriters();
        std::string layoutToken = runtime.headLayout(head);
        std::string formatToken = format.token();
        mxlRational rate{format.rateNum, format.rateDen};
        std::uint64_t index = mxlGetCurrentIndex(&rate);
        Frame422 canvas;
        canvas.allocate(format.width, format.height, false);
        Frame422 coveredBackground;
        // Bumped whenever coveredBackground is redrawn: the GPU keeps its copy until then.
        std::uint64_t backgroundVersion = 0;
        // The output when no grain is open, and the CPU compose target.
        std::vector<std::uint8_t> packedOut;
        int cudaFailures = 0;
        int previewDiv = 0;
        OutputView view = runtime.output(head);
        view.domainId = domainId;
        view.backend = "cpu";
        view.videoFlowId = videoFlow;
        view.audioFlowId = audioWriter != nullptr ? audioFlow : "";

        while (run.load())
        {
            auto const nowIndex = mxlGetCurrentIndex(&rate);
            if (nowIndex > index && nowIndex != MXL_UNDEFINED_INDEX)
            {
                auto const skipped = nowIndex - index;
                view.missed += skipped;
                view.late += skipped;
                metrics.inc("output_frames_missed_total", {{"head", std::to_string(head)}}, static_cast<double>(skipped));
                metrics.inc("output_frames_late_total", {{"head", std::to_string(head)}}, static_cast<double>(skipped));
                index = nowIndex;
            }
            auto const when = mxlIndexToTimestamp(&rate, index);
            if (mxlGetTime() + 1000000 < when)
            {
                mxlSleepUntil(when);
            }
            if (runtime.headFormat(head).token() != formatToken || runtime.headLayout(head) != layoutToken)
            {
                if (runtime.headFormat(head).token() != formatToken)
                {
                    openWriters();
                    format = runtime.headFormat(head);
                    formatToken = format.token();
                    rate = mxlRational{format.rateNum, format.rateDen};
                    canvas.allocate(format.width, format.height, false);
                    index = mxlGetCurrentIndex(&rate);
                    view.videoFlowId = videoFlow;
                    view.audioFlowId = audioWriter != nullptr ? audioFlow : "";
                    view.format = formatToken;
                }
                layoutToken = runtime.headLayout(head);
                view.layout = layoutToken;
            }
            auto const started = std::chrono::steady_clock::now();
            auto layout = layouts.layout(layoutToken);
            std::vector<Tile> tiles = layout ? layout->tiles : std::vector<Tile>{};
            std::sort(tiles.begin(), tiles.end(), [](Tile const& a, Tile const& b) { return a.z < b.z; });
            struct Source
            {
                Placement place;
                std::shared_ptr<Frame422> frame;
                // Held until compose returns, so the device frame is not recycled under it.
                std::shared_ptr<CudaFrame const> gpu;
                int width = 0;
                int height = 0;
                bool bob = false;
            };
            std::vector<Source> sources;
            std::uint64_t const frameDur = mxlIndexToTimestamp(&rate, 1);
            std::uint64_t const offset = frameDur * static_cast<std::uint64_t>(config.inputOffsetGrains);
            std::uint64_t const outputTime = when;
            std::uint64_t const target = outputTime > offset ? outputTime - offset : 0;
            for (auto const& tile : tiles)
            {
                if (tile.content != TileContent::Input)
                {
                    continue;
                }
                auto const current = snap(tile.input);
                SavedGrain const* best = nullptr;
                for (auto const& grain : current->grains)
                {
                    if (grain.origin <= target && (best == nullptr || grain.origin > best->origin))
                    {
                        best = &grain;
                    }
                }
                if (best != nullptr && target > best->origin + frameDur * 2)
                {
                    runtime.addLate(tile.input);
                    metrics.inc("input_late_grains_total", {{"input", std::to_string(tile.input)}});
                }
                auto const px = rectToPixels(tile.rect, format.width, format.height);
                Source source;
                source.place = best != nullptr ? placeTile(px, best->width, best->height, tile.scale) : Placement{};
                if (best == nullptr)
                {
                    source.place.dst = px;
                }
                if (best != nullptr)
                {
                    source.frame = best->frame;
                    source.gpu = best->gpu;
                    source.width = best->width;
                    source.height = best->height;
                    source.bob = best->interlaced;
                }
                sources.push_back(std::move(source));
            }
            if (background.width > 0 && (coveredBackground.width != format.width || coveredBackground.height != format.height))
            {
                coveredBackground.allocate(format.width, format.height, false);
                coverFrame(coveredBackground, background);
                ++backgroundVersion;
            }
            auto const overlayFrame = overlayOf(head);
            bool const withOverlay = overlayFrame != nullptr && overlayFrame->width == format.width && overlayFrame->height == format.height;
            packedOut.resize(static_cast<std::size_t>(v210RowBytes(format.width)) * static_cast<std::size_t>(format.height));
            // Compose straight into the open MXL grain; packedOut only when none is open.
            mxlGrainInfo outGrain{};
            std::uint8_t* outPayload = nullptr;
            bool const grainOpen =
                videoWriter != nullptr && mxlFlowWriterOpenGrain(videoWriter, index, &outGrain, &outPayload) == MXL_STATUS_OK && outPayload != nullptr;
            std::uint8_t* const out = grainOpen ? outPayload : packedOut.data();
            bool cudaFrame = false;
            if (useCuda.load())
            {
                std::vector<CudaTileView> views;
                views.reserve(sources.size());
                for (auto const& source : sources)
                {
                    CudaTileView viewTile;
                    viewTile.dstX = source.place.dst.x;
                    viewTile.dstY = source.place.dst.y;
                    viewTile.dstW = source.place.dst.w;
                    viewTile.dstH = source.place.dst.h;
                    viewTile.srcX = source.place.srcX;
                    viewTile.srcY = source.place.srcY;
                    viewTile.srcW = source.place.srcW;
                    viewTile.srcH = source.place.srcH;
                    viewTile.bob = source.bob;
                    if (source.frame == nullptr && source.gpu == nullptr)
                    {
                        viewTile.solid = true;
                    }
                    else
                    {
                        viewTile.srcWidth = source.width;
                        viewTile.srcHeight = source.height;
                        viewTile.frame = source.gpu.get();
                        if (source.frame != nullptr)
                        {
                            viewTile.y = source.frame->y.data();
                            viewTile.cb = source.frame->cb.data();
                            viewTile.cr = source.frame->cr.data();
                            if (source.frame->hasAlpha && !source.frame->a.empty())
                            {
                                viewTile.a = source.frame->a.data();
                            }
                        }
                    }
                    views.push_back(viewTile);
                }
                CudaComposeDesc desc;
                desc.width = format.width;
                desc.height = format.height;
                desc.bgY = 64;
                desc.bgCb = 512;
                desc.bgCr = 512;
                if (coveredBackground.width == format.width && coveredBackground.height == format.height)
                {
                    desc.backgroundY = coveredBackground.y.data();
                    desc.backgroundCb = coveredBackground.cb.data();
                    desc.backgroundCr = coveredBackground.cr.data();
                    desc.backgroundWidth = coveredBackground.width;
                    desc.backgroundHeight = coveredBackground.height;
                    desc.backgroundVersion = backgroundVersion;
                }
                desc.tiles = views.data();
                desc.tileCount = static_cast<int>(views.size());
                desc.rgba = withOverlay ? overlayFrame->rgba.data() : nullptr;
                desc.rgbaStride = format.width * 4;
                desc.rgbaVersion = withOverlay ? overlayFrame->version : 0;
                desc.overlay = withOverlay ? overlayFrame->device.get() : nullptr;
                desc.v210Out = out;
                desc.v210RowBytes = static_cast<int>(v210RowBytes(format.width));
                desc.v210OutIsGrain = grainOpen;
                CudaComposeTiming timing;
                desc.timing = &timing;
                cudaFrame = cudaComposeFrame(desc) == CudaComposeStatus::Ok;
                if (cudaFrame)
                {
                    cudaFailures = 0;
                    std::pair<char const*, float> const stages[] = {{"background", timing.background}, {"tiles", timing.tiles}, {"overlay", timing.overlay},
                        {"pack", timing.pack}, {"download", timing.download}};
                    for (auto const& [stage, ms] : stages)
                    {
                        metrics.observe("compose_gpu_seconds", {{"head", std::to_string(head)}, {"stage", stage}}, ms / 1000.0);
                    }
                }
                else
                {
                    static std::atomic<int> logged{0};
                    if (logged.fetch_add(1) == 0)
                    {
                        logError("cuda_compose_fallback", {{"head", std::to_string(head)}});
                    }
                    // Inputs on the GPU have no CPU copy, so the CPU fallback shows them
                    // black. If CUDA keeps failing (about half a second), the inputs go
                    // back to unpacking on the CPU.
                    if (++cudaFailures >= 25 && useCuda.exchange(false))
                    {
                        logError("cuda_disabled", {{"head", std::to_string(head)}});
                    }
                }
            }
            if (!cudaFrame)
            {
                struct Job
                {
                    PixelRect dst;
                    Frame422 image;
                };
                std::vector<std::future<Job>> jobs;
                for (auto const& source : sources)
                {
                    jobs.push_back(std::async(std::launch::async, [source] {
                        Job job;
                        job.dst = source.place.dst;
                        job.image.allocate(std::max(2, job.dst.w), std::max(1, job.dst.h), false);
                        if (source.frame == nullptr)
                        {
                            job.image.fill(64, 512, 512);
                            return job;
                        }
                        Placement local = source.place;
                        local.dst = {0, 0, job.image.width, job.image.height};
                        scaleInto(job.image, local, *source.frame, source.bob);
                        return job;
                    }));
                }
                canvas.fill(64, 512, 512);
                if (coveredBackground.width == canvas.width && coveredBackground.height == canvas.height)
                {
                    canvas.y = coveredBackground.y;
                    canvas.cb = coveredBackground.cb;
                    canvas.cr = coveredBackground.cr;
                }
                for (auto& job : jobs)
                {
                    auto piece = job.get();
                    blit(canvas, piece.dst, piece.image);
                }
                if (withOverlay)
                {
                    blendStraightRgba(canvas, overlayFrame->rgba.data(), format.width * 4);
                }
                packV210(canvas, out, static_cast<int>(v210RowBytes(format.width)));
            }
            if (videoWriter != nullptr)
            {
                if (grainOpen)
                {
                    outGrain.validSlices = outGrain.totalSlices;
                    outGrain.flags = 0;
                    mxlFlowWriterCommitGrain(videoWriter, &outGrain);
                    ++view.frames;
                    metrics.inc("output_frames_total", {{"head", std::to_string(head)}});
                }
                else
                {
                    ++view.missed;
                    metrics.inc("output_frames_missed_total", {{"head", std::to_string(head)}});
                }
            }
            int const channels = runtime.headAudioChannels(head);
            int const follow = runtime.headAudioFollow(head);
            if (audioWriter != nullptr && follow >= 1 && (channels == 2 || channels == 16))
            {
                auto const source = snap(follow);
                std::uint64_t const samples = std::max<std::uint64_t>(1, (48000ull * static_cast<std::uint64_t>(format.rateDen)) / static_cast<std::uint64_t>(std::max(1, format.rateNum)));
                mxlRational audioRate{48000, 1};
                std::uint64_t const end = mxlTimestampToIndex(&audioRate, target == 0 ? outputTime : target) + samples;
                std::size_t maxWrite = samples;
                mxlFlowWriterGetMaxWriteLengthSamples(audioWriter, &maxWrite);
                std::size_t const count = std::min<std::size_t>(samples, maxWrite == 0 ? samples : maxWrite);
                mxlMutableWrappedMultiBufferSlice slice{};
                if (count > 0 && mxlFlowWriterOpenSamples(audioWriter, end, count, &slice) == MXL_STATUS_OK)
                {
                    for (int ch = 0; ch < channels && ch < static_cast<int>(slice.count); ++ch)
                    {
                        std::size_t filled = 0;
                        for (int frag = 0; frag < 2 && filled < count; ++frag)
                        {
                            auto* pointer = static_cast<char*>(slice.base.fragments[frag].pointer);
                            if (pointer == nullptr)
                            {
                                continue;
                            }
                            auto* dst = reinterpret_cast<float*>(pointer + static_cast<std::size_t>(ch) * slice.stride);
                            std::size_t const room = slice.base.fragments[frag].size / sizeof(float);
                            std::size_t const take = std::min(room, count - filled);
                            std::fill(dst, dst + take, 0.f);
                            if (ch < static_cast<int>(source->audio.channels.size()))
                            {
                                auto const& src = source->audio.channels[static_cast<std::size_t>(ch)];
                                std::uint64_t const srcBegin = source->audio.first;
                                std::uint64_t const want = (end - count) + filled;
                                if (want >= srcBegin && want - srcBegin < src.size())
                                {
                                    std::size_t const offset = static_cast<std::size_t>(want - srcBegin);
                                    std::size_t const n = std::min(take, src.size() - offset);
                                    std::memcpy(dst, src.data() + offset, n * sizeof(float));
                                }
                            }
                            filled += take;
                        }
                    }
                    mxlFlowWriterCommitSamples(audioWriter);
                }
            }
            auto const elapsed = std::chrono::steady_clock::now() - started;
            view.backend = cudaFrame ? "cuda" : "cpu";
            view.composeMs = std::chrono::duration<double, std::milli>(elapsed).count();
            metrics.observe("compose_seconds", {{"head", std::to_string(head)}, {"backend", view.backend}}, std::chrono::duration<double>(elapsed).count());
            if (cudaFrame)
            {
                std::uint64_t freeBytes = 0;
                std::uint64_t totalBytes = 0;
                cudaDeviceMemory(&freeBytes, &totalBytes);
                metrics.set("gpu_memory_bytes", {}, totalBytes >= freeBytes ? static_cast<double>(totalBytes - freeBytes) : 0);
            }
            auto const after = mxlGetTime();
            if (after > mxlIndexToTimestamp(&rate, index + 1))
            {
                ++view.late;
                metrics.inc("output_frames_late_total", {{"head", std::to_string(head)}});
            }
            runtime.setOutput(view);
            runtime.touch();
            if (head == 1 && ++previewDiv >= std::max(1, format.rateNum / std::max(1, format.rateDen) / std::max(1, config.previewFps)))
            {
                previewDiv = 0;
                // CUDA: sample the written grain at preview size; a full unpack of the
                // output on this thread made frames late.
                runtime.setPreview(cudaFrame ? encodePreviewJpeg(out, static_cast<int>(v210RowBytes(format.width)), format.width, format.height, config.previewWidth, 60)
                                             : encodePreviewJpeg(canvas, config.previewWidth, 60));
            }
            ++index;
        }
        if (videoWriter != nullptr)
        {
            cudaReleaseHostMemory();
            mxlReleaseFlowWriter(instance, videoWriter);
        }
        if (audioWriter != nullptr)
        {
            mxlReleaseFlowWriter(instance, audioWriter);
        }
        mxlDestroyInstance(instance);
    }

    void openTsl()
    {
        if (!config.tslEnable)
        {
            return;
        }
        tslUdp = ::socket(AF_INET, SOCK_DGRAM, 0);
        tslListen = ::socket(AF_INET, SOCK_STREAM, 0);
        if (tslUdp < 0 || tslListen < 0)
        {
            throw std::runtime_error("TSL socket failed");
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<std::uint16_t>(config.tslUdpPort));
        if (::bind(tslUdp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            throw std::runtime_error("TSL UDP bind failed on port " + std::to_string(config.tslUdpPort));
        }
        int one = 1;
        ::setsockopt(tslListen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        addr.sin_port = htons(static_cast<std::uint16_t>(config.tslTcpPort));
        if (::bind(tslListen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            throw std::runtime_error("TSL TCP bind failed on port " + std::to_string(config.tslTcpPort));
        }
        if (::listen(tslListen, 8) != 0)
        {
            throw std::runtime_error("TSL TCP listen failed on port " + std::to_string(config.tslTcpPort));
        }
    }

    void persistRoutes()
    {
        if (loadingRoutes || config.stateDir.empty())
        {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(config.stateDir, ec);
        std::ostringstream out;
        out << "{\"routes\":[";
        bool first = true;
        auto write = [&](int input, bool video, Route const& route) {
            if (!route.enable && route.domainId.empty() && route.flowId.empty() && route.senderId.empty())
            {
                return;
            }
            if (!first)
            {
                out << ',';
            }
            first = false;
            out << "{\"input\":" << input << ",\"video\":" << (video ? "true" : "false") << ",\"enable\":" << (route.enable ? "true" : "false")
                << ",\"domain_id\":\"" << route.domainId << "\",\"flow_id\":\"" << route.flowId << "\",\"sender_id\":\"" << route.senderId << "\"}";
        };
        for (int input = 1; input <= config.maxInputs; ++input)
        {
            write(input, true, videoRoutes[static_cast<std::size_t>(input - 1)]);
            write(input, false, audioRoutes[static_cast<std::size_t>(input - 1)]);
        }
        out << "]}";
        auto const path = std::filesystem::path(config.stateDir) / "routes.json";
        auto const tmp = path.string() + ".tmp";
        std::ofstream file(tmp, std::ios::trunc);
        if (!file)
        {
            return;
        }
        file << out.str();
        file.close();
        std::filesystem::rename(tmp, path, ec);
    }

    void loadRoutes()
    {
        auto const path = std::filesystem::path(config.stateDir) / "routes.json";
        std::ifstream in(path);
        if (!in)
        {
            return;
        }
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        auto const root = json::parse(body, error);
        if (!error.empty() || !root.is<picojson::object>())
        {
            return;
        }
        auto const routes = root.get<picojson::object>().find("routes");
        if (routes == root.get<picojson::object>().end() || !routes->second.is<picojson::array>())
        {
            return;
        }
        loadingRoutes = true;
        for (auto const& item : routes->second.get<picojson::array>())
        {
            if (!item.is<picojson::object>())
            {
                continue;
            }
            auto const& obj = item.get<picojson::object>();
            auto num = [&](char const* key) {
                auto const it = obj.find(key);
                return it != obj.end() && it->second.is<double>() ? static_cast<int>(it->second.get<double>()) : 0;
            };
            auto flag = [&](char const* key) {
                auto const it = obj.find(key);
                return it != obj.end() && it->second.is<bool>() && it->second.get<bool>();
            };
            auto str = [&](char const* key) {
                auto const it = obj.find(key);
                return it != obj.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string{};
            };
            int const input = num("input");
            if (input < 1 || input > config.maxInputs)
            {
                continue;
            }
            auto& route = flag("video") ? videoRoutes[static_cast<std::size_t>(input - 1)] : audioRoutes[static_cast<std::size_t>(input - 1)];
            route.enable = flag("enable");
            route.domainId = str("domain_id");
            route.flowId = str("flow_id");
            route.senderId = str("sender_id");
        }
        loadingRoutes = false;
    }

    void tslMain()
    {
        auto apply = [&](TslMessage const& message, char const* transport) {
            if (!message.error.empty())
            {
                return;
            }
            metrics.inc("tsl_messages_total", {{"transport", transport}});
            for (auto const& display : message.displays)
            {
                if (config.tslScreen >= 0 && display.screen != config.tslScreen)
                {
                    continue;
                }
                int const input = inputForDisplay(config.tslMap, display.index);
                if (input >= 1 && input <= config.maxInputs)
                {
                    runtime.setTally(input, display.textValue, effectiveTally(display));
                }
            }
        };
        struct TslClient
        {
            int fd = -1;
            std::vector<std::uint8_t> buffer;
        };
        std::vector<TslClient> clients;
        while (run.load())
        {
            fd_set fds;
            FD_ZERO(&fds);
            int maxFd = -1;
            if (tslUdp >= 0)
            {
                FD_SET(tslUdp, &fds);
                maxFd = std::max(maxFd, tslUdp);
            }
            if (tslListen >= 0)
            {
                FD_SET(tslListen, &fds);
                maxFd = std::max(maxFd, tslListen);
            }
            std::vector<int> watched;
            watched.reserve(clients.size());
            for (auto const& client : clients)
            {
                FD_SET(client.fd, &fds);
                watched.push_back(client.fd);
                maxFd = std::max(maxFd, client.fd);
            }
            if (maxFd < 0)
            {
                break;
            }
            timeval tv{};
            tv.tv_usec = 200000;
            if (::select(maxFd + 1, &fds, nullptr, nullptr, &tv) <= 0)
            {
                continue;
            }
            if (tslUdp >= 0 && FD_ISSET(tslUdp, &fds))
            {
                std::uint8_t buffer[2048];
                auto const n = ::recv(tslUdp, buffer, sizeof(buffer), 0);
                if (n > 0)
                {
                    bool ok = true;
                    auto const body = unwrapDle(buffer, static_cast<std::size_t>(n), ok);
                    if (ok && body.size() >= 6 && (body.size() < 18 || body[0] != 0 || config.tslV31 == false || body.size() != 18))
                    {
                        apply(parseTsl5(body.data(), body.size()), "udp");
                    }
                    if (config.tslV31 && n >= 18)
                    {
                        apply(parseTsl31(buffer, static_cast<std::size_t>(n)), "udp");
                    }
                }
            }
            if (tslListen >= 0 && FD_ISSET(tslListen, &fds))
            {
                int const client = ::accept(tslListen, nullptr, nullptr);
                if (client >= 0)
                {
                    if (static_cast<int>(clients.size()) >= 8)
                    {
                        ::close(client);
                    }
                    else
                    {
                        clients.push_back(TslClient{client, {}});
                    }
                }
            }
            for (std::size_t i = 0; i < clients.size();)
            {
                if (std::find(watched.begin(), watched.end(), clients[i].fd) == watched.end() || !FD_ISSET(clients[i].fd, &fds))
                {
                    ++i;
                    continue;
                }
                std::uint8_t chunk[2048];
                auto const n = ::recv(clients[i].fd, chunk, sizeof(chunk), 0);
                if (n <= 0)
                {
                    ::close(clients[i].fd);
                    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                auto& buffer = clients[i].buffer;
                buffer.insert(buffer.end(), chunk, chunk + n);
                if (buffer.size() > 1024 * 1024)
                {
                    buffer.clear();
                }
                for (auto const& body : pullTslFrames(buffer))
                {
                    apply(parseTsl5(body.data(), body.size()), "tcp");
                }
                ++i;
            }
        }
        for (auto const& client : clients)
        {
            ::close(client.fd);
        }
        if (tslUdp >= 0)
        {
            ::close(tslUdp);
        }
        if (tslListen >= 0)
        {
            ::close(tslListen);
        }
    }
};

Engine::Engine(Config config, RuntimeModel& runtime, LayoutBookStore& layouts, Metrics& metrics)
    : impl_(new Impl(std::move(config), runtime, layouts, metrics))
{
}

Engine::~Engine()
{
    stop();
    delete impl_;
}

void Engine::start()
{
    impl_->ensureDomain();
    impl_->openTsl();
    impl_->loadRoutes();
    if (!impl_->config.backgroundFile.empty())
    {
        loadImageFile(impl_->config.backgroundFile, impl_->background);
    }
    if (impl_->config.backend != "cpu")
    {
        impl_->useCuda = cudaRuntimeAvailable();
    }
    if (impl_->config.backend == "cuda" && !impl_->useCuda)
    {
        throw std::runtime_error("MV_BACKEND=cuda but no CUDA device is visible");
    }
    impl_->runtime.setGpu(cudaSupportCompiled(), cudaDeviceCount());
    impl_->metrics.set("info", {{"version", MV_VERSION}, {"mxl_revision", MV_MXL_REVISION}, {"backend", impl_->useCuda ? "cuda" : "cpu"}}, 1);
    std::uint64_t freeBytes = 0;
    std::uint64_t totalBytes = 0;
    if (impl_->useCuda)
    {
        cudaDeviceMemory(&freeBytes, &totalBytes);
    }
    impl_->metrics.set("gpu_memory_bytes", {}, totalBytes >= freeBytes ? static_cast<double>(totalBytes - freeBytes) : 0);
    impl_->run.store(true);
    impl_->runtime.touch();
    for (int input = 1; input <= impl_->config.maxInputs; ++input)
    {
        impl_->threads.emplace_back([this, input] { impl_->readerMain(input); });
    }
    impl_->overlays.assign(static_cast<std::size_t>(impl_->config.outputs), nullptr);
    for (int head = 1; head <= impl_->config.outputs; ++head)
    {
        impl_->threads.emplace_back([this, head] { impl_->overlayMain(head); });
        impl_->threads.emplace_back([this, head] { impl_->headMain(head); });
    }
    if (impl_->config.tslEnable)
    {
        impl_->threads.emplace_back([this] { impl_->tslMain(); });
    }
}

void Engine::stop()
{
    if (impl_ == nullptr || !impl_->run.exchange(false))
    {
        if (impl_ != nullptr)
        {
            for (auto& thread : impl_->threads)
            {
                if (thread.joinable())
                {
                    thread.join();
                }
            }
            impl_->threads.clear();
        }
        return;
    }
    for (auto& thread : impl_->threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    impl_->threads.clear();
}

void Engine::setRoute(int input, bool video, bool enable, std::string domainId, std::string flowId, std::string senderId)
{
    if (input < 1 || input > impl_->config.maxInputs)
    {
        return;
    }
    std::lock_guard lock{impl_->routeMu};
    auto& route = video ? impl_->videoRoutes[static_cast<std::size_t>(input - 1)] : impl_->audioRoutes[static_cast<std::size_t>(input - 1)];
    route.enable = enable;
    route.domainId = std::move(domainId);
    route.flowId = std::move(flowId);
    route.senderId = std::move(senderId);
    impl_->metrics.inc("nmos_activations_total", {{"input", std::to_string(input)}, {"kind", video ? "video" : "audio"}});
    impl_->persistRoutes();
}

void Engine::removeOwnDomain()
{
    if (impl_ == nullptr || impl_->config.outputDomainDir.empty() || isMirrorDomain(impl_->config.outputDomainDir))
    {
        return;
    }
    std::error_code ec;
    auto const output = std::filesystem::weakly_canonical(impl_->config.outputDomainDir, ec);
    auto const scan = std::filesystem::weakly_canonical(impl_->config.scanPath, ec);
    if (!output.empty() && output == scan)
    {
        logError("domain_cleanup_refused", {{"path", impl_->config.outputDomainDir}});
        return;
    }
    std::filesystem::remove_all(impl_->config.outputDomainDir, ec);
    if (ec)
    {
        logError("domain_cleanup_failed", {{"path", impl_->config.outputDomainDir}, {"error", ec.message()}});
    }
}

std::string Engine::domainId() const
{
    return impl_->domainId;
}

void Engine::setFlowCallback(std::function<void(int, std::string const&, std::string const&, VideoFormat const&)> callback)
{
    impl_->onFlow = std::move(callback);
}
} // namespace mv
