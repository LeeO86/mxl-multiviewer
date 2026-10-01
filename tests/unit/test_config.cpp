#include <doctest/doctest.h>

#include "config/config.hpp"
#include "config/store.hpp"
#include "domain/scan.hpp"
#include "media/timebase.hpp"
#include "nmos/ids.hpp"
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
    CHECK_THROWS_AS(loadConfig({{"MV_MAX_INPUTS", "100"}}, {}), ConfigError);
    CHECK_THROWS_AS(loadConfig({{"WEB_PORT", "8110"}, {"NMOS_PORT", "8110"}, {"NMOS_ENABLE", "true"}}, {}), ConfigError);
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
