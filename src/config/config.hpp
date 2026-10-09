#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mv
{
class ConfigError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct VideoFormat
{
    int width = 1920;
    int height = 1080;
    int rateNum = 50;
    int rateDen = 1;

    [[nodiscard]] std::string token() const;
    [[nodiscard]] bool operator==(VideoFormat const& other) const
    {
        return width == other.width && height == other.height && rateNum == other.rateNum && rateDen == other.rateDen;
    }
};

struct HeadConfig
{
    VideoFormat format;
    std::string layout = "2x2";
    // True when MV_OUT<h>_LAYOUT names this head's layout (not just MV_ACTIVE_LAYOUT).
    bool layoutSet = false;
    int audioFollow = 1;
    int audioChannels = 2;
};

struct Config
{
    std::string hostId;
    std::string scanPath = "/Volumes/mxl";
    std::string outputDomainDir = "/Volumes/mxl/multiviewer";
    std::string outputDomainId;
    std::string stateDir = "/config";
    bool cleanupOnExit = false;
    std::string backend = "auto";
    int maxInputs = 16;
    int outputs = 1;
    VideoFormat outputFormat;
    int inputOffsetGrains = 2;
    int holdMs = 1000;
    std::uint64_t historyNs = 200000000;
    std::string layoutsFile;
    std::string activeLayout = "2x2";
    int audioChannels = 2;
    int audioFollow = 1;
    int overlayHz = 25;
    int previewFps = 5;
    int previewWidth = 480;
    int grid = 24;
    int blackY = 32;
    double silenceDbfs = -60;
    double clipLinear = 0.999;
    int alarmDebounceMs = 500;
    int alarmClearMs = 500;
    // A picture unchanged for this long is frozen (§6.3).
    int freezeMs = 2000;
    // IANA zone of clock tiles with clock_zone local; empty keeps TZ (§6.2).
    std::string timezone;
    std::string backgroundFile;
    std::string configFile;
    bool nmosEnable = true;
    std::string nmosRegistryAddress;
    int nmosRegistryPort = 3210;
    std::string nmosQueryAddress;
    int nmosQueryPort = 3211;
    bool nmosDnsSd = false;
    int nmosPort = 3262;
    std::string nmosSeed;
    std::string nmosLabel;
    std::string nmosHostAddress;
    std::map<std::string, std::vector<std::string>> nmosTags;
    bool webEnable = true;
    int webPort = 8110;
    bool tslEnable = true;
    int tslUdpPort = 8910;
    int tslTcpPort = 8911;
    bool tslV31 = false;
    int tslScreen = -1;
    std::string tslMap;
    std::string logLevel = "info";
    std::string logFormat = "json";
    int shutdownTimeoutS = 10;
    std::vector<HeadConfig> heads;
};

struct SettingDef
{
    char const* name;
    char const* defaultValue;
    bool restart = false;
};

std::vector<SettingDef> const& settingSchema();
// Maps platform aliases onto the canonical key. Unknown names are returned unchanged.
std::string canonicalSetting(std::string const& key);
bool knownSetting(std::string const& key);

VideoFormat parseVideoFormat(std::string const& token);
std::string formatLabel(int width, int height, int rateNum, int rateDen, bool interlaced);

// env overrides file. Unknown keys in `file` throw. Environment variables that are not
// in the schema are ignored, so CI pins such as NMOS_CPP_REF do not fail startup.
Config loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file);

std::string hostnameString();

// True when `name` is an IANA zone (Europe/Zurich, UTC) in the zone database (TZDIR, else
// /usr/share/zoneinfo).
bool knownTimeZone(std::string const& name);
} // namespace mv
