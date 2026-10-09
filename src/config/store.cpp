#include "config/store.hpp"

#include "util/jsonutil.hpp"

#include <cstdio>
#include <fstream>

namespace mv
{
namespace
{
std::map<std::string, std::string> readFlatFile(std::string const& path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw ConfigError("cannot read config file " + path);
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    auto const root = json::parse(body, error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("config file is not a JSON object");
    }
    std::map<std::string, std::string> out;
    for (auto const& [key, value] : root.get<picojson::object>())
    {
        if (!value.is<std::string>())
        {
            throw ConfigError("config value for " + key + " must be a string");
        }
        out.emplace(key, value.get<std::string>());
    }
    return out;
}

void writeFlatFile(std::string const& path, std::map<std::string, std::string> const& values)
{
    picojson::object obj;
    for (auto const& [key, value] : values)
    {
        obj[key] = picojson::value(value);
    }
    auto const tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            throw ConfigError("cannot write config file");
        }
        out << picojson::value(obj).serialize(true);
        if (!out)
        {
            throw ConfigError("cannot write config file");
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
    {
        throw ConfigError("cannot replace config file");
    }
}
} // namespace

char const* settingSourceName(SettingSource source)
{
    switch (source)
    {
    case SettingSource::Env:
        return "environment";
    case SettingSource::File:
        return "file";
    case SettingSource::Default:
        return "default";
    }
    return "default";
}

ConfigStore::ConfigStore(std::map<std::string, std::string> env, std::optional<std::string> filePath)
    : env_(std::move(env))
    , filePath_(std::move(filePath))
{
    if (!filePath_)
    {
        if (auto const it = env_.find("MV_CONFIG_FILE"); it != env_.end() && !it->second.empty())
        {
            filePath_ = it->second;
        }
    }
    if (filePath_)
    {
        std::ifstream probe(*filePath_);
        if (probe)
        {
            loadFile();
        }
    }
}

void ConfigStore::loadFile()
{
    auto raw = readFlatFile(*filePath_);
    file_.clear();
    for (auto const& [key, value] : raw)
    {
        file_[canonicalSetting(key)] = value;
    }
}

void ConfigStore::ensureFile(std::string path)
{
    std::lock_guard lock{mutex_};
    if (filePath_)
    {
        return;
    }
    filePath_ = std::move(path);
    std::ifstream probe(*filePath_);
    if (probe)
    {
        loadFile();
    }
}

Config ConfigStore::effectiveConfig() const
{
    std::lock_guard lock{mutex_};
    return loadConfig(env_, file_);
}

SettingSource ConfigStore::sourceOf(std::string const& key) const
{
    std::lock_guard lock{mutex_};
    auto const name = canonicalSetting(key);
    if (env_.count(name) != 0 || (name == "MV_OUTPUT_DOMAIN_DIR" && env_.count("MXL_OUTPUT_DOMAIN_DIR") != 0) ||
        (name == "MV_OUTPUT_DOMAIN_ID" && env_.count("MXL_OUTPUT_DOMAIN_ID") != 0))
    {
        return SettingSource::Env;
    }
    if (file_.count(name) != 0)
    {
        return SettingSource::File;
    }
    return SettingSource::Default;
}

std::optional<std::string> ConfigStore::effectiveValue(std::string const& key) const
{
    std::lock_guard lock{mutex_};
    auto const name = canonicalSetting(key);
    if (auto const it = env_.find(name); it != env_.end())
    {
        return it->second;
    }
    if (name == "MV_OUTPUT_DOMAIN_DIR")
    {
        if (auto const it = env_.find("MXL_OUTPUT_DOMAIN_DIR"); it != env_.end())
        {
            return it->second;
        }
    }
    if (name == "MV_OUTPUT_DOMAIN_ID")
    {
        if (auto const it = env_.find("MXL_OUTPUT_DOMAIN_ID"); it != env_.end())
        {
            return it->second;
        }
    }
    if (auto const it = file_.find(name); it != file_.end())
    {
        return it->second;
    }
    for (auto const& def : settingSchema())
    {
        if (key == def.name && def.defaultValue != nullptr)
        {
            return std::string(def.defaultValue);
        }
    }
    return std::nullopt;
}

std::optional<std::string> ConfigStore::pinnedLayout(int head) const
{
    for (auto const& key : {"MV_OUT" + std::to_string(head) + "_LAYOUT", std::string("MV_ACTIVE_LAYOUT")})
    {
        auto const value = effectiveValue(key).value_or("");
        if (!value.empty() && sourceOf(key) == SettingSource::Env)
        {
            return value;
        }
    }
    return std::nullopt;
}

bool ConfigStore::hasFileLayer() const
{
    return filePath_.has_value();
}

std::optional<std::string> ConfigStore::filePath() const
{
    return filePath_;
}

std::string ConfigStore::renderEnvBlock() const
{
    auto const cfg = effectiveConfig();
    (void)cfg;
    std::string out;
    for (auto const& def : settingSchema())
    {
        if (std::string(def.name).rfind("MV_OUT", 0) == 0 && effectiveValue(def.name).value_or("").empty() && sourceOf(def.name) == SettingSource::Default)
        {
            continue;
        }
        auto const value = effectiveValue(def.name).value_or("");
        if (std::string(def.name) == "HOST_ID" && sourceOf(def.name) == SettingSource::Default)
        {
            out += std::string(def.name) + "=" + cfg.hostId + "\n";
            continue;
        }
        if (std::string(def.name) == "NMOS_SEED" && sourceOf(def.name) == SettingSource::Default)
        {
            out += std::string(def.name) + "=" + cfg.nmosSeed + "\n";
            continue;
        }
        out += std::string(def.name) + "=" + value + "\n";
    }
    return out;
}

std::map<std::string, std::string> ConfigStore::mergedFile(std::map<std::string, std::optional<std::string>> const& changes) const
{
    auto next = file_;
    for (auto const& [key, value] : changes)
    {
        auto const name = canonicalSetting(key);
        if (!knownSetting(name))
        {
            throw ConfigError("unknown config key " + key);
        }
        if (env_.count(name) != 0 || (name == "MV_OUTPUT_DOMAIN_DIR" && env_.count("MXL_OUTPUT_DOMAIN_DIR") != 0) ||
            (name == "MV_OUTPUT_DOMAIN_ID" && env_.count("MXL_OUTPUT_DOMAIN_ID") != 0))
        {
            throw ConfigError(name + " is set via the environment");
        }
        if (!value)
        {
            next.erase(name);
        }
        else
        {
            next[name] = *value;
        }
    }
    return next;
}

std::variant<ConfigStore::UpdateResult, std::string> ConfigStore::update(std::map<std::string, std::optional<std::string>> const& changes)
{
    std::lock_guard lock{mutex_};
    if (!filePath_)
    {
        return std::string("MV_CONFIG_FILE is not set");
    }
    try
    {
        auto const next = mergedFile(changes);
        auto const cfg = loadConfig(env_, next);
        writeFlatFile(*filePath_, next);
        file_ = next;
        UpdateResult result;
        result.config = cfg;
        for (auto const& [key, value] : changes)
        {
            (void)value;
            auto const name = canonicalSetting(key);
            result.changedKeys.push_back(name);
            for (auto const& def : settingSchema())
            {
                if (name == def.name && def.restart)
                {
                    result.restartRequired.push_back(name);
                }
            }
        }
        return result;
    }
    catch (ConfigError const& ex)
    {
        return std::string(ex.what());
    }
}
} // namespace mv
