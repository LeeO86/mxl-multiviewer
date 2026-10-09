#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "config/config.hpp"

namespace mv
{
enum class SettingSource
{
    Default,
    File,
    Env
};

char const* settingSourceName(SettingSource source);

class ConfigStore
{
public:
    ConfigStore(std::map<std::string, std::string> env, std::optional<std::string> filePath);

    [[nodiscard]] Config effectiveConfig() const;
    [[nodiscard]] SettingSource sourceOf(std::string const& key) const;
    [[nodiscard]] std::optional<std::string> effectiveValue(std::string const& key) const;
    [[nodiscard]] bool hasFileLayer() const;
    [[nodiscard]] std::optional<std::string> filePath() const;
    // Used when MV_CONFIG_FILE is unset. The path is the file layer, not an environment override.
    void ensureFile(std::string path);
    [[nodiscard]] std::string renderEnvBlock() const;
    // The layout the environment pins head `head` to at start (§6.1): MV_OUT<h>_LAYOUT, else
    // MV_ACTIVE_LAYOUT, when set in the environment (not the file).
    [[nodiscard]] std::optional<std::string> pinnedLayout(int head) const;

    struct UpdateResult
    {
        Config config;
        std::vector<std::string> changedKeys;
        std::vector<std::string> restartRequired;
    };

    std::variant<UpdateResult, std::string> update(std::map<std::string, std::optional<std::string>> const& changes);

private:
    void loadFile();
    [[nodiscard]] std::map<std::string, std::string> mergedFile(std::map<std::string, std::optional<std::string>> const& changes) const;

    std::map<std::string, std::string> env_;
    std::optional<std::string> filePath_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> file_;
};
} // namespace mv
