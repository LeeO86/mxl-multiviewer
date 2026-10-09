#include "app/runtime.hpp"
#include "config/store.hpp"
#include "layout/book.hpp"
#include "media/timebase.hpp"
#include "mxlio/engine.hpp"
#include "nmos/node.hpp"
#include "ops/api.hpp"
#include "ops/httpserver.hpp"
#include "media/cuda_compose.hpp"
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
#include <iostream>
#include <map>
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
        // A head starts on its saved layout, else MV_OUT<h>_LAYOUT, else the book's active
        // layout (SPECIFICATION.md §6.1).
        for (int head = 1; head <= config.outputs; ++head)
        {
            auto const& configured = config.heads[static_cast<std::size_t>(head - 1)];
            auto const start = layouts.startLayout(head, configured.layout, configured.layoutSet);
            runtime.setHeadLayout(head, start);
            mv::logInfo("head_layout", {{"head", std::to_string(head)}, {"layout", start}});
        }
        mv::Metrics metrics;
        mv::Engine engine(config, runtime, layouts, metrics);
        mv::NmosNode node(config, [&](int input, bool video, bool enable, std::string domain, std::string flow, std::string sender) {
            engine.setRoute(input, video, enable, std::move(domain), std::move(flow), std::move(sender));
        });
        engine.setFlowCallback([&](int head, std::string const& videoFlow, std::string const& audioFlow, mv::VideoFormat const& format) {
            node.updateOutputFlow(head, videoFlow, audioFlow, format);
        });
        mv::Api api(config, store, layouts, runtime, metrics);
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
        engine.stop();
        node.stop();
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
