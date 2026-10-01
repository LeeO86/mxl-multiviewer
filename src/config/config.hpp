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
    int audioFollow = 1;
    int audioChannels = 2;
};

struct Config
{
    std::string hostId;
    std::string scanPath = "/Volumes/mxl";
    std::string outputDomainDir = "/Volumes/mxl/multiviewer";
    std::string outputDomainId;
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
    std::string backgroundFile;
    std::string configFile;
    bool nmosEnable = true;
    std::string nmosRegistryAddress;
    int nmosRegistryPort = 3210;
    bool nmosDnsSd = false;
    int nmosPort = 3262;
    std::string nmosSeed;
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
bool knownSetting(std::string const& key);
bool claimedSetting(std::string const& key);

VideoFormat parseVideoFormat(std::string const& token);
std::string formatLabel(int width, int height, int rateNum, int rateDen, bool interlaced);

// env overrides file. Unknown keys in `file` throw. Unknown claimed keys in `env` throw.
// Unrelated environment variables must not be passed in `env` or they are ignored only when
// claimedSetting() is false — pass the full environment; unrelated keys are skipped.
Config loadConfig(std::map<std::string, std::string> const& env, std::map<std::string, std::string> const& file);

std::string hostnameString();
} // namespace mv
