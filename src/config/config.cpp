#include "config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>
#include <unistd.h>

namespace mv
{
namespace
{
std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

bool parseBool(std::string const& text, bool& out)
{
    auto const v = lower(text);
    if (v == "1" || v == "true" || v == "yes" || v == "on")
    {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off")
    {
        out = false;
        return true;
    }
    return false;
}

bool parseInt(std::string const& text, long& out)
{
    if (text.empty())
    {
        return false;
    }
    char* end = nullptr;
    out = std::strtol(text.c_str(), &end, 10);
    return end != nullptr && *end == '\0';
}

bool parseDouble(std::string const& text, double& out)
{
    if (text.empty())
    {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != nullptr && *end == '\0';
}

std::optional<std::string> pick(std::string const& key, std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file,
    char const* fallback)
{
    if (auto const it = env.find(key); it != env.end())
    {
        return it->second;
    }
    if (auto const it = file.find(key); it != file.end())
    {
        return it->second;
    }
    if (fallback != nullptr)
    {
        return std::string(fallback);
    }
    return std::nullopt;
}

int requireInt(std::string const& key, std::string const& text, long min, long max)
{
    long value = 0;
    if (!parseInt(text, value) || value < min || value > max)
    {
        throw ConfigError(key + " must be an integer in [" + std::to_string(min) + "," + std::to_string(max) + "]");
    }
    return static_cast<int>(value);
}

bool requireBool(std::string const& key, std::string const& text)
{
    bool value = false;
    if (!parseBool(text, value))
    {
        throw ConfigError(key + " must be a boolean");
    }
    return value;
}

void requireEnum(std::string const& key, std::string const& text, std::set<std::string> const& allowed)
{
    if (!allowed.count(text))
    {
        throw ConfigError(key + " has an unsupported value");
    }
}

std::optional<std::pair<int, int>> parseRate(std::string const& token)
{
    if (token == "2398")
    {
        return std::pair<int, int>{24000, 1001};
    }
    if (token == "2997")
    {
        return std::pair<int, int>{30000, 1001};
    }
    if (token == "5994")
    {
        return std::pair<int, int>{60000, 1001};
    }
    auto const slash = token.find('/');
    if (slash != std::string::npos)
    {
        long num = 0;
        long den = 0;
        if (!parseInt(token.substr(0, slash), num) || !parseInt(token.substr(slash + 1), den) || num <= 0 || den <= 0)
        {
            return std::nullopt;
        }
        return std::pair<int, int>{static_cast<int>(num), static_cast<int>(den)};
    }
    long num = 0;
    if (!parseInt(token, num) || num <= 0)
    {
        return std::nullopt;
    }
    return std::pair<int, int>{static_cast<int>(num), 1};
}

void validateMap(std::string const& text, int maxInputs)
{
    if (text.empty())
    {
        return;
    }
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        if (item.empty())
        {
            throw ConfigError("TSL_MAP contains an empty entry");
        }
        auto const colon = item.find(':');
        if (colon == std::string::npos)
        {
            throw ConfigError("TSL_MAP entries must look like display:input");
        }
        long display = 0;
        long input = 0;
        if (!parseInt(item.substr(0, colon), display) || !parseInt(item.substr(colon + 1), input) || display < 0 || input < 1 || input > maxInputs)
        {
            throw ConfigError("TSL_MAP entry is out of range");
        }
    }
}

void checkPorts(Config const& cfg)
{
    std::vector<std::pair<std::string, int>> ports{{"WEB_PORT", cfg.webPort}};
    if (cfg.nmosEnable)
    {
        ports.push_back({"NMOS_PORT", cfg.nmosPort});
        ports.push_back({"NMOS_PORT+1", cfg.nmosPort + 1});
    }
    if (cfg.tslEnable)
    {
        ports.push_back({"TSL_UDP_PORT", cfg.tslUdpPort});
        ports.push_back({"TSL_TCP_PORT", cfg.tslTcpPort});
    }
    for (std::size_t i = 0; i < ports.size(); ++i)
    {
        if (ports[i].second < 1 || ports[i].second > 65535)
        {
            throw ConfigError(ports[i].first + " is not a valid port");
        }
        for (std::size_t j = i + 1; j < ports.size(); ++j)
        {
            if (ports[i].second == ports[j].second)
            {
                throw ConfigError(ports[i].first + " collides with " + ports[j].first);
            }
        }
    }
}
} // namespace

std::string VideoFormat::token() const
{
    if (rateDen == 1001 && rateNum == 24000)
    {
        return std::to_string(width) + "x" + std::to_string(height) + "p2398";
    }
    if (rateDen == 1001 && rateNum == 30000)
    {
        return std::to_string(width) + "x" + std::to_string(height) + "p2997";
    }
    if (rateDen == 1001 && rateNum == 60000)
    {
        return std::to_string(width) + "x" + std::to_string(height) + "p5994";
    }
    if (rateDen == 1)
    {
        return std::to_string(width) + "x" + std::to_string(height) + "p" + std::to_string(rateNum);
    }
    return std::to_string(width) + "x" + std::to_string(height) + "p" + std::to_string(rateNum) + "/" + std::to_string(rateDen);
}

std::string formatLabel(int width, int height, int rateNum, int rateDen, bool interlaced)
{
    VideoFormat format{width, height, rateNum, rateDen};
    auto token = format.token();
    auto const p = token.find('p');
    if (p != std::string::npos)
    {
        std::string rate = token.substr(p + 1);
        char kind = interlaced ? 'i' : 'p';
        if (width >= 3840 && height >= 2160)
        {
            return std::string("2160") + kind + rate;
        }
        if (height >= 1080)
        {
            return std::string("1080") + kind + rate;
        }
        if (height >= 720)
        {
            return std::string("720") + kind + rate;
        }
        return std::to_string(width) + "x" + std::to_string(height) + kind + rate;
    }
    return token;
}

VideoFormat parseVideoFormat(std::string const& token)
{
    auto const x = token.find('x');
    auto const p = token.find('p', x == std::string::npos ? 0 : x);
    if (x == std::string::npos || p == std::string::npos || token.find('i') != std::string::npos)
    {
        throw ConfigError("video format must look like 1920x1080p50 (progressive only)");
    }
    long width = 0;
    long height = 0;
    if (!parseInt(token.substr(0, x), width) || !parseInt(token.substr(x + 1, p - x - 1), height))
    {
        throw ConfigError("video format raster is not numeric");
    }
    if ((width % 2) != 0 || (height % 2) != 0 || width < 2 || height < 2 || width > 3840 || height > 2160)
    {
        throw ConfigError("video format raster must be even and at most 3840x2160");
    }
    auto const rate = parseRate(token.substr(p + 1));
    if (!rate)
    {
        throw ConfigError("video format rate is not supported");
    }
    double const hz = static_cast<double>(rate->first) / static_cast<double>(rate->second);
    if (hz < 23.0 || hz > 60.5)
    {
        throw ConfigError("video format rate is outside 23.98–60");
    }
    return VideoFormat{static_cast<int>(width), static_cast<int>(height), rate->first, rate->second};
}

std::vector<SettingDef> const& settingSchema()
{
    static std::vector<SettingDef> const schema = {
        {"HOST_ID", "", true},
        {"MXL_DOMAIN_SCAN_PATH", "/Volumes/mxl", true},
        {"MV_OUTPUT_DOMAIN_DIR", "/Volumes/mxl/multiviewer", true},
        {"MV_OUTPUT_DOMAIN_ID", "", true},
        {"MV_BACKEND", "auto", true},
        {"MV_MAX_INPUTS", "16", true},
        {"MV_OUTPUTS", "1", true},
        {"MV_OUTPUT_FORMAT", "1920x1080p50", false},
        {"MV_INPUT_OFFSET_GRAINS", "2", false},
        {"MV_HOLD_MS", "1000", false},
        {"MV_HISTORY_DURATION_NS", "200000000", true},
        {"MV_LAYOUTS_FILE", "", false},
        {"MV_ACTIVE_LAYOUT", "2x2", false},
        {"MV_AUDIO_CHANNELS", "2", false},
        {"MV_AUDIO_FOLLOW", "1", false},
        {"MV_OVERLAY_HZ", "25", false},
        {"MV_PREVIEW_FPS", "5", false},
        {"MV_PREVIEW_WIDTH", "480", false},
        {"MV_GRID", "24", false},
        {"MV_BLACK_Y", "32", false},
        {"MV_SILENCE_DBFS", "-60", false},
        {"MV_CLIP_LINEAR", "0.999", false},
        {"MV_ALARM_DEBOUNCE_MS", "500", false},
        {"MV_ALARM_CLEAR_MS", "500", false},
        {"MV_BACKGROUND_FILE", "", false},
        {"MV_CONFIG_FILE", "", true},
        {"NMOS_ENABLE", "true", true},
        {"NMOS_REGISTRY_ADDRESS", "", true},
        {"NMOS_REGISTRY_PORT", "3210", true},
        {"NMOS_DNS_SD", "false", true},
        {"NMOS_PORT", "3262", true},
        {"NMOS_SEED", "", true},
        {"WEB_ENABLE", "true", true},
        {"WEB_PORT", "8110", true},
        {"TSL_ENABLE", "true", true},
        {"TSL_UDP_PORT", "8910", true},
        {"TSL_TCP_PORT", "8911", true},
        {"TSL_V31", "false", true},
        {"TSL_SCREEN", "-1", false},
        {"TSL_MAP", "", false},
        {"LOG_LEVEL", "info", false},
        {"LOG_FORMAT", "json", true},
        {"SHUTDOWN_TIMEOUT_S", "10", true},
        {"MV_OUT1_FORMAT", "", false},
        {"MV_OUT1_LAYOUT", "", false},
        {"MV_OUT1_AUDIO_FOLLOW", "", false},
        {"MV_OUT1_AUDIO_CHANNELS", "", false},
        {"MV_OUT2_FORMAT", "", false},
        {"MV_OUT2_LAYOUT", "", false},
        {"MV_OUT2_AUDIO_FOLLOW", "", false},
        {"MV_OUT2_AUDIO_CHANNELS", "", false},
        {"MV_OUT3_FORMAT", "", false},
        {"MV_OUT3_LAYOUT", "", false},
        {"MV_OUT3_AUDIO_FOLLOW", "", false},
        {"MV_OUT3_AUDIO_CHANNELS", "", false},
    };
    return schema;
}

bool knownSetting(std::string const& key)
{
    for (auto const& def : settingSchema())
    {
        if (key == def.name)
        {
            return true;
        }
    }
    return false;
}

std::string hostnameString()
{
    char buffer[256] = {};
    if (gethostname(buffer, sizeof(buffer) - 1) != 0)
    {
        return "multiviewer";
    }
    return buffer;
}

Config loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file)
{
    for (auto const& [key, value] : file)
    {
        (void)value;
        if (!knownSetting(key))
        {
            throw ConfigError("unknown config key " + key);
        }
    }
    auto raw = [&](char const* key) {
        char const* fallback = nullptr;
        for (auto const& def : settingSchema())
        {
            if (std::string(def.name) == key)
            {
                fallback = def.defaultValue;
                break;
            }
        }
        return pick(key, env, file, fallback).value_or("");
    };

    Config cfg;
    cfg.hostId = raw("HOST_ID");
    if (cfg.hostId.empty())
    {
        cfg.hostId = hostnameString();
    }
    cfg.scanPath = raw("MXL_DOMAIN_SCAN_PATH");
    cfg.outputDomainDir = raw("MV_OUTPUT_DOMAIN_DIR");
    cfg.outputDomainId = raw("MV_OUTPUT_DOMAIN_ID");
    cfg.backend = lower(raw("MV_BACKEND"));
    requireEnum("MV_BACKEND", cfg.backend, {"auto", "cuda", "cpu"});
    cfg.maxInputs = requireInt("MV_MAX_INPUTS", raw("MV_MAX_INPUTS"), 1, 32);
    cfg.outputs = requireInt("MV_OUTPUTS", raw("MV_OUTPUTS"), 1, 3);
    cfg.outputFormat = parseVideoFormat(raw("MV_OUTPUT_FORMAT"));
    cfg.inputOffsetGrains = requireInt("MV_INPUT_OFFSET_GRAINS", raw("MV_INPUT_OFFSET_GRAINS"), 0, 30);
    cfg.holdMs = requireInt("MV_HOLD_MS", raw("MV_HOLD_MS"), 0, 60000);
    long history = 0;
    if (!parseInt(raw("MV_HISTORY_DURATION_NS"), history) || history < 1000000)
    {
        throw ConfigError("MV_HISTORY_DURATION_NS must be at least 1000000");
    }
    cfg.historyNs = static_cast<std::uint64_t>(history);
    cfg.layoutsFile = raw("MV_LAYOUTS_FILE");
    cfg.activeLayout = raw("MV_ACTIVE_LAYOUT");
    if (cfg.activeLayout.empty())
    {
        throw ConfigError("MV_ACTIVE_LAYOUT must not be empty");
    }
    cfg.audioChannels = requireInt("MV_AUDIO_CHANNELS", raw("MV_AUDIO_CHANNELS"), 0, 16);
    if (cfg.audioChannels != 0 && cfg.audioChannels != 2 && cfg.audioChannels != 16)
    {
        throw ConfigError("MV_AUDIO_CHANNELS must be 0, 2, or 16");
    }
    cfg.audioFollow = requireInt("MV_AUDIO_FOLLOW", raw("MV_AUDIO_FOLLOW"), 0, cfg.maxInputs);
    cfg.overlayHz = requireInt("MV_OVERLAY_HZ", raw("MV_OVERLAY_HZ"), 1, 60);
    cfg.previewFps = requireInt("MV_PREVIEW_FPS", raw("MV_PREVIEW_FPS"), 1, 30);
    cfg.previewWidth = requireInt("MV_PREVIEW_WIDTH", raw("MV_PREVIEW_WIDTH"), 160, 1920);
    cfg.grid = requireInt("MV_GRID", raw("MV_GRID"), 1, 96);
    cfg.blackY = requireInt("MV_BLACK_Y", raw("MV_BLACK_Y"), 0, 1023);
    if (!parseDouble(raw("MV_SILENCE_DBFS"), cfg.silenceDbfs) || cfg.silenceDbfs > 0 || cfg.silenceDbfs < -120)
    {
        throw ConfigError("MV_SILENCE_DBFS must be between -120 and 0");
    }
    if (!parseDouble(raw("MV_CLIP_LINEAR"), cfg.clipLinear) || cfg.clipLinear <= 0 || cfg.clipLinear > 1)
    {
        throw ConfigError("MV_CLIP_LINEAR must be in (0,1]");
    }
    cfg.alarmDebounceMs = requireInt("MV_ALARM_DEBOUNCE_MS", raw("MV_ALARM_DEBOUNCE_MS"), 0, 60000);
    cfg.alarmClearMs = requireInt("MV_ALARM_CLEAR_MS", raw("MV_ALARM_CLEAR_MS"), 0, 60000);
    cfg.backgroundFile = raw("MV_BACKGROUND_FILE");
    cfg.configFile = raw("MV_CONFIG_FILE");
    cfg.nmosEnable = requireBool("NMOS_ENABLE", raw("NMOS_ENABLE"));
    cfg.nmosRegistryAddress = raw("NMOS_REGISTRY_ADDRESS");
    cfg.nmosRegistryPort = requireInt("NMOS_REGISTRY_PORT", raw("NMOS_REGISTRY_PORT"), 1, 65535);
    cfg.nmosDnsSd = requireBool("NMOS_DNS_SD", raw("NMOS_DNS_SD"));
    cfg.nmosPort = requireInt("NMOS_PORT", raw("NMOS_PORT"), 1, 65534);
    cfg.nmosSeed = raw("NMOS_SEED");
    if (cfg.nmosSeed.empty())
    {
        cfg.nmosSeed = cfg.hostId + "-multiviewer";
    }
    cfg.webEnable = requireBool("WEB_ENABLE", raw("WEB_ENABLE"));
    cfg.webPort = requireInt("WEB_PORT", raw("WEB_PORT"), 1, 65535);
    cfg.tslEnable = requireBool("TSL_ENABLE", raw("TSL_ENABLE"));
    cfg.tslUdpPort = requireInt("TSL_UDP_PORT", raw("TSL_UDP_PORT"), 1, 65535);
    cfg.tslTcpPort = requireInt("TSL_TCP_PORT", raw("TSL_TCP_PORT"), 1, 65535);
    cfg.tslV31 = requireBool("TSL_V31", raw("TSL_V31"));
    cfg.tslScreen = requireInt("TSL_SCREEN", raw("TSL_SCREEN"), -1, 65535);
    cfg.tslMap = raw("TSL_MAP");
    validateMap(cfg.tslMap, cfg.maxInputs);
    cfg.logLevel = lower(raw("LOG_LEVEL"));
    requireEnum("LOG_LEVEL", cfg.logLevel, {"trace", "debug", "info", "warn", "error"});
    cfg.logFormat = lower(raw("LOG_FORMAT"));
    requireEnum("LOG_FORMAT", cfg.logFormat, {"json", "text"});
    cfg.shutdownTimeoutS = requireInt("SHUTDOWN_TIMEOUT_S", raw("SHUTDOWN_TIMEOUT_S"), 1, 120);
    checkPorts(cfg);

    if (cfg.scanPath.empty() || cfg.outputDomainDir.empty())
    {
        throw ConfigError("MXL paths must not be empty");
    }

    cfg.heads.resize(static_cast<std::size_t>(cfg.outputs));
    for (int h = 0; h < cfg.outputs; ++h)
    {
        auto const prefix = "MV_OUT" + std::to_string(h + 1) + "_";
        HeadConfig head;
        head.format = cfg.outputFormat;
        head.layout = cfg.activeLayout;
        head.audioFollow = cfg.audioFollow;
        head.audioChannels = cfg.audioChannels;
        auto const format = raw((prefix + "FORMAT").c_str());
        if (!format.empty())
        {
            head.format = parseVideoFormat(format);
        }
        else if (h == 0)
        {
            head.format = cfg.outputFormat;
        }
        auto const layout = raw((prefix + "LAYOUT").c_str());
        if (!layout.empty())
        {
            head.layout = layout;
        }
        auto const follow = raw((prefix + "AUDIO_FOLLOW").c_str());
        if (!follow.empty())
        {
            head.audioFollow = requireInt(prefix + "AUDIO_FOLLOW", follow, 0, cfg.maxInputs);
        }
        auto const channels = raw((prefix + "AUDIO_CHANNELS").c_str());
        if (!channels.empty())
        {
            head.audioChannels = requireInt(prefix + "AUDIO_CHANNELS", channels, 0, 16);
            if (head.audioChannels != 0 && head.audioChannels != 2 && head.audioChannels != 16)
            {
                throw ConfigError(prefix + "AUDIO_CHANNELS must be 0, 2, or 16");
            }
        }
        cfg.heads[static_cast<std::size_t>(h)] = head;
    }
    if (!raw("MV_OUT1_FORMAT").empty())
    {
        cfg.outputFormat = cfg.heads[0].format;
    }
    return cfg;
}
} // namespace mv
