// WebRTC preview (MV_PREVIEW_MODE, the mosaic and its map), the preview contract (PREVIEW_*, own or
// shared MediaMTX), the widgets (/widgets, /widget/<id>, CSP and CORS) and the MediaMTX supervisor.
#include <doctest/doctest.h>

#include "app/runtime.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "layout/book.hpp"
#include "media/preview.hpp"
#include "ops/api.hpp"
#include "ops/child.hpp"
#include "ops/mediamtx.hpp"
#include "ops/metrics.hpp"
#include "util/jsonutil.hpp"
#include "version.hpp"

#include <chrono>
#include <thread>

using namespace mv;

namespace
{
std::map<std::string, std::string> base(std::map<std::string, std::string> values)
{
    values.emplace("NMOS_ENABLE", "false");
    values.emplace("MV_BACKEND", "cpu");
    return values;
}

struct Fixture
{
    explicit Fixture(std::map<std::string, std::string> const& env)
        : store(base(env), std::nullopt)
        , cfg(store.effectiveConfig())
        , layouts(cfg.maxInputs, cfg.activeLayout, "")
        , runtime(cfg)
        , api(cfg, store, layouts, runtime, metrics)
    {
    }

    HttpResponse get(std::string const& path, std::string const& query = {}, std::map<std::string, std::string> headers = {})
    {
        return api.handle(HttpRequest{"GET", path, query, {}, std::move(headers)});
    }

    picojson::value json(std::string const& path)
    {
        auto const response = get(path);
        REQUIRE(response.status == 200);
        std::string error;
        auto value = json::parse(response.body, error);
        REQUIRE(error.empty());
        return value;
    }

    ConfigStore store;
    Config cfg;
    LayoutBookStore layouts;
    RuntimeModel runtime;
    Metrics metrics;
    Api api;
};

std::string header(HttpResponse const& response, std::string const& name)
{
    for (auto const& [key, value] : response.headers)
    {
        if (key == name)
        {
            return value;
        }
    }
    return {};
}

template <typename Pred>
bool waitFor(Pred pred, int ms)
{
    auto const end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
} // namespace

TEST_CASE("preview settings: defaults, precedence and validation")
{
    auto const defaults = loadConfig(base({}), {});
    CHECK(defaults.previewMode == "jpeg");
    CHECK(defaults.previewPublishUrl.empty());
    CHECK(defaults.previewPathPrefix == "mxl-multiviewer");
    CHECK(defaults.widgetFrameAncestors == "'self'");
    CHECK(defaults.mediamtxRtspPort == 8754);
    CHECK(defaults.mediamtxWhepPort == 8789);
    CHECK(defaults.mediamtxHlsPort == 8788);
    CHECK(defaults.mediamtxIcePort == 8389);
    // Environment beats the file.
    auto const layered = loadConfig(base({{"MV_PREVIEW_MODE", "WebRTC"}}), {{"MV_PREVIEW_MODE", "jpeg"}, {"PREVIEW_PATH_PREFIX", "/test-all/mv1/"}});
    CHECK(layered.previewMode == "webrtc");
    CHECK(layered.previewPathPrefix == "test-all/mv1");
    CHECK_THROWS_AS(loadConfig(base({{"MV_PREVIEW_MODE", "both"}}), {}), ConfigError);
    CHECK(loadConfig(base({{"PREVIEW_PUBLISH_URL", "RTSP://mediamtx.mxl-platform.svc:8554/"}}), {}).previewPublishUrl == "rtsp://mediamtx.mxl-platform.svc:8554");
    CHECK_THROWS_AS(loadConfig(base({{"PREVIEW_PUBLISH_URL", "http://host:8554"}}), {}), ConfigError);
    CHECK_THROWS_AS(loadConfig(base({{"PREVIEW_PUBLISH_URL", "rtsp://user:pw@host:8554"}}), {}), ConfigError);
    CHECK_THROWS_AS(loadConfig(base({{"PREVIEW_PUBLISH_URL", "rtsp://host:8554/path"}}), {}), ConfigError);
    CHECK(loadConfig(base({{"PREVIEW_WHEP_URL", "https://mv1-whep.example/"}}), {}).previewWhepUrl == "https://mv1-whep.example");
    CHECK_THROWS_AS(loadConfig(base({{"PREVIEW_HLS_URL", "rtsp://host"}}), {}), ConfigError);
    CHECK_THROWS_AS(loadConfig(base({{"PREVIEW_PATH_PREFIX", "a/../b"}}), {}), ConfigError);
    CHECK_THROWS_AS(loadConfig(base({{"PREVIEW_PATH_PREFIX", "a b"}}), {}), ConfigError);
    CHECK(loadConfig(base({{"PREVIEW_PATH_PREFIX", "/"}}), {}).previewPathPrefix == "mxl-multiviewer");
    CHECK_THROWS_AS(loadConfig(base({{"WIDGET_FRAME_ANCESTORS", "'self'; script-src *"}}), {}), ConfigError);
    CHECK_THROWS_AS(loadConfig(base({{"WIDGET_FRAME_ANCESTORS", "a\r\nX-Injected: 1"}}), {}), ConfigError);
    // Four heads fill the 2×2 mosaic.
    CHECK(loadConfig(base({{"MV_OUTPUTS", "4"}, {"MV_OUT4_FORMAT", "1280x720p50"}}), {}).heads[3].format.width == 1280);
    CHECK_THROWS_AS(loadConfig(base({{"MV_OUTPUTS", "5"}}), {}), ConfigError);
    // The built-in MediaMTX's ports are checked only when it runs.
    CHECK_NOTHROW(loadConfig(base({{"MEDIAMTX_WHEP_PORT", "8110"}}), {}));
    CHECK_THROWS_AS(loadConfig(base({{"MV_PREVIEW_MODE", "webrtc"}, {"MEDIAMTX_WHEP_PORT", "8110"}}), {}), ConfigError);
    CHECK_NOTHROW(loadConfig(base({{"MV_PREVIEW_MODE", "webrtc"}, {"MEDIAMTX_WHEP_PORT", "8110"}, {"PREVIEW_PUBLISH_URL", "rtsp://shared:8554"}}), {}));
}

TEST_CASE("preview mode: never both")
{
    auto const jpeg = previewPlan(loadConfig(base({}), {}));
    CHECK(jpeg.jpeg);
    CHECK_FALSE(jpeg.webrtc);
    CHECK_FALSE(jpeg.ownMediamtx);
    auto const own = previewPlan(loadConfig(base({{"MV_PREVIEW_MODE", "webrtc"}}), {}));
    CHECK_FALSE(own.jpeg);
    CHECK(own.webrtc);
    CHECK(own.ownMediamtx);
    CHECK(own.path == "mxl-multiviewer/heads");
    CHECK(own.publishUrl == "rtsp://127.0.0.1:8754/mxl-multiviewer/heads");
    auto const shared =
        previewPlan(loadConfig(base({{"MV_PREVIEW_MODE", "webrtc"}, {"PREVIEW_PUBLISH_URL", "rtsp://mediamtx:8554"}, {"PREVIEW_PATH_PREFIX", "show/mv"}}), {}));
    CHECK_FALSE(shared.jpeg);
    CHECK_FALSE(shared.ownMediamtx);
    CHECK(shared.publishUrl == "rtsp://mediamtx:8554/show/mv/heads");

    // JPEG mode serves /preview.jpg (204 until a head encoded one); WebRTC mode refuses it.
    CHECK(Fixture({}).get("/preview.jpg").status == 204);
    CHECK(Fixture({{"MV_PREVIEW_MODE", "webrtc"}}).get("/preview.jpg").status == 404);
}

TEST_CASE("mosaic tile map")
{
    VideoFormat const hd{1920, 1080, 50, 1};
    auto const one = mosaicRegion(1, 1, hd);
    CHECK((one.x == 0 && one.y == 0 && one.w == 1920 && one.h == 1080));
    for (int head = 1; head <= 4; ++head)
    {
        auto const quarter = mosaicRegion(head, 4, hd);
        CHECK(quarter.x == ((head - 1) % 2) * 960);
        CHECK(quarter.y == ((head - 1) / 2) * 540);
        CHECK((quarter.w == 960 && quarter.h == 540));
    }
    auto const uhd = mosaicRegion(2, 2, VideoFormat{3840, 2160, 50, 1});
    CHECK((uhd.x == 960 && uhd.y == 0 && uhd.w == 960 && uhd.h == 540));
    // 4:3 keeps its aspect, centred in its quarter.
    auto const sd = mosaicRegion(3, 3, VideoFormat{720, 540, 25, 1});
    CHECK((sd.x == 120 && sd.y == 540 && sd.w == 720 && sd.h == 540));

    Fixture fixture({{"MV_PREVIEW_MODE", "webrtc"}, {"MV_OUTPUTS", "4"}, {"MV_OUT4_FORMAT", "1280x720p50"}});
    auto const map = fixture.json("/api/v1/preview/map");
    CHECK(map.get("mode").get<std::string>() == "webrtc");
    CHECK(map.get("width").get<double>() == 1920);
    CHECK(map.get("height").get<double>() == 1080);
    auto const& heads = map.get("heads").get<picojson::array>();
    REQUIRE(heads.size() == 4);
    CHECK(heads[3].get("head").get<double>() == 4);
    CHECK(heads[3].get("x").get<double>() == 960);
    CHECK(heads[3].get("y").get<double>() == 540);
    CHECK(heads[3].get("w").get<double>() == 960);
    CHECK(heads[3].get("h").get<double>() == 540);
}

TEST_CASE("mosaic pictures: scale to NV12 and place")
{
    Frame422 frame;
    frame.allocate(64, 32, false);
    for (int y = 0; y < 32; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            frame.y[static_cast<std::size_t>(y * 64 + x)] = x < 32 ? 64 : 940;
        }
        for (int x = 0; x < 32; ++x)
        {
            frame.cb[static_cast<std::size_t>(y * 32 + x)] = x < 16 ? 512 : 960;
        }
    }
    // Area average, 10 to 8 bit: limited-range black 16, white 235, Cb 128 and 240.
    std::vector<std::uint8_t> nv12(32 * 16 * 3 / 2);
    scaleToNv12(frame, 32, 16, nv12.data());
    CHECK(int(nv12[0]) == 16);
    CHECK(int(nv12[31]) == 235);
    CHECK(int(nv12[32 * 16]) == 128);
    CHECK(int(nv12[32 * 16 + 1]) == 128);
    CHECK(int(nv12[32 * 16 + 30]) == 240);
    // Full scale stays 255 (no wrap to 0); upscaling repeats samples.
    frame.fill(1023, 512, 512);
    std::vector<std::uint8_t> big(128 * 64 * 3 / 2);
    scaleToNv12(frame, 128, 64, big.data());
    CHECK(int(big[0]) == 255);
    CHECK(int(big[128 * 64 - 1]) == 255);

    PreviewMosaic mosaic(2);
    std::vector<std::uint8_t> tile(960 * 540 * 3 / 2, 200);
    mosaic.put(2, VideoFormat{1920, 1080, 50, 1}, tile.data());
    std::vector<std::uint8_t> luma(1920 * 1080);
    std::vector<std::uint8_t> chroma(1920 * 540);
    mosaic.copy(luma.data(), 1920, chroma.data(), 1920);
    CHECK(int(luma[0]) == 16);
    CHECK(int(luma[959]) == 16);
    CHECK(int(luma[960]) == 200);
    CHECK(int(luma[539 * 1920 + 1919]) == 200);
    CHECK(int(luma[540 * 1920 + 1919]) == 16);
    CHECK(int(chroma[0]) == 128);
    CHECK(int(chroma[960]) == 200);
    // A 4:3 raster on the same head: the 16:9 picture's edges are black again.
    std::vector<std::uint8_t> narrow(720 * 540 * 3 / 2, 90);
    mosaic.put(2, VideoFormat{720, 540, 25, 1}, narrow.data());
    mosaic.copy(luma.data(), 1920, chroma.data(), 1920);
    CHECK(int(luma[960]) == 16);
    CHECK(int(luma[960 + 120]) == 90);
    CHECK(int(luma[960 + 120 + 719]) == 90);
    CHECK(int(luma[960 + 120 + 720]) == 16);
}

TEST_CASE("preview in /api/v1/info and /statusz")
{
    Fixture jpeg({});
    auto const info = jpeg.json("/api/v1/info");
    CHECK(info.get("preview_mode").get<std::string>() == "jpeg");
    CHECK(jpeg.json("/statusz").get("preview").get("mode").get<std::string>() == "jpeg");
    CHECK(jpeg.json("/statusz").get("inputs").is<picojson::array>());

    Fixture own({{"MV_PREVIEW_MODE", "webrtc"}, {"NMOS_HOST_ADDRESS", "10.1.2.3"}});
    auto const ownInfo = own.json("/api/v1/info").get("preview");
    CHECK(ownInfo.get("whep").get<std::string>() == "http://10.1.2.3:8789/mxl-multiviewer/heads/whep");
    CHECK(ownInfo.get("hls").get<std::string>() == "http://10.1.2.3:8788/mxl-multiviewer/heads/index.m3u8");
    CHECK_FALSE(ownInfo.get("public").get("whep").get<bool>());
    own.api.setPreviewStatus([] {
        PreviewStatus status;
        status.state = "publishing";
        status.encoder = "nvenc";
        status.frames = 42;
        status.mediamtxRunning = true;
        return status;
    });
    auto const status = own.json("/statusz").get("preview");
    CHECK(status.get("mode").get<std::string>() == "webrtc");
    CHECK(status.get("publish").get<std::string>() == "own");
    CHECK(status.get("publish_url").get<std::string>() == "rtsp://127.0.0.1:8754");
    CHECK(status.get("path").get<std::string>() == "mxl-multiviewer/heads");
    CHECK(status.get("state").get<std::string>() == "publishing");
    CHECK(status.get("encoder").get<std::string>() == "nvenc");
    CHECK(status.get("frames").get<double>() == 42);
    CHECK(status.get("mediamtx").get("running").get<bool>());

    Fixture shared({{"MV_PREVIEW_MODE", "webrtc"}, {"PREVIEW_PUBLISH_URL", "rtsp://mediamtx:8554"}, {"PREVIEW_PATH_PREFIX", "show/mv"},
        {"PREVIEW_WHEP_URL", "https://whep.example"}, {"PREVIEW_HLS_URL", "https://hls.example"}});
    auto const sharedInfo = shared.json("/api/v1/info").get("preview");
    CHECK(sharedInfo.get("whep").get<std::string>() == "https://whep.example/show/mv/heads/whep");
    CHECK(sharedInfo.get("hls").get<std::string>() == "https://hls.example/show/mv/heads/index.m3u8");
    CHECK(sharedInfo.get("public").get("whep").get<bool>());
    auto const sharedStatus = shared.json("/statusz").get("preview");
    CHECK(sharedStatus.get("publish").get<std::string>() == "shared");
    CHECK(sharedStatus.get("publish_url").get<std::string>() == "rtsp://mediamtx:8554");
    CHECK(sharedStatus.get("state").get<std::string>() == "connecting");
    CHECK_FALSE(sharedStatus.contains("mediamtx"));
}

TEST_CASE("widgets: list, pages, CSP and CORS")
{
    Fixture fixture({{"MV_OUTPUTS", "3"}, {"WIDGET_FRAME_ANCESTORS", "'self' https://designer.example https://*.ops.example"}});
    auto const list = fixture.json("/widgets").get<picojson::array>();
    REQUIRE(list.size() == 2);
    CHECK(list[0].get("id").get<std::string>() == "head");
    CHECK(list[0].get("min_size").get("w").get<double>() == 480);
    CHECK(list[0].get("min_size").get("h").get<double>() == 270);
    CHECK(list[0].get("params").get("properties").get("head").get("maximum").get<double>() == 3);
    CHECK(list[0].get("params").get("required").get<picojson::array>()[0].get<std::string>() == "head");
    CHECK(list[1].get("id").get<std::string>() == "tile-editor");
    CHECK(list[1].get("min_size").get("w").get<double>() == 600);
    CHECK(list[1].get("min_size").get("h").get<double>() == 400);
    CHECK(list[1].get("version").get<std::string>() == MV_VERSION);

    // CORS for an origin WIDGET_FRAME_ANCESTORS lists; none for others or without an Origin.
    auto const listed = fixture.get("/widgets", {}, {{"origin", "https://designer.example"}});
    CHECK(header(listed, "Access-Control-Allow-Origin") == "https://designer.example");
    CHECK(header(listed, "Vary") == "Origin");
    CHECK(header(fixture.get("/widgets", {}, {{"origin", "https://evil.example"}}), "Access-Control-Allow-Origin").empty());
    CHECK(header(fixture.get("/widgets", {}, {{"origin", "https://a.ops.example"}}), "Access-Control-Allow-Origin").empty());
    CHECK(header(fixture.get("/widgets"), "Access-Control-Allow-Origin").empty());
    Fixture any({{"WIDGET_FRAME_ANCESTORS", "*"}});
    CHECK(header(any.get("/widgets", {}, {{"origin", "http://x.example:8080"}}), "Access-Control-Allow-Origin") == "http://x.example:8080");

    // The pages: CSP frame-ancestors, no X-Frame-Options; bad parameters 400, unknown widgets 404.
    auto const page = fixture.get("/widget/head", "head=3&theme=transparent");
    CHECK(page.status == 200);
    CHECK(page.contentType.rfind("text/html", 0) == 0);
    CHECK(header(page, "Content-Security-Policy") == "frame-ancestors 'self' https://designer.example https://*.ops.example");
    CHECK(header(page, "X-Frame-Options").empty());
    CHECK(fixture.get("/widget/tile-editor", "head=1").status == 200);
    CHECK(fixture.get("/widget/head", "head=4").status == 400);
    CHECK(fixture.get("/widget/head", "head=0").status == 400);
    CHECK(fixture.get("/widget/head").status == 400);
    CHECK(fixture.get("/widget/head", "head=1&theme=blue").status == 400);
    CHECK(fixture.get("/widget/nope", "head=1").status == 404);
    CHECK(header(fixture.get("/api/v1/info"), "Content-Security-Policy").empty());
    CHECK(header(Fixture({}).get("/widget/head", "head=1"), "Content-Security-Policy") == "frame-ancestors 'self'");
    // WEB_ENABLE=false hides the pages; the list stays (a GET API).
    Fixture hidden({{"WEB_ENABLE", "false"}});
    CHECK(hidden.get("/widget/head", "head=1").status == 404);
    CHECK(hidden.get("/widgets").status == 200);
}

TEST_CASE("built-in MediaMTX config")
{
    auto const cfg = loadConfig(base({{"MV_PREVIEW_MODE", "webrtc"}, {"NMOS_HOST_ADDRESS", "10.1.2.3"}, {"MEDIAMTX_WHEP_PORT", "18789"}}), {});
    auto const yml = renderMediamtxConfig(cfg);
    CHECK(yml.find("rtspAddress: 127.0.0.1:8754\n") != std::string::npos);
    CHECK(yml.find("rtspTransports: [tcp]\n") != std::string::npos);
    CHECK(yml.find("webrtcAddress: :18789\n") != std::string::npos);
    CHECK(yml.find("hlsAddress: :8788\n") != std::string::npos);
    CHECK(yml.find("webrtcLocalUDPAddress: :8389\n") != std::string::npos);
    CHECK(yml.find("webrtcLocalTCPAddress: :8389\n") != std::string::npos);
    CHECK(yml.find("webrtcAdditionalHosts: [\"10.1.2.3\"]\n") != std::string::npos);
    CHECK(yml.find("api: false\n") != std::string::npos);
}

TEST_CASE("a supervised child is started again after it exits, and stopped")
{
    ChildProcess failing("test");
    failing.start({"/bin/sh", "-c", "exit 3"});
    CHECK(waitFor([&] { return failing.restarts() >= 1; }, 3000));
    failing.stop();
    CHECK_FALSE(failing.running());

    ChildProcess sleeper("test");
    sleeper.start({"sleep", "60"});
    REQUIRE(waitFor([&] { return sleeper.running(); }, 2000));
    auto const before = std::chrono::steady_clock::now();
    sleeper.stop();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
    CHECK_FALSE(sleeper.running());
    CHECK(sleeper.restarts() == 0);
}
