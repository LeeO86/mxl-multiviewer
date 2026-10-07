#include "mxlio/engine.hpp"

#include "control/tsl.hpp"
#include "media/alarm.hpp"
#include "media/audioring.hpp"
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
#include "util/taskpool.hpp"
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
    // upload failed. CPU backend: `v210`, a copy of the packed grain the compositor scales
    // from directly; `frame` for inputs with a key.
    std::shared_ptr<Frame422> frame;
    std::shared_ptr<CudaFrame const> gpu;
    std::shared_ptr<std::vector<std::uint8_t> const> v210;
    int rowBytes = 0;
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

// One input's video leg, published by its reader thread.
struct Snap
{
    std::vector<SavedGrain> grains;
    std::string videoState = "not_routed";
    std::string videoReason;
    std::string senderId;
    // The MXL flow label of the routed flow.
    std::string label;
    std::uint64_t grainsRead = 0;
    std::uint64_t resyncs = 0;
    bool alarmNoSignal = false;
    bool alarmBlack = false;
    bool alarmFreeze = false;
    bool alarmFormat = false;
    std::array<std::int64_t, kAlarmCount> alarmSince{};
};

// One input's audio leg, published by its audio thread.
struct AudioSnap
{
    std::string state = "not_routed";
    std::string reason;
    int channels = 0;
    std::array<double, 16> ppm = silentMeters();
    std::array<double, 16> hold = silentMeters();
    std::array<double, 16> rms = silentMeters();
    std::array<bool, 16> clip{};
    bool alarmSilence = false;
    bool alarmClip = false;
    std::array<std::int64_t, kAlarmCount> alarmSince{};
};

std::int64_t wallClockMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

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

// Limited-range black into a rectangle of the canvas: letterbox areas of fit tiles (§5.5).
void fillBlack(Frame422& canvas, PixelRect const& rect)
{
    int const cw = canvas.chromaWidth();
    int const x0 = std::clamp(rect.x, 0, canvas.width);
    int const x1 = std::clamp(rect.x + rect.w, 0, canvas.width);
    for (int y = std::max(0, rect.y); y < std::min(canvas.height, rect.y + rect.h); ++y)
    {
        std::fill(canvas.y.begin() + y * canvas.width + x0, canvas.y.begin() + y * canvas.width + x1, std::uint16_t{64});
        int const c0 = std::min(cw, (x0 + 1) / 2);
        int const c1 = std::min(cw, x1 / 2);
        if (c1 > c0)
        {
            std::fill(canvas.cb.begin() + y * cw + c0, canvas.cb.begin() + y * cw + c1, std::uint16_t{512});
            std::fill(canvas.cr.begin() + y * cw + c0, canvas.cr.begin() + y * cw + c1, std::uint16_t{512});
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
    std::vector<std::shared_ptr<AudioSnap>> audioSnaps;
    std::mutex snapMu;
    // Per input: recent audio samples for audio-follow (appended by audioMain, copied by headMain).
    std::vector<std::shared_ptr<AudioRing>> rings;
    struct SenderLabel
    {
        std::string key;
        std::string label;
    };
    // Per input: the registry label of the routed sender (labelMain).
    std::vector<SenderLabel> senderLabels;
    std::mutex labelMu;
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
        // CPU backend: the same overlay in YCbCr, converted by the overlay thread so the
        // compose thread only blends it.
        std::shared_ptr<PreparedOverlay const> prepared;
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
        }
        audioSnaps.resize(static_cast<std::size_t>(config.maxInputs));
        for (auto& sound : audioSnaps)
        {
            sound = std::make_shared<AudioSnap>();
        }
        rings.resize(static_cast<std::size_t>(config.maxInputs));
        for (auto& ring : rings)
        {
            ring = std::make_shared<AudioRing>();
        }
        senderLabels.resize(static_cast<std::size_t>(config.maxInputs));
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

    std::shared_ptr<AudioSnap> audioSnap(int input)
    {
        std::lock_guard lock{snapMu};
        return audioSnaps[static_cast<std::size_t>(input - 1)];
    }

    void publishAudio(int input, std::shared_ptr<AudioSnap> next)
    {
        std::lock_guard lock{snapMu};
        audioSnaps[static_cast<std::size_t>(input - 1)] = std::move(next);
    }

    std::string senderLabel(int input)
    {
        std::lock_guard lock{labelMu};
        return senderLabels[static_cast<std::size_t>(input - 1)].label;
    }

    // Debounces one alarm of an input (§6.3) and keeps the wall-clock time it became active.
    void bumpAlarm(int input, char const* name, Debounce& debounce, bool raw, bool& flag, std::int64_t& since)
    {
        auto const nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (debounce.update(raw, nowMs, config.alarmDebounceMs, config.alarmClearMs))
        {
            if (debounce.active)
            {
                metrics.inc("alarms_total", {{"input", std::to_string(input)}, {"name", name}});
                since = wallClockMs();
            }
            else
            {
                since = 0;
            }
        }
        flag = debounce.active;
        metrics.set("alarms", {{"input", std::to_string(input)}, {"name", name}}, flag ? 1 : 0);
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
            // BCP-007-03 requires id, label, description and tags.
            out << "{\"id\":\"" << domainId << "\",\"label\":\"mxl-multiviewer\",\"description\":\"Multiviewer output domain\",\"tags\":{}}\n";
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
        // CUDA locks the reader's grain memory; unlock it before the reader unmaps it.
        auto const releaseVideo = [&] {
            cudaReleaseHostMemory();
            mxlReleaseFlowReader(instance, video);
        };
        std::string openKey;
        // The route the grains in the snapshot came from: a re-route drops them at once.
        std::string grainsKey;
        FlowMeta meta;
        std::string metaKey;
        int backoff = 250;
        AlarmSet alarms;
        // CPU backend: buffers for copies of the packed grains (see SavedGrain::v210).
        std::vector<std::shared_ptr<std::vector<std::uint8_t>>> grainPool;
        std::string prevState;
        std::uint64_t const holdNs = static_cast<std::uint64_t>(config.holdMs) * 1000000ull;
        auto const bump = [&](std::shared_ptr<Snap> const& snap, char const* name, AlarmIndex index, Debounce& debounce, bool raw, bool& flag) {
            bumpAlarm(input, name, debounce, raw, flag, snap->alarmSince[index]);
        };
        // Without a new grain only the no-signal alarm can rise; the picture alarms clear.
        auto const idleAlarms = [&](std::shared_ptr<Snap> const& snap, bool noSignal) {
            bump(snap, AlarmNames::noSignal, kAlarmNoSignal, alarms.noSignal, noSignal, snap->alarmNoSignal);
            bump(snap, AlarmNames::black, kAlarmBlack, alarms.black, false, snap->alarmBlack);
            bump(snap, AlarmNames::freeze, kAlarmFreeze, alarms.freeze, false, snap->alarmFreeze);
            bump(snap, AlarmNames::formatMismatch, kAlarmFormat, alarms.formatMismatch, false, snap->alarmFormat);
        };
        // No new grain in this wait (§5.3): `holding` once the last grain is three of its
        // frames old, `no_signal` after MV_HOLD_MS. Until then black, freeze, and format keep
        // their state: a slow or jittery source (below 20 fps, a mirror, a loaded host) misses
        // the 50 ms wait between grains, and clearing them then kept those alarms from rising.
        auto const staleState = [&](std::shared_ptr<Snap> const& snap) {
            auto const now = mxlGetTime();
            if (snap->grains.empty() || now > snap->grains.back().origin + holdNs)
            {
                snap->videoState = "no_signal";
                idleAlarms(snap, true);
                return;
            }
            auto const& last = snap->grains.back();
            std::uint64_t const frameNs =
                1000000000ull * static_cast<std::uint64_t>(std::max(1, last.rateDen)) / static_cast<std::uint64_t>(std::max(1, last.rateNum));
            if (now > last.origin + 3 * frameNs)
            {
                snap->videoState = "holding";
            }
            bump(snap, AlarmNames::noSignal, kAlarmNoSignal, alarms.noSignal, false, snap->alarmNoSignal);
        };
        auto const waiting = [&](std::shared_ptr<Snap> const& snap, char const* reason) {
            snap->videoState = "waiting";
            snap->videoReason = reason;
            idleAlarms(snap, true);
        };
        auto const publishState = [&](std::shared_ptr<Snap> const& snap) {
            auto const route = videoRoute(input);
            InputView view;
            view.index = input;
            view.video.enable = route.enable;
            view.video.domainId = route.domainId;
            view.video.flowId = route.flowId;
            view.video.senderId = route.senderId;
            view.video.state = snap->videoState;
            view.video.reason = snap->videoReason;
            auto const registered = senderLabel(input);
            view.video.label = registered.empty() ? snap->label : registered;
            view.video.grains = snap->grainsRead;
            view.video.resyncs = snap->resyncs;
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
            view.alarmNoSignal = snap->alarmNoSignal;
            view.alarmBlack = snap->alarmBlack;
            view.alarmFreeze = snap->alarmFreeze;
            view.alarmFormat = snap->alarmFormat;
            view.alarmSinceMs = snap->alarmSince;
            runtime.setInputVideo(view);
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
            auto next = std::make_shared<Snap>(*snap(input));
            if (!route.enable || route.flowId.empty() || route.domainId.empty())
            {
                next->videoState = "not_routed";
                next->videoReason.clear();
                next->grains.clear();
                next->label.clear();
                grainsKey.clear();
                alarms.haveHash = false;
                if (video != nullptr)
                {
                    releaseVideo();
                    video = nullptr;
                }
                if (instance != nullptr)
                {
                    mxlDestroyInstance(instance);
                    instance = nullptr;
                }
                openKey.clear();
                idleAlarms(next, false);
                publishState(next);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            auto const domain = resolveDomain(config.scanPath, route.domainId);
            if (!domain)
            {
                waiting(next, "domain_not_found");
                publishState(next);
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                backoff = std::min(5000, backoff * 2);
                continue;
            }
            auto const key = domain->path + "|" + route.flowId;
            if (!next->grains.empty() && grainsKey != key)
            {
                // Another flow is routed now: never show the previous source under it.
                next->grains.clear();
                next->label.clear();
                alarms.haveHash = false;
            }
            if (instance == nullptr || openKey.substr(0, domain->path.size()) != domain->path)
            {
                if (video != nullptr)
                {
                    releaseVideo();
                    video = nullptr;
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
                waiting(next, "domain_not_found");
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
                    video = nullptr;
                    waiting(next, "flow_not_found");
                    publishState(next);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                    backoff = std::min(5000, backoff * 2);
                    continue;
                }
                openKey = key;
                backoff = 250;
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
                waiting(next, "flow_not_found");
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
                staleState(next);
                publishState(next);
                continue;
            }
            if (status == MXL_ERR_FLOW_INVALID)
            {
                // The writer re-created the flow (a restart deletes and re-creates it): open the
                // new one. The last frame holds meanwhile.
                releaseVideo();
                video = nullptr;
                openKey.clear();
                metaKey.clear();
                staleState(next);
                publishState(next);
                continue;
            }
            if (status != MXL_STATUS_OK || payload == nullptr || (grain.flags & MXL_GRAIN_FLAG_INVALID) != 0)
            {
                staleState(next);
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
            V210Scan scan;
            bool scanned = false;
            if (useCuda.load())
            {
                // Straight from the MXL grain to the GPU; the CPU neither copies nor unpacks it.
                saved.gpu = cudaUploadFrame(payload, rowBytes, alphaKey, static_cast<int>(alpha10RowBytes(width)), width, height);
            }
            if (saved.gpu == nullptr && !useCuda.load() && alphaKey == nullptr)
            {
                // A copy of the packed grain from a small pool (a buffer is free again once
                // only the pool holds it): unpacking every input completely each frame was
                // the largest cost of the CPU backend, and the compositor needs only the
                // source lines a tile touches.
                std::shared_ptr<std::vector<std::uint8_t>> buffer;
                for (auto const& candidate : grainPool)
                {
                    if (candidate.use_count() == 1)
                    {
                        buffer = candidate;
                        break;
                    }
                }
                if (buffer == nullptr)
                {
                    buffer = std::make_shared<std::vector<std::uint8_t>>();
                    if (grainPool.size() < 12)
                    {
                        grainPool.push_back(buffer);
                    }
                }
                buffer->resize(fillBytes);
                // The alarms read their samples from each line while it is in cache: as a
                // separate pass they read most of the frame from memory again.
                copyV210Scan(payload, buffer->data(), rowBytes, width, height, 32, scan);
                scanned = true;
                saved.v210 = std::move(buffer);
                saved.rowBytes = rowBytes;
            }
            else if (saved.gpu == nullptr)
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
            grainsKey = key;
            ++next->grainsRead;
            metrics.inc("grains_read_total", {{"input", std::to_string(input)}});
            next->videoState = "running";
            next->videoReason.clear();
            next->label = meta.label.empty() ? next->label : meta.label;
            next->senderId = route.senderId;
            double sum = 0;
            int samples = 0;
            // Without an unpacked frame (CUDA, and the CPU backend's packed copy) the same luma
            // samples are read from the packed grain in one pass.
            if (scanned)
            {
                sum = static_cast<double>(scan.sum);
                samples = scan.count;
            }
            else if (saved.frame)
            {
                for (int i = 0; i < width * height; i += 32)
                {
                    sum += saved.frame->y[static_cast<std::size_t>(i)];
                    ++samples;
                }
            }
            else
            {
                sum = static_cast<double>(v210LumaSum(payload, rowBytes, width, height, 32, &samples));
            }
            bool const black = samples > 0 && (sum / samples) <= config.blackY;
            auto const hash = scanned ? scan.hash : saved.frame ? lumaHash(*saved.frame) : lumaHash(payload, rowBytes, width, height);
            bool const freeze = alarms.haveHash && hash == alarms.lastHash;
            alarms.lastHash = hash;
            alarms.haveHash = true;
            bool const formatBad = width > 3840 || height > 2160 || !allowedRate(meta.rateNum, meta.rateDen) ||
                                   (meta.mediaType != "video/v210" && meta.mediaType != "video/v210a" && !meta.mediaType.empty());
            bump(next, AlarmNames::noSignal, kAlarmNoSignal, alarms.noSignal, false, next->alarmNoSignal);
            bump(next, AlarmNames::black, kAlarmBlack, alarms.black, black, next->alarmBlack);
            bump(next, AlarmNames::freeze, kAlarmFreeze, alarms.freeze, freeze, next->alarmFreeze);
            bump(next, AlarmNames::formatMismatch, kAlarmFormat, alarms.formatMismatch, formatBad, next->alarmFormat);
            publishState(next);
        }
        if (video != nullptr && instance != nullptr)
        {
            releaseVideo();
        }
        if (instance != nullptr)
        {
            mxlDestroyInstance(instance);
        }
    }

    // True while a head copies this input's audio (audio-follow).
    bool followed(int input) const
    {
        for (int head = 1; head <= config.outputs; ++head)
        {
            if (runtime.headAudioFollow(head) == input && runtime.headAudioChannels(head) > 0)
            {
                return true;
            }
        }
        return false;
    }

    // The audio leg of one input, independent of its video leg (§4.2: they may come from
    // different senders and domains). Meters every new sample from the flow's own head.
    void audioMain(int input)
    {
        mxlInstance instance = nullptr;
        mxlFlowReader reader = nullptr;
        std::string openDomain;
        std::string openFlow;
        PpmMeter meters[16];
        PpmConfig ballistics;
        ballistics.clipLinear = config.clipLinear;
        std::uint64_t lastEnd = 0;
        std::uint64_t lastHead = 0;
        auto lastMove = std::chrono::steady_clock::now();
        Debounce silenceAlarm;
        Debounce clipAlarm;
        int backoff = 250;
        std::string prevState;
        auto& ring = *rings[static_cast<std::size_t>(input - 1)];
        auto lastProbe = std::chrono::steady_clock::now();
        auto const release = [&] {
            if (reader != nullptr)
            {
                mxlReleaseFlowReader(instance, reader);
                reader = nullptr;
            }
            openFlow.clear();
        };
        // The writer re-created the flow: drop the reader and its instance, open again.
        auto const reopen = [&] {
            release();
            if (instance != nullptr)
            {
                mxlDestroyInstance(instance);
                instance = nullptr;
            }
            openDomain.clear();
        };
        auto const resetMeters = [&](AudioSnap& sound) {
            for (auto& meter : meters)
            {
                meter = PpmMeter{};
            }
            sound.ppm = silentMeters();
            sound.hold = silentMeters();
            sound.rms = silentMeters();
            sound.clip = {};
            ring.clear();
            lastEnd = 0;
        };
        auto const publishState = [&](std::shared_ptr<AudioSnap> const& sound, bool silence, bool clip) {
            bumpAlarm(input, AlarmNames::silence, silenceAlarm, silence, sound->alarmSilence, sound->alarmSince[kAlarmSilence]);
            bumpAlarm(input, AlarmNames::clip, clipAlarm, clip, sound->alarmClip, sound->alarmSince[kAlarmClip]);
            auto const route = audioRoute(input);
            InputView view;
            view.index = input;
            view.audio.enable = route.enable;
            view.audio.domainId = route.domainId;
            view.audio.flowId = route.flowId;
            view.audio.senderId = route.senderId;
            view.audio.state = sound->state;
            view.audio.reason = sound->reason;
            view.audio.channels = sound->channels;
            view.ppmDbfs = sound->ppm;
            view.holdDbfs = sound->hold;
            view.rmsDbfs = sound->rms;
            view.clip = sound->clip;
            view.alarmSilence = sound->alarmSilence;
            view.alarmClip = sound->alarmClip;
            view.alarmSinceMs = sound->alarmSince;
            runtime.setInputAudio(view);
            if (prevState != sound->state)
            {
                if (!prevState.empty())
                {
                    metrics.set("input_state", {{"input", std::to_string(input)}, {"kind", "audio"}, {"state", prevState}}, 0);
                }
                metrics.set("input_state", {{"input", std::to_string(input)}, {"kind", "audio"}, {"state", sound->state}}, 1);
                prevState = sound->state;
            }
            publishAudio(input, sound);
        };
        auto const waitingFor = [&](std::shared_ptr<AudioSnap> const& sound, char const* reason) {
            sound->state = "waiting";
            sound->reason = reason;
            resetMeters(*sound);
            // Routed audio that does not arrive is silence on the wall.
            publishState(sound, true, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
            backoff = std::min(5000, backoff * 2);
        };

        while (run.load())
        {
            auto const route = audioRoute(input);
            auto next = std::make_shared<AudioSnap>(*audioSnap(input));
            if (!route.enable || route.flowId.empty())
            {
                release();
                if (instance != nullptr)
                {
                    mxlDestroyInstance(instance);
                    instance = nullptr;
                    openDomain.clear();
                }
                next->state = "not_routed";
                next->reason.clear();
                next->channels = 0;
                resetMeters(*next);
                publishState(next, false, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            auto const domain = resolveDomain(config.scanPath, route.domainId.empty() ? videoRoute(input).domainId : route.domainId);
            if (!domain)
            {
                waitingFor(next, "domain_not_found");
                continue;
            }
            if (instance == nullptr || openDomain != domain->path)
            {
                release();
                if (instance != nullptr)
                {
                    mxlDestroyInstance(instance);
                }
                instance = mxlCreateInstance(domain->path.c_str(), nullptr);
                openDomain = instance != nullptr ? domain->path : std::string{};
            }
            if (instance == nullptr)
            {
                waitingFor(next, "domain_not_found");
                continue;
            }
            if (openFlow != route.flowId)
            {
                // A re-route to another audio flow opens that flow (§4.2).
                release();
                resetMeters(*next);
                if (mxlCreateFlowReader(instance, route.flowId.c_str(), nullptr, &reader) != MXL_STATUS_OK)
                {
                    reader = nullptr;
                    waitingFor(next, "flow_not_found");
                    continue;
                }
                openFlow = route.flowId;
                backoff = 250;
                lastHead = 0;
            }
            mxlFlowInfo info{};
            if (mxlFlowReaderGetInfo(reader, &info) != MXL_STATUS_OK)
            {
                release();
                waitingFor(next, "flow_not_found");
                continue;
            }
            // Continuous flows only move runtime.headIndex; a head that stops for
            // MV_HOLD_MS is no signal.
            auto const head = info.runtime.headIndex;
            auto const now = std::chrono::steady_clock::now();
            if (head != lastHead)
            {
                lastHead = head;
                lastMove = now;
            }
            bool const moving = head != 0 && head != MXL_UNDEFINED_INDEX && now - lastMove <= std::chrono::milliseconds(config.holdMs);
            if (moving && head > lastEnd)
            {
                std::size_t maxRead = 0;
                mxlFlowReaderGetMaxReadLengthSamples(reader, &maxRead);
                // Every sample since the last read, at most 100 ms (a first read, or after a stall).
                std::uint64_t const fresh = lastEnd == 0 ? 4800 : head - lastEnd;
                auto const count = static_cast<std::size_t>(std::min<std::uint64_t>({fresh, maxRead == 0 ? 960 : maxRead, 4800}));
                mxlWrappedMultiBufferSlice slice{};
                auto const read = count > 0 ? mxlFlowReaderGetSamplesNonBlocking(reader, head, count, &slice) : MXL_ERR_UNKNOWN;
                if (read == MXL_ERR_FLOW_INVALID)
                {
                    reopen();
                    continue;
                }
                if (read == MXL_STATUS_OK)
                {
                    int const channels = static_cast<int>(std::min<std::size_t>(slice.count, 64));
                    next->channels = channels;
                    for (int ch = 0; ch < std::min(channels, 16); ++ch)
                    {
                        auto const index = static_cast<std::size_t>(ch);
                        for (int frag = 0; frag < 2; ++frag)
                        {
                            auto const* pointer = static_cast<char const*>(slice.base.fragments[frag].pointer);
                            auto const samples = slice.base.fragments[frag].size / sizeof(float);
                            if (pointer != nullptr && samples > 0)
                            {
                                meters[ch].process(reinterpret_cast<float const*>(pointer + static_cast<std::size_t>(ch) * slice.stride), static_cast<int>(samples),
                                    48000.0, ballistics);
                            }
                        }
                        next->ppm[index] = meters[ch].levelDbfs();
                        next->hold[index] = meters[ch].holdDbfs();
                        next->rms[index] = meters[ch].rmsDbfs();
                        next->clip[index] = meters[ch].clip;
                    }
                    if (followed(input))
                    {
                        // Only the new samples go into the ring (an output carries at most 16 channels).
                        std::uint64_t first = head - count;
                        for (int frag = 0; frag < 2; ++frag)
                        {
                            auto const* pointer = static_cast<char const*>(slice.base.fragments[frag].pointer);
                            auto const samples = slice.base.fragments[frag].size / sizeof(float);
                            if (pointer == nullptr || samples == 0)
                            {
                                continue;
                            }
                            std::vector<float const*> planes;
                            for (int ch = 0; ch < std::min(channels, 16); ++ch)
                            {
                                planes.push_back(reinterpret_cast<float const*>(pointer + static_cast<std::size_t>(ch) * slice.stride));
                            }
                            ring.push(first, planes, samples);
                            first += samples;
                        }
                    }
                    lastEnd = head;
                }
            }
            next->reason.clear();
            bool silence = true;
            bool clip = false;
            if (moving)
            {
                next->state = "running";
                for (int ch = 0; ch < std::min(next->channels, 16); ++ch)
                {
                    silence = silence && next->ppm[static_cast<std::size_t>(ch)] < config.silenceDbfs;
                    clip = clip || next->clip[static_cast<std::size_t>(ch)];
                }
            }
            else
            {
                next->state = "no_signal";
                resetMeters(*next);
                // A stopped head never reads, and MXL reports a re-created flow only on a
                // read past the head: probe once a second.
                if (now - lastProbe >= std::chrono::seconds(1))
                {
                    lastProbe = now;
                    mxlWrappedMultiBufferSlice probe{};
                    if (mxlFlowReaderGetSamplesNonBlocking(reader, head + 1, 1, &probe) == MXL_ERR_FLOW_INVALID)
                    {
                        reopen();
                        publishState(next, true, false);
                        continue;
                    }
                }
            }
            publishState(next, silence, clip);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        release();
        if (instance != nullptr)
        {
            mxlDestroyInstance(instance);
        }
    }

    // Looks up the routed senders' labels in the registry Query API for UMD source is04
    // (§4.3). A route without a sender id is looked up by its flow id. A failed lookup
    // keeps the previous label; a re-route clears it.
    void labelMain()
    {
        std::string const base = "http://" + config.nmosQueryAddress + ":" + std::to_string(config.nmosQueryPort) + "/x-nmos/query/v1.3/senders";
        std::vector<std::chrono::steady_clock::time_point> checked(static_cast<std::size_t>(config.maxInputs));
        while (run.load())
        {
            for (int input = 1; input <= config.maxInputs && run.load(); ++input)
            {
                auto const index = static_cast<std::size_t>(input - 1);
                auto const route = videoRoute(input);
                std::string const key = !route.enable || route.flowId.empty() ? std::string{} : route.senderId.empty() ? "flow:" + route.flowId : route.senderId;
                {
                    std::lock_guard lock{labelMu};
                    if (senderLabels[index].key != key)
                    {
                        senderLabels[index] = {key, {}};
                        checked[index] = {};
                    }
                }
                auto const now = std::chrono::steady_clock::now();
                if (key.empty() || (checked[index] != std::chrono::steady_clock::time_point{} && now - checked[index] < std::chrono::seconds(10)))
                {
                    continue;
                }
                checked[index] = now;
                auto const result = httpGet(route.senderId.empty() ? base + "?flow_id=" + route.flowId : base + "/" + route.senderId, 500);
                if (result.status != 200)
                {
                    continue;
                }
                std::string error;
                auto root = json::parse(result.body, error);
                if (!error.empty())
                {
                    continue;
                }
                if (root.is<picojson::array>())
                {
                    auto const& items = root.get<picojson::array>();
                    if (items.empty())
                    {
                        continue;
                    }
                    root = picojson::value(items.front());
                }
                if (auto const label = json::fieldString(root, "label"))
                {
                    std::lock_guard lock{labelMu};
                    if (senderLabels[index].key == key)
                    {
                        senderLabels[index].label = *label;
                    }
                }
            }
            for (int i = 0; i < 10 && run.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    // The overlay items of one head (UMD, tally, meters, alarms, slates, clocks), from
    // input snapshots and runtime state only, so the overlay thread can build them.
    std::vector<OverlayTile> overlayTiles(std::vector<Tile> const& tiles, VideoFormat const& format)
    {
        std::vector<OverlayTile> drawn;
        auto const inputs = runtime.inputs();
        auto const now = mxlGetTime();
        // The composer shows a grain until it is older than MV_HOLD_MS at its read point.
        std::uint64_t const frameNs = 1000000000ull * static_cast<std::uint64_t>(std::max(1, format.rateDen)) / static_cast<std::uint64_t>(std::max(1, format.rateNum));
        std::uint64_t const staleNs = static_cast<std::uint64_t>(config.holdMs) * 1000000ull + frameNs * static_cast<std::uint64_t>(config.inputOffsetGrains);
        for (auto const& tile : tiles)
        {
            OverlayTile item;
            item.rect = rectToPixels(tile.rect, format.width, format.height);
            bool const isInput = tile.content == TileContent::Input && tile.input >= 1 && tile.input <= config.maxInputs;
            item.umd = tile.umd && isInput;
            auto const current = isInput ? snap(tile.input) : nullptr;
            auto const sound = isInput ? audioSnap(tile.input) : nullptr;
            auto const viewIn = isInput ? inputs[static_cast<std::size_t>(tile.input - 1)] : InputView{};
            std::string const inputName = "MV In " + std::to_string(tile.input);
            if (tile.umdSource == UmdSource::Manual)
            {
                item.umdText = tile.umdText;
            }
            else if (tile.umdSource == UmdSource::Tsl)
            {
                item.umdText = !viewIn.tslText.empty() ? viewIn.tslText : !tile.umdText.empty() ? tile.umdText : inputName;
            }
            else if (current != nullptr)
            {
                // §4.3: the routed sender's registry label, then the tile's text, then the
                // MXL flow label (a lab without a registry), then MV In <n>.
                auto const registered = senderLabel(tile.input);
                item.umdText = !registered.empty() ? registered : !tile.umdText.empty() ? tile.umdText : !current->label.empty() ? current->label : inputName;
            }
            item.umdPosition = tile.umdPosition;
            item.umdFont = std::max(8, tile.umdFont * format.height / 1080);
            item.umdBg = parseHexColor(tile.umdBg, {0, 0, 0, 192});
            item.tally = viewIn.tally;
            item.tallyBorder = tile.tallyBorder;
            item.tallyLamp = tile.tallyLamp;
            item.umdFg = tallyRgba(item.tally);
            // Bars only on input tiles; the flag is ignored on clock, label, and empty tiles.
            item.bars = tile.audioBars && isInput;
            item.showRms = tile.audioBarRms;
            item.barChannels = tile.audioBarChannels;
            item.barsPosition = tile.audioBarPosition;
            item.zoneGreen = tile.zoneGreen;
            item.zoneAmber = tile.zoneAmber;
            if (sound != nullptr)
            {
                item.audioRouted = sound->state != "not_routed";
                for (int c = 0; c < 16; ++c)
                {
                    int const src = tile.audioBarFirst + c;
                    if (src < 16)
                    {
                        auto const from = static_cast<std::size_t>(src);
                        auto const to = static_cast<std::size_t>(c);
                        item.ppmDbfs[to] = sound->ppm[from];
                        item.holdDbfs[to] = sound->hold[from];
                        item.rmsDbfs[to] = sound->rms[from];
                        item.clip[to] = sound->clip[from];
                    }
                }
            }
            if (current != nullptr)
            {
                if (tile.formatLabel && !current->grains.empty())
                {
                    auto const& grain = current->grains.back();
                    item.formatText = formatLabel(grain.width, grain.height, grain.rateNum, grain.rateDen, grain.interlaced);
                }
                if (tile.latency && !current->grains.empty())
                {
                    auto const origin = current->grains.back().origin;
                    item.latencyText = std::to_string(static_cast<int>((now > origin ? now - origin : 0) / 1000000ull)) + " ms";
                }
                // §6.4: after the hold time the tile is black with a slate and the input name.
                auto const route = videoRoute(tile.input);
                bool const routed = route.enable && !route.flowId.empty() && !route.domainId.empty();
                bool const picture = !current->grains.empty() && now <= current->grains.back().origin + staleNs;
                if (!routed)
                {
                    item.slate = "NOT ROUTED";
                }
                else if (!picture)
                {
                    item.slate = current->videoState == "waiting" ? "WAITING" : "NO SIGNAL";
                }
                if (!item.slate.empty())
                {
                    item.slateLabel = inputName;
                    item.formatText.clear();
                    item.latencyText.clear();
                }
                // §6.3: red for no signal, black, freeze, and clip; amber for silence and format.
                bool const clip = sound != nullptr && sound->alarmClip;
                bool const silence = sound != nullptr && sound->alarmSilence;
                bool const red = current->alarmNoSignal || current->alarmBlack || current->alarmFreeze || clip;
                bool const amber = silence || current->alarmFormat;
                item.alarm = red ? AlarmLevel::Red : amber ? AlarmLevel::Amber : AlarmLevel::None;
                // A slate already says what is wrong; the badge names the first other alarm.
                if (item.slate.empty())
                {
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
                    else if (clip)
                    {
                        item.badge = "CLIP";
                    }
                    else if (silence)
                    {
                        item.badge = "SILENCE";
                    }
                    else if (current->alarmFormat)
                    {
                        item.badge = "FORMAT";
                    }
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
            else
            {
                frame->prepared = std::make_shared<PreparedOverlay const>(prepareOverlay(frame->rgba.data(), frame->width, frame->height, frame->width * 4));
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
        // CPU backend: tile workers, their images (reused), and the overlay in YCbCr.
        TaskPool tilePool(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 16u));
        std::vector<Frame422> tileImages;
        PreparedOverlay prepared;
        std::uint64_t preparedVersion = 0;
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
                // The whole tile; place.dst is the picture inside it.
                PixelRect rect;
                Placement place;
                std::shared_ptr<Frame422> frame;
                // Held until compose returns, so the device frame is not recycled under it.
                std::shared_ptr<CudaFrame const> gpu;
                std::shared_ptr<std::vector<std::uint8_t> const> v210;
                int rowBytes = 0;
                int width = 0;
                int height = 0;
                bool bob = false;
            };
            std::vector<Source> sources;
            std::uint64_t const frameDur = mxlIndexToTimestamp(&rate, 1);
            std::uint64_t const offset = frameDur * static_cast<std::uint64_t>(config.inputOffsetGrains);
            std::uint64_t const outputTime = when;
            std::uint64_t const target = outputTime > offset ? outputTime - offset : 0;
            std::uint64_t const holdNs = static_cast<std::uint64_t>(config.holdMs) * 1000000ull;
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
                // §5.3/§6.4: the last frame holds for MV_HOLD_MS, then the tile is black
                // and the overlay draws the slate.
                if (best != nullptr && target > best->origin + holdNs)
                {
                    best = nullptr;
                }
                auto const px = rectToPixels(tile.rect, format.width, format.height);
                Source source;
                source.rect = px;
                source.place = best != nullptr ? placeTile(px, best->width, best->height, tile.scale) : Placement{};
                if (best == nullptr)
                {
                    source.place.dst = px;
                }
                if (best != nullptr)
                {
                    source.frame = best->frame;
                    source.gpu = best->gpu;
                    source.v210 = best->v210;
                    source.rowBytes = best->rowBytes;
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
            // The layout's background colour where no tile is (§6.2); MV_BACKGROUND_FILE covers it.
            Ycbcr10 const bg = toYcbcr10(parseHexColor(layout ? layout->background : std::string("#101010"), {16, 16, 16, 255}));
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
                    bool const letterbox = (source.frame != nullptr || source.gpu != nullptr) &&
                                           (source.place.dst.x != source.rect.x || source.place.dst.y != source.rect.y || source.place.dst.w != source.rect.w ||
                                               source.place.dst.h != source.rect.h);
                    if (letterbox)
                    {
                        CudaTileView bars;
                        bars.dstX = source.rect.x;
                        bars.dstY = source.rect.y;
                        bars.dstW = source.rect.w;
                        bars.dstH = source.rect.h;
                        bars.solid = true;
                        views.push_back(bars);
                    }
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
                desc.bgY = bg.y;
                desc.bgCb = bg.cb;
                desc.bgCr = bg.cr;
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
                // Each tile into its own reused image on the pool, then blitted in z order.
                if (tileImages.size() < sources.size())
                {
                    tileImages.resize(sources.size());
                }
                std::vector<std::function<void()>> jobs;
                jobs.reserve(sources.size());
                for (std::size_t i = 0; i < sources.size(); ++i)
                {
                    jobs.emplace_back([&source = sources[i], &image = tileImages[i]] {
                        int const w = std::max(2, source.place.dst.w);
                        int const h = std::max(1, source.place.dst.h);
                        if (image.width != w || image.height != h || image.hasAlpha)
                        {
                            image.allocate(w, h, false);
                        }
                        Placement local = source.place;
                        local.dst = {0, 0, w, h};
                        if (source.v210 != nullptr)
                        {
                            scaleV210Into(image, local, V210View{source.v210->data(), source.rowBytes, source.width, source.height}, source.bob);
                        }
                        else if (source.frame != nullptr)
                        {
                            scaleInto(image, local, *source.frame, source.bob);
                        }
                        else
                        {
                            image.fill(64, 512, 512);
                        }
                    });
                }
                tilePool.run(jobs);
                if (coveredBackground.width == canvas.width && coveredBackground.height == canvas.height)
                {
                    canvas.y = coveredBackground.y;
                    canvas.cb = coveredBackground.cb;
                    canvas.cr = coveredBackground.cr;
                }
                else
                {
                    canvas.fill(bg.y, bg.cb, bg.cr);
                }
                for (std::size_t i = 0; i < sources.size(); ++i)
                {
                    auto const& source = sources[i];
                    if (source.place.dst.x != source.rect.x || source.place.dst.y != source.rect.y || source.place.dst.w != source.rect.w ||
                        source.place.dst.h != source.rect.h)
                    {
                        fillBlack(canvas, source.rect);
                    }
                    blit(canvas, source.place.dst, tileImages[i]);
                }
                if (withOverlay)
                {
                    // Converted by the overlay thread (or here once per version when that thread
                    // ran for CUDA); the blend is integer and skips clear pixels.
                    if (overlayFrame->prepared != nullptr)
                    {
                        blendOverlay(canvas, *overlayFrame->prepared);
                    }
                    else
                    {
                        if (overlayFrame->version != preparedVersion)
                        {
                            prepared = prepareOverlay(overlayFrame->rgba.data(), overlayFrame->width, overlayFrame->height, overlayFrame->width * 4);
                            preparedVersion = overlayFrame->version;
                        }
                        blendOverlay(canvas, prepared);
                    }
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
            if (audioWriter != nullptr && follow >= 1 && follow <= config.maxInputs && (channels == 2 || channels == 16))
            {
                auto const& ring = *rings[static_cast<std::size_t>(follow - 1)];
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
                            ring.copy(ch, (end - count) + filled, take, dst);
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
            if (++previewDiv >= std::max(1, format.rateNum / std::max(1, format.rateDen) / std::max(1, config.previewFps)))
            {
                previewDiv = 0;
                // CUDA: sample the written grain at preview size; a full unpack of the
                // output on this thread made frames late.
                runtime.setPreview(head, cudaFrame ? encodePreviewJpeg(out, static_cast<int>(v210RowBytes(format.width)), format.width, format.height, config.previewWidth, 60)
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
                << ",\"domain_id\":\"" << jsonEscape(route.domainId) << "\",\"flow_id\":\"" << jsonEscape(route.flowId) << "\",\"sender_id\":\""
                << jsonEscape(route.senderId) << "\"}";
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
        impl_->threads.emplace_back([this, input] { impl_->audioMain(input); });
    }
    if (impl_->config.nmosEnable && !impl_->config.nmosQueryAddress.empty())
    {
        impl_->threads.emplace_back([this] { impl_->labelMain(); });
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

std::vector<RouteState> Engine::routes() const
{
    std::vector<RouteState> out;
    std::lock_guard lock{impl_->routeMu};
    for (int i = 1; i <= impl_->config.maxInputs; ++i)
    {
        for (bool video : {true, false})
        {
            auto const& route = video ? impl_->videoRoutes[static_cast<std::size_t>(i - 1)] : impl_->audioRoutes[static_cast<std::size_t>(i - 1)];
            if (route.enable || !route.flowId.empty())
            {
                out.push_back(RouteState{i, video, route.enable, route.domainId, route.flowId, route.senderId});
            }
        }
    }
    return out;
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
