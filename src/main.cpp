#include "app/runtime.hpp"
#include "config/store.hpp"
#include "layout/book.hpp"
#include "media/timebase.hpp"
#include "mxlio/engine.hpp"
#include "nmos/node.hpp"
#include "ops/api.hpp"
#include "ops/httpserver.hpp"
#include "media/cuda_compose.hpp"
#include "media/preview.hpp"
#include "media/publisher.hpp"
#include "ops/child.hpp"
#include "ops/mediamtx.hpp"
#include "ops/metrics.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#ifdef MV_HAS_UI
#include "ops/webui_generated.hpp"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace
{
std::atomic<bool> gStop{false};

void onSignal(int)
{
    gStop.store(true);
}

std::map<std::string, std::string> environmentMap()
{
    std::map<std::string, std::string> env;
    for (char** cursor = environ; cursor != nullptr && *cursor != nullptr; ++cursor)
    {
        std::string entry(*cursor);
        auto const eq = entry.find('=');
        if (eq != std::string::npos)
        {
            env.emplace(entry.substr(0, eq), entry.substr(eq + 1));
        }
    }
    return env;
}
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::cout << "mxl-multiviewer " << MV_VERSION << "\n"
                      << "MXL pin " << MV_MXL_REVISION << "\n"
                      << "Usage: mxl-multiviewer [--config FILE]\n"
                      << "Configuration: environment > MV_CONFIG_FILE > defaults. See SPECIFICATION.md.\n";
            return 0;
        }
    }
    auto env = environmentMap();
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc && env.count("MV_CONFIG_FILE") == 0)
        {
            env["MV_CONFIG_FILE"] = argv[++i];
        }
    }
    mv::setLogFormatJson(true);
    try
    {
        mv::ConfigStore store(env, std::nullopt);
        auto config = store.effectiveConfig();
        mv::setLogLevel(mv::parseLogLevel(config.logLevel));
        mv::setLogFormatJson(config.logFormat != "text");
        // Local time of the clock tiles (§6.2): MV_TIMEZONE beats TZ. Set before any thread starts.
        if (!config.timezone.empty())
        {
            setenv("TZ", config.timezone.c_str(), 1);
        }
        tzset();
        mv::logInfo("time_zone", {{"zone", mv::localZoneName()}, {"utc_offset_s", std::to_string(mv::utcOffsetSeconds())}});
#if !defined(MV_WITH_NMOS)
        if (config.nmosEnable)
        {
            mv::logError("config", {{"error", "NMOS_ENABLE=true but this binary was built without nmos-cpp"}});
            return 78;
        }
#endif
        if (config.backend == "cuda" && !mv::cudaSupportCompiled())
        {
            mv::logError("config", {{"error", "MV_BACKEND=cuda but this binary has no CUDA backend"}});
            return 78;
        }
        std::filesystem::create_directories(config.stateDir);
        store.ensureFile(config.stateDir + "/config.json");
        mv::RuntimeModel runtime(config);
        auto layoutsPath = config.layoutsFile.empty() ? config.stateDir + "/layouts.json" : config.layoutsFile;
        mv::LayoutBookStore layouts(config.maxInputs, config.activeLayout, layoutsPath);
        // A head starts on a layout set in the environment, else its start layout, else its
        // saved layout, else MV_OUT<h>_LAYOUT, else the book's active layout (SPECIFICATION.md §6.1).
        for (int head = 1; head <= config.outputs; ++head)
        {
            auto const& configured = config.heads[static_cast<std::size_t>(head - 1)];
            auto const start = layouts.startLayout(head, configured.layout, configured.layoutSet, store.pinnedLayout(head).value_or(""));
            runtime.setHeadLayout(head, start);
            mv::logInfo("head_layout", {{"head", std::to_string(head)}, {"layout", start}});
        }
        mv::Metrics metrics;
        // Pictures of image tiles: stored under the state folder, URLs fetched off the render path.
        mv::ImageStore images(config.stateDir + "/images");
        // Preview (§8.4): JPEG per head, or one WebRTC mosaic of the heads. Never both. The mosaic
        // outlives the engine, whose head threads draw into it.
        auto const preview = mv::previewPlan(config);
        auto const mosaic = preview.webrtc ? std::make_unique<mv::PreviewMosaic>(config.outputs) : nullptr;
        mv::Engine engine(config, runtime, layouts, metrics, images);
        mv::NmosNode node(config, [&](int input, bool video, bool enable, std::string domain, std::string flow, std::string sender) {
            engine.setRoute(input, video, enable, std::move(domain), std::move(flow), std::move(sender));
        });
        engine.setFlowCallback([&](int head, std::string const& videoFlow, std::string const& audioFlow, mv::VideoFormat const& format) {
            node.updateOutputFlow(head, videoFlow, audioFlow, format);
        });
        mv::Api api(config, store, layouts, runtime, metrics);
        api.setImages(images);
#ifdef MV_HAS_UI
        api.setIndexPage(std::string(mv::webui::indexHtml()));
#endif
        metrics.set("preview_mode", {{"mode", "jpeg"}}, preview.jpeg ? 1 : 0);
        metrics.set("preview_mode", {{"mode", "webrtc"}}, preview.webrtc ? 1 : 0);
        std::unique_ptr<mv::PreviewPublisher> publisher;
        mv::ChildProcess mediamtx("mediamtx");
        if (preview.webrtc)
        {
            engine.setPreviewMosaic(mosaic.get());
            metrics.set("preview_publish_mode", {{"mode", "own"}}, preview.ownMediamtx ? 1 : 0);
            metrics.set("preview_publish_mode", {{"mode", "shared"}}, preview.ownMediamtx ? 0 : 1);
            mv::logInfo("preview", {{"mode", "webrtc"}, {"publish", preview.ownMediamtx ? "own" : "shared"}, {"url", preview.publishUrl}});
        }
        if (preview.ownMediamtx)
        {
            // Own mode: the image's MediaMTX runs as a supervised child with the generated config.
            auto const path = config.stateDir + "/mediamtx.yml";
            std::ofstream yml(path, std::ios::binary | std::ios::trunc);
            yml << mv::renderMediamtxConfig(config);
            if (!yml.flush())
            {
                mv::logError("mediamtx_config_failed", {{"path", path}});
                return 75;
            }
            mediamtx.start({"mediamtx", path});
        }
        api.setPreviewStatus([&] {
            auto status = publisher ? publisher->status() : mv::PreviewStatus{};
            status.mediamtxRunning = mediamtx.running();
            status.mediamtxRestarts = mediamtx.restarts();
            return status;
        });
        mv::HttpServer http;
        try
        {
            node.start();
            engine.start();
            // Routes restored from routes.json; outside the engine's route lock, because
            // an IS-05 activation takes the NMOS model lock first and the route lock second.
            for (auto const& route : engine.routes())
            {
                node.restoreRoute(route.input, route.video, route.enable, route.domainId, route.flowId, route.senderId);
            }
            http.start(config.webPort, [&](mv::HttpRequest const& request) {
                if (!config.webEnable && (request.path == "/" || request.path == "/index.html" || request.path == "/preview.jpg"))
                {
                    mv::HttpResponse hidden;
                    hidden.status = 404;
                    hidden.contentType = "text/plain";
                    hidden.body = "web ui disabled";
                    return hidden;
                }
                if (request.path == "/" || request.path == "/index.html")
                {
                    mv::HttpResponse page;
                    page.contentType = "text/html; charset=utf-8";
#ifdef MV_HAS_UI
                    page.body = std::string(mv::webui::indexHtml());
#else
                    page.body = "<!doctype html><title>mxl-multiviewer</title><p>UI was not embedded. API is at /api/v1/info.</p>";
#endif
                    return page;
                }
                return api.handle(request);
            });
            if (preview.webrtc)
            {
                // NVENC in the compositor's CUDA context on the CUDA backend.
                publisher = std::make_unique<mv::PreviewPublisher>(*mosaic, preview.publishUrl, config.previewFps, engine.usesCuda());
                publisher->start([&metrics](double seconds) { metrics.observe("preview_encode_seconds", {}, seconds); });
            }
        }
        catch (mv::ConfigError const& ex)
        {
            mv::logError("startup", {{"error", ex.what()}});
            return 78;
        }
        catch (std::exception const& ex)
        {
            mv::logError("startup", {{"error", ex.what()}});
            return 75;
        }
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        mv::logInfo("ready", {{"web", std::to_string(config.webPort)}, {"nmos", std::to_string(config.nmosPort)}});
        auto nextEvents = std::chrono::steady_clock::now();
        int const eventMs = std::max(20, 1000 / std::max(1, config.overlayHz));
        while (!gStop.load())
        {
            runtime.setNmosUp(node.registered());
            metrics.set("nmos_registry_up", {}, runtime.nmosUp() ? 1 : 0);
            if (publisher)
            {
                auto const status = publisher->status();
                for (char const* state : {"connecting", "publishing", "error"})
                {
                    metrics.set("preview_publish_state", {{"state", state}}, status.state == state ? 1 : 0);
                }
                for (char const* encoder : {"nvenc", "x264"})
                {
                    metrics.set("preview_encoder", {{"encoder", encoder}}, status.encoder == encoder ? 1 : 0);
                }
                metrics.set("preview_frames_total", {}, static_cast<double>(status.frames));
            }
            auto const now = std::chrono::steady_clock::now();
            if (now >= nextEvents)
            {
                http.broadcast(api.eventsJson());
                nextEvents = now + std::chrono::milliseconds(eventMs);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::atomic<bool> finished{false};
        std::thread watchdog([&] {
            for (int i = 0; i < config.shutdownTimeoutS * 10 && !finished.load(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!finished.load())
            {
                _exit(143);
            }
        });
        http.stop();
        if (publisher)
        {
            publisher->stop();
        }
        engine.stop();
        node.stop();
        mediamtx.stop();
        if (config.cleanupOnExit)
        {
            engine.removeOwnDomain();
        }
        finished.store(true);
        watchdog.join();
        return 143;
    }
    catch (mv::ConfigError const& ex)
    {
        mv::logError("config", {{"error", ex.what()}});
        return 78;
    }
    catch (std::exception const& ex)
    {
        mv::logError("fatal", {{"error", ex.what()}});
        return 75;
    }
}
