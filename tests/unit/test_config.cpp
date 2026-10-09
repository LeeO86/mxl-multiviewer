#include <doctest/doctest.h>

#include "app/runtime.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "domain/scan.hpp"
#include "layout/book.hpp"
#include "layout/migrate.hpp"
#include "media/timebase.hpp"
#include "nmos/ids.hpp"
#include "ops/api.hpp"
#include "ops/metrics.hpp"
#include "util/jsonutil.hpp"
#include "util/uuid.hpp"

#include <cstdlib>
#include <fstream>

using namespace mv;

TEST_CASE("video format tokens")
{
    auto const p50 = parseVideoFormat("1920x1080p50");
    CHECK(p50.width == 1920);
    CHECK(p50.height == 1080);
    CHECK(p50.rateNum == 50);
    CHECK(p50.rateDen == 1);
    CHECK(p50.token() == "1920x1080p50");
    auto const drop = parseVideoFormat("3840x2160p5994");
    CHECK(drop.rateNum == 60000);
    CHECK(drop.rateDen == 1001);
    CHECK(drop.token() == "3840x2160p5994");
    CHECK_THROWS_AS(parseVideoFormat("1920x1080i25"), ConfigError);
    CHECK_THROWS_AS(parseVideoFormat("1921x1080p50"), ConfigError);
}

TEST_CASE("config precedence and validation")
{
    std::map<std::string, std::string> env{{"MV_MAX_INPUTS", "8"}, {"MV_BACKEND", "cpu"}};
    std::map<std::string, std::string> file{{"MV_MAX_INPUTS", "4"}, {"WEB_PORT", "18110"}, {"MV_OUTPUT_FORMAT", "1280x720p25"}};
    auto const cfg = loadConfig(env, file);
    CHECK(cfg.maxInputs == 8);
    CHECK(cfg.backend == "cpu");
    CHECK(cfg.webPort == 18110);
    CHECK(cfg.outputFormat.height == 720);
    CHECK(cfg.nmosSeed.find("multiviewer") != std::string::npos);
    CHECK_THROWS_AS(loadConfig({}, {{"MV_NO_SUCH", "1"}}), ConfigError);
    auto const withPins = loadConfig({{"NMOS_CPP_REF", "fe30384"}, {"MXL_REF", "218ddaa"}, {"PATH", "/usr/bin"}, {"MV_BACKEND", "cpu"}}, {});
    CHECK(withPins.backend == "cpu");
    CHECK_THROWS_AS(loadConfig({{"MV_MAX_INPUTS", "100"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"WEB_PORT", "8110"}, {"NMOS_PORT", "8110"}, {"NMOS_ENABLE", "true"}}, {}), ConfigError);
    auto const aliased = loadConfig({{"MXL_OUTPUT_DOMAIN_DIR", "/tmp/mv-alias"}, {"NMOS_ENABLE", "false"}}, {});
    CHECK(aliased.outputDomainDir == "/tmp/mv-alias");
    auto const primary = loadConfig({{"MV_OUTPUT_DOMAIN_DIR", "/tmp/mv-primary"}, {"MXL_OUTPUT_DOMAIN_DIR", "/tmp/mv-alias"}, {"NMOS_ENABLE", "false"}}, {});
    CHECK(primary.outputDomainDir == "/tmp/mv-primary");
    auto const query = loadConfig({{"NMOS_REGISTRY_ADDRESS", "10.1.1.9"}, {"NMOS_REGISTRY_PORT", "4000"}, {"NMOS_HOST_ADDRESS", "10.1.1.8"}, {"NMOS_ENABLE", "true"}}, {});
    CHECK(query.nmosQueryAddress == "10.1.1.9");
    CHECK(query.nmosQueryPort == 4001);
    auto const tags = loadConfig({{"NMOS_ENABLE", "false"}, {"NMOS_TAGS", "{\"urn:x-srf:production\":[\"sport-sa\"],\"urn:x-srf:function\":[\"mv1\"]}"}}, {});
    CHECK(tags.nmosTags.at("urn:x-srf:production").at(0) == "sport-sa");
    CHECK(tags.cleanupOnExit == false);
    CHECK(tags.stateDir == "/config");
    CHECK_THROWS_AS(loadConfig({{"NMOS_HOST_ADDRESS", "127.0.0.1"}, {"NMOS_ENABLE", "true"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"NMOS_HOST_ADDRESS", "0.0.0.0"}, {"NMOS_ENABLE", "true"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"NMOS_HOST_ADDRESS", "multiviewer.local"}, {"NMOS_ENABLE", "true"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"NMOS_TAGS", "[]"}, {"NMOS_ENABLE", "false"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"NMOS_REGISTRY_PORT", "65535"}, {"NMOS_ENABLE", "false"}}, {}), ConfigError);
    auto const queryOverride = loadConfig(
        {{"NMOS_REGISTRY_ADDRESS", "10.1.1.9"},
         {"NMOS_REGISTRY_PORT", "4000"},
         {"NMOS_QUERY_ADDRESS", "10.2.2.9"},
         {"NMOS_QUERY_PORT", "4100"},
         {"NMOS_HOST_ADDRESS", "10.1.1.8"},
         {"NMOS_LABEL", "wall"},
         {"MXL_OUTPUT_DOMAIN_ID", "dddddddd-dddd-4ddd-8ddd-dddddddddddd"},
         {"MXL_CLEANUP_ON_EXIT", "true"},
         {"NMOS_ENABLE", "true"}},
        {});
    CHECK(queryOverride.nmosQueryAddress == "10.2.2.9");
    CHECK(queryOverride.nmosQueryPort == 4100);
    CHECK(queryOverride.nmosLabel == "wall");
    CHECK(queryOverride.outputDomainId == "dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    CHECK(queryOverride.cleanupOnExit);
}

TEST_CASE("config file layer")
{
    auto const dir = std::string("/tmp/mv-config-test");
    std::system(("mkdir -p " + dir).c_str());
    auto const path = dir + "/mv.json";
    {
        std::ofstream out(path);
        out << "{\"MV_BACKEND\":\"cpu\",\"WEB_PORT\":\"18111\"}";
    }
    ConfigStore store({{"MV_MAX_INPUTS", "4"}}, path);
    auto const cfg = store.effectiveConfig();
    CHECK(cfg.backend == "cpu");
    CHECK(cfg.maxInputs == 4);
    CHECK(store.sourceOf("MV_BACKEND") == SettingSource::File);
    CHECK(store.sourceOf("MV_MAX_INPUTS") == SettingSource::Env);
    auto const updated = store.update({{"MV_HOLD_MS", std::string("250")}});
    CHECK(std::holds_alternative<ConfigStore::UpdateResult>(updated));
    CHECK(store.effectiveConfig().holdMs == 250);
    auto const rejected = store.update({{"MV_MAX_INPUTS", std::string("3")}});
    CHECK(std::holds_alternative<std::string>(rejected));
    ConfigStore envAlias({{"MXL_OUTPUT_DOMAIN_DIR", "/tmp/from-env"}, {"NMOS_ENABLE", "false"}}, path);
    auto const blocked = envAlias.update({{"MV_OUTPUT_DOMAIN_DIR", std::string("/tmp/from-file")}});
    CHECK(std::holds_alternative<std::string>(blocked));
    CHECK(envAlias.effectiveConfig().outputDomainDir == "/tmp/from-env");
    CHECK(envAlias.sourceOf("MV_OUTPUT_DOMAIN_DIR") == SettingSource::Env);
}

TEST_CASE("web disable keeps probes and blocks mutations")
{
    std::map<std::string, std::string> env{{"NMOS_ENABLE", "false"}, {"WEB_ENABLE", "false"}, {"MV_BACKEND", "cpu"}};
    ConfigStore store(env, std::nullopt);
    auto const cfg = store.effectiveConfig();
    LayoutBookStore layouts(cfg.maxInputs, cfg.activeLayout, "");
    RuntimeModel runtime(cfg);
    Metrics metrics;
    Api api(cfg, store, layouts, runtime, metrics);
    CHECK(api.handle(HttpRequest{"GET", "/api/v1/info", {}, {}, {}}).status == 200);
    CHECK(api.handle(HttpRequest{"GET", "/livez", {}, {}, {}}).status == 503);
    CHECK(api.handle(HttpRequest{"GET", "/metrics", {}, {}, {}}).status == 200);
    CHECK(api.handle(HttpRequest{"GET", "/preview.jpg", {}, {}, {}}).status == 404);
    CHECK(api.handle(HttpRequest{"POST", "/api/v1/config/import", {}, "{}", {}}).status == 404);
    CHECK(api.handle(HttpRequest{"GET", "/api/v1/config/export", {}, {}, {}}).status == 200);
}

TEST_CASE("layout routes decode percent-encoded names and keep the json valid")
{
    std::map<std::string, std::string> env{{"NMOS_ENABLE", "false"}, {"MV_BACKEND", "cpu"}};
    ConfigStore store(env, std::nullopt);
    auto const cfg = store.effectiveConfig();
    LayoutBookStore layouts(cfg.maxInputs, cfg.activeLayout, "");
    RuntimeModel runtime(cfg);
    Metrics metrics;
    Api api(cfg, store, layouts, runtime, metrics);
    // The UI encodes names: the preset "2+8" arrives as "2%2B8".
    CHECK(api.handle(HttpRequest{"POST", "/api/v1/layouts/2%2B8/activate", {}, {}, {}}).status == 200);
    CHECK(layouts.activeName() == "2+8");
    auto const body = R"({"version":1,"name":"ignored","background":"#203040","tiles":[{"id":"c","content":"clock","clock_style":"analog","rect":{"x":0,"y":0,"w":1,"h":1}}]})";
    CHECK(api.handle(HttpRequest{"PUT", "/api/v1/layouts/My%20%22wall%22", {}, body, {}}).status == 200);
    CHECK(layouts.layout("My \"wall\"")->name == "My \"wall\"");
    std::string error;
    auto const book = json::parse(api.handle(HttpRequest{"GET", "/api/v1/layouts", {}, {}, {}}).body, error);
    CHECK(error.empty());
    auto const info = json::parse(api.handle(HttpRequest{"GET", "/api/v1/info", {}, {}, {}}).body, error);
    CHECK(error.empty());
    CHECK(info.get("grid").get<double>() == doctest::Approx(24));
    CHECK(info.get("preview_fps").get<double>() == doctest::Approx(5));
    auto const inputs = json::parse(api.handle(HttpRequest{"GET", "/api/v1/inputs", {}, {}, {}}).body, error);
    CHECK(error.empty());
    CHECK(inputs.get("inputs").get(0).get("hold_dbfs").get<picojson::array>().size() == 16);
    CHECK(api.handle(HttpRequest{"GET", "/preview.jpg", "head=2", {}, {}}).status == 204);
}

TEST_CASE("inputs carry the TSL tally fields; layouts carry tally_text")
{
    std::map<std::string, std::string> env{{"NMOS_ENABLE", "false"}, {"MV_BACKEND", "cpu"}};
    ConfigStore store(env, std::nullopt);
    auto const cfg = store.effectiveConfig();
    auto const dir = std::string("/tmp/mv-tally-test");
    std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    LayoutBookStore layouts(cfg.maxInputs, cfg.activeLayout, dir + "/layouts.json");
    RuntimeModel runtime(cfg);
    Metrics metrics;
    Api api(cfg, store, layouts, runtime, metrics);
    TallyUpdate update;
    update.lh = 1;
    update.rh = 2;
    update.textValue = "CAM 2";
    runtime.setTally(2, update);
    for (auto const* path : {"/api/v1/inputs", "/statusz"})
    {
        std::string error;
        auto const doc = json::parse(api.handle(HttpRequest{"GET", path, {}, {}, {}}).body, error);
        REQUIRE(error.empty());
        auto const& input = doc.get("inputs").get(1);
        CHECK(input.get("tsl_lh").get<double>() == 1);
        CHECK(input.get("tsl_rh").get<double>() == 2);
        CHECK(input.get("tsl_text_tally").get<double>() == 0);
        CHECK(input.get("tally").get<double>() == 2);
        CHECK(input.get("tsl_text").get<std::string>() == "CAM 2");
        CHECK(doc.get("inputs").get(0).get("tsl_lh").get<double>() == 0);
    }
    CHECK(api.eventsJson().find("\"tsl_lh\":1,\"tsl_rh\":2,\"tsl_text_tally\":0") != std::string::npos);

    auto const put = [&](std::string const& body) { return api.handle(HttpRequest{"PUT", "/api/v1/layouts/wall", {}, body, {}}); };
    auto const tile = std::string(R"({"id":"a","input":1,"tally_text":false,"rect":{"x":0,"y":0,"w":1,"h":1}})");
    auto const rejected = put(R"({"version":1,"name":"wall","tally_text":"on","tiles":[]})");
    CHECK(rejected.status == 400);
    CHECK(rejected.body.find("tally_text") != std::string::npos);
    CHECK(put(R"({"version":1,"name":"wall","tally_text":true,"tiles":[{"id":"a","tally_text":"off","rect":{"x":0,"y":0,"w":1,"h":1}}]})").status == 400);
    auto const saved = put(R"({"version":1,"name":"wall","tally_text":true,"tiles":[)" + tile + "]}");
    CHECK(saved.status == 200);
    CHECK(layouts.layout("wall")->tallyText);
    CHECK(layouts.layout("wall")->tiles[0].tallyText == false);
    auto const book = api.handle(HttpRequest{"GET", "/api/v1/layouts", {}, {}, {}}).body;
    CHECK(book.find(R"("name":"wall","background":"#101010","tally_text":true)") != std::string::npos);
    // Persisted: a restart reads the same values back.
    LayoutBookStore reloaded(cfg.maxInputs, cfg.activeLayout, dir + "/layouts.json");
    CHECK(reloaded.layout("wall")->tallyText);
    CHECK(reloaded.layout("wall")->tiles[0].tallyText == false);
}

TEST_CASE("outputs, deletes, and imports keep every head on a layout that exists")
{
    std::map<std::string, std::string> env{{"NMOS_ENABLE", "false"}, {"MV_BACKEND", "cpu"}, {"MV_OUTPUTS", "2"}};
    ConfigStore store(env, std::nullopt);
    auto const cfg = store.effectiveConfig();
    LayoutBookStore layouts(cfg.maxInputs, cfg.activeLayout, "");
    RuntimeModel runtime(cfg);
    Metrics metrics;
    Api api(cfg, store, layouts, runtime, metrics);
    auto const put = [&](std::string const& body) { return api.handle(HttpRequest{"PUT", "/api/v1/outputs/2", {}, body, {}}).status; };
    CHECK(put(R"({"layout":"nope"})") == 404);
    CHECK(put(R"({"audio_follow":17})") == 400);
    CHECK(put(R"({"audio_follow":-1})") == 400);
    CHECK(put(R"({"audio_follow":1.5})") == 400);
    CHECK(put(R"({"layout":"3x3","audio_follow":99})") == 400);
    CHECK(runtime.headLayout(2) != "3x3");
    CHECK(put(R"({"layout":"3x3","audio_follow":0})") == 200);
    CHECK(runtime.headLayout(2) == "3x3");
    CHECK(runtime.headAudioFollow(2) == 0);
    // A layout on a head cannot be deleted, even when it is not the book's active one.
    CHECK(api.handle(HttpRequest{"DELETE", "/api/v1/layouts/3x3", {}, {}, {}}).status == 409);
    // An import without that layout moves the head to the imported book's active layout.
    auto const doc = R"({"layouts":{"version":1,"active":"wall","layouts":[{"version":1,"name":"wall","tiles":[]},{"version":1,"name":"2x2","tiles":[]}]}})";
    auto const result = api.handle(HttpRequest{"POST", "/api/v1/config/import", {}, doc, {}});
    CHECK(result.status == 200);
    CHECK(result.body.find("\"heads_moved\":[2]") != std::string::npos);
    CHECK(runtime.headLayout(1) == "2x2");
    CHECK(runtime.headLayout(2) == "wall");
    // A book without layouts is refused.
    CHECK(api.handle(HttpRequest{"POST", "/api/v1/config/import", {}, R"({"layouts":{"version":1,"active":"x","layouts":[]}})", {}}).status == 400);
}

TEST_CASE("an unreadable layouts file is moved aside, an older one is repaired")
{
    auto const dir = std::string("/tmp/mv-layouts-test");
    std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    auto const path = dir + "/layouts.json";
    {
        std::ofstream out(path);
        out << "{\"version\":1,\"active\":\"mine\",\"layouts\":[{\"version\":1,\"name\":\"mine\",\"tiles\":[{\"id\":\"a\",\"rect\":{\"x\":0,\"y\":0,\"w\":2,\"h\":1}}]}]}";
    }
    {
        LayoutBookStore broken(16, "2x2", path);
        CHECK(broken.activeName() == "2x2");
        CHECK(std::ifstream(path + ".bad").good());
        CHECK_FALSE(std::ifstream(path).good());
    }
    {
        std::ofstream out(path);
        out << "{\"version\":1,\"active\":\"mine\",\"layouts\":[{\"version\":1,\"name\":\"mine\",\"tiles\":[{\"id\":\"a\",\"zone_green\":-3,\"zone_amber\":-30,"
               "\"rect\":{\"x\":0,\"y\":0,\"w\":1,\"h\":1}}]}]}";
    }
    LayoutBookStore repaired(16, "2x2", path);
    CHECK(repaired.activeName() == "mine");
    CHECK(repaired.has("mine"));
    CHECK(repaired.has("4x4"));
    CHECK(repaired.layout("mine")->tiles[0].zoneGreen == doctest::Approx(-30));
}

namespace
{
std::string readFile(std::string const& path)
{
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("a 1.1.x layout file: presets migrate once, active and head choices survive restarts")
{
    auto const dir = std::string("/tmp/mv-migrate-test");
    std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    auto const path = dir + "/layouts.json";
    // As 1.1.3 wrote it: no preset_revision, no heads, active 3x3, one edited preset.
    LayoutBook old;
    old.active = "3x3";
    old.layouts = legacyPresets(16);
    old.layouts[5].tiles[0].umdText = "EDITED"; // 2+8
    auto body = bookToJson(old);
    auto const marker = std::string("\"preset_revision\":2,\"heads\":{},");
    REQUIRE(body.find(marker) != std::string::npos);
    body.erase(body.find(marker), marker.size());
    {
        std::ofstream out(path);
        out << body;
    }
    {
        LayoutBookStore store(16, "2x2", path);
        // MV_ACTIVE_LAYOUT (2x2) is only the start of a book without a file.
        CHECK(store.startLayout(1, "2x2", false) == "3x3");
        CHECK(store.layout("4x4")->tiles[0].audioBars);
        CHECK_FALSE(store.layout("2+8")->tiles[0].audioBars);
        CHECK(store.layout("2+8")->tiles[0].umdText == "EDITED");
        CHECK(readFile(path + ".bak") == body);
    }
    auto const migrated = readFile(path);
    CHECK(migrated.find("\"preset_revision\":2") != std::string::npos);
    {
        // A second start changes nothing.
        LayoutBookStore again(16, "2x2", path);
        CHECK(readFile(path) == migrated);
        CHECK(again.startLayout(1, "2x2", false) == "3x3");
        again.saveHead(1, "4x4");
    }
    {
        LayoutBookStore third(16, "2x2", path);
        // Saved head layout, then MV_OUT<h>_LAYOUT, then the book's active layout.
        CHECK(third.startLayout(1, "2x2", false) == "4x4");
        CHECK(third.startLayout(2, "1+5", true) == "1+5");
        CHECK(third.startLayout(2, "nope", true) == "3x3");
        CHECK(third.activate("2+6", 2));
    }
    LayoutBookStore fourth(16, "2x2", path);
    CHECK(fourth.startLayout(1, "2x2", false) == "2+6");
    CHECK(fourth.startLayout(2, "1+5", true) == "2+6");
    CHECK(readFile(path + ".bak") == body);

    // PUT /api/v1/outputs/{h} saves the head's layout for the next start.
    std::map<std::string, std::string> env{{"NMOS_ENABLE", "false"}, {"MV_BACKEND", "cpu"}};
    ConfigStore store(env, std::nullopt);
    RuntimeModel runtime(store.effectiveConfig());
    Metrics metrics;
    Api api(store.effectiveConfig(), store, fourth, runtime, metrics);
    CHECK(api.handle(HttpRequest{"PUT", "/api/v1/outputs/1", {}, R"({"layout":"1+7"})", {}}).status == 200);
    LayoutBookStore fifth(16, "2x2", path);
    CHECK(fifth.startLayout(1, "2x2", false) == "1+7");
    auto const presets = api.handle(HttpRequest{"GET", "/api/v1/presets", {}, {}, {}});
    CHECK(presets.status == 200);
    CHECK(presets.body.find("\"audio_bars\":true") != std::string::npos);
}

TEST_CASE("an activation is not lost to the composer's status report")
{
    std::map<std::string, std::string> env{{"NMOS_ENABLE", "false"}, {"MV_BACKEND", "cpu"}};
    ConfigStore store(env, std::nullopt);
    RuntimeModel runtime(store.effectiveConfig());
    // The composer took its copy of the head before the API changed it.
    auto view = runtime.output(1);
    runtime.setHeadLayout(1, "3x3");
    runtime.setHeadAudio(1, 4, 2);
    view.frames = 10;
    runtime.setOutput(view);
    CHECK(runtime.headLayout(1) == "3x3");
    CHECK(runtime.headAudioFollow(1) == 4);
    CHECK(runtime.output(1).frames == 10);
}

TEST_CASE("uuid v5 ids")
{
    auto const ids = makeNmosIds("test-seed");
    CHECK(ids.node == "3c023964-57f2-59f1-b597-e33a48ebb58a");
    CHECK(ids.device == "ccf75a52-bf29-5a09-94cb-4b3b599218cc");
    CHECK(ids.domain == "3e896ea1-6698-57a5-9725-a88effb5c00c");
    CHECK(ids.videoReceiver(1) == "9ae8cfc1-92db-5ab1-a639-1fafcd7cb218");
    CHECK(ids.videoFlow(1, "1920x1080p50") == "ad33f609-8455-58ef-9f6c-7023af632082");
    CHECK(ids.videoReceiver(1) != ids.audioReceiver(1));
    CHECK(isUuid(ids.node));
    CHECK_FALSE(isUuid("not-a-uuid"));
}

TEST_CASE("tai index rounding matches MXL")
{
    CHECK(timestampToIndex(30000, 1001, 0) == 0);
    auto const second = (1001ull * 1000000000ull + 30000ull / 2ull) / 30000ull;
    CHECK(timestampToIndex(30000, 1001, second) == 1);
    CHECK(indexToTimestamp(30000, 1001, 0) == 0);
    CHECK(indexToTimestamp(30000, 1001, 1) == second);
    CHECK(timestampToIndex(50, 1, indexToTimestamp(50, 1, 123456)) == 123456);
    CHECK(timestampToIndex(0, 1, 10) == UINT64_MAX);
}

TEST_CASE("domain scan keeps mirror domains and ignores unknown fields")
{
    auto const root = std::string("/tmp/mv-domain-scan");
    std::system(("rm -rf " + root + " && mkdir -p " + root + "/cam " + root + "/mirror-abc " + root + "/not-a-domain").c_str());
    {
        std::ofstream out(root + "/cam/domain_def.json");
        out << "{\"id\":\"11111111-1111-1111-1111-111111111111\",\"label\":\"Cam\",\"extra\":true}";
    }
    {
        std::ofstream out(root + "/mirror-abc/domain_def.json");
        out << "{\"id\":\"22222222-2222-2222-2222-222222222222\",\"label\":\"mirror\",\"x-mxl-fabrics-agent\":{\"mirror\":true,\"source_host_id\":\"a\"}}";
    }
    auto const domains = scanDomains(root);
    REQUIRE(domains.size() == 2);
    auto const mirror = resolveDomain(root, "22222222-2222-2222-2222-222222222222");
    REQUIRE(mirror.has_value());
    CHECK(mirror->mirror);
    CHECK(mirror->path.find("mirror-abc") != std::string::npos);
    auto const local = resolveDomain(root, "11111111-1111-1111-1111-111111111111");
    REQUIRE(local.has_value());
    CHECK_FALSE(local->mirror);
    CHECK(local->label == "Cam");
    CHECK_FALSE(resolveDomain(root, "missing").has_value());
}
