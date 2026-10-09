#include "ops/api.hpp"

#include "media/overlay.hpp"
#include "util/logging.hpp"
#include "media/timebase.hpp"
#include "nmos/ids.hpp"
#include "util/jsonutil.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <variant>
#include <vector>

namespace mv
{
namespace
{
std::string quote(std::string const& text)
{
    return "\"" + jsonEscape(text) + "\"";
}

std::string legJson(LegView const& leg)
{
    std::ostringstream out;
    out << "{\"enable\":" << (leg.enable ? "true" : "false") << ",\"state\":" << quote(leg.state) << ",\"reason\":" << quote(leg.reason)
        << ",\"domain_id\":" << (leg.domainId.empty() ? "null" : quote(leg.domainId)) << ",\"flow_id\":" << (leg.flowId.empty() ? "null" : quote(leg.flowId))
        << ",\"sender_id\":" << (leg.senderId.empty() ? "null" : quote(leg.senderId)) << ",\"label\":" << quote(leg.label) << ",\"format\":" << quote(leg.format)
        << ",\"width\":" << leg.width << ",\"height\":" << leg.height << ",\"rate_num\":" << leg.rateNum << ",\"rate_den\":" << leg.rateDen
        << ",\"channels\":" << leg.channels << ",\"interlaced\":" << (leg.interlaced ? "true" : "false") << ",\"grains\":" << leg.grains
        << ",\"late\":" << leg.late << ",\"resyncs\":" << leg.resyncs << ",\"latency_ms\":" << leg.latencyMs << "}";
    return out.str();
}

// Path segments arrive percent-encoded (a layout called "2+8" is "2%2B8").
std::string urlDecode(std::string const& text)
{
    auto const hex = [](char c) {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0)
        {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        }
        else
        {
            out += text[i];
        }
    }
    return out;
}

// The value of `key` in a query string such as "head=2&t=1", or empty.
std::string queryValue(std::string const& query, std::string const& key)
{
    std::size_t pos = 0;
    while (pos <= query.size())
    {
        auto const end = std::min(query.find('&', pos), query.size());
        auto const part = query.substr(pos, end - pos);
        if (part.rfind(key + "=", 0) == 0)
        {
            return urlDecode(part.substr(key.size() + 1));
        }
        pos = end + 1;
    }
    return {};
}

void numbers(std::ostringstream& out, std::array<double, 16> const& values)
{
    for (std::size_t c = 0; c < values.size(); ++c)
    {
        out << (c != 0 ? "," : "") << values[c];
    }
}

HttpResponse jsonResponse(int status, std::string body)
{
    HttpResponse response;
    response.status = status;
    response.contentType = "application/json";
    response.body = std::move(body);
    return response;
}
} // namespace

Api::Api(Config config, ConfigStore& store, LayoutBookStore& layouts, RuntimeModel& runtime, Metrics& metrics)
    : config_(std::move(config))
    , store_(store)
    , layouts_(layouts)
    , runtime_(runtime)
    , metrics_(metrics)
{
}

void Api::setFlowCallback(std::function<void(OutputFlowNote const&)> callback)
{
    onFlow_ = std::move(callback);
}

HttpResponse Api::handle(HttpRequest const& request)
{
    auto const& path = request.path;
    if (!config_.webEnable)
    {
        bool const mutating = request.method == "POST" || request.method == "PUT" || request.method == "PATCH" || request.method == "DELETE";
        if (mutating || path == "/preview.jpg")
        {
            return jsonResponse(404, "{\"error\":\"web ui disabled\"}");
        }
    }
    if (path == "/api/v1/events" && request.method == "GET")
    {
        HttpResponse response;
        response.websocket = true;
        return response;
    }
    if (path == "/preview.jpg" && request.method == "GET")
    {
        HttpResponse response;
        response.status = 200;
        response.contentType = "image/jpeg";
        auto const head = queryValue(request.query, "head");
        response.body = runtime_.preview(head.empty() ? 1 : std::atoi(head.c_str()));
        if (response.body.empty())
        {
            response.status = 204;
            response.contentType = "text/plain";
        }
        return response;
    }
    if (path == "/api/v1/info" && request.method == "GET")
    {
        auto const ids = makeNmosIds(config_.nmosSeed);
        std::ostringstream out;
        out << "{\"version\":\"" << MV_VERSION << "\",\"mxl_revision\":\"" << MV_MXL_REVISION << "\",\"label\":"
            << quote(config_.nmosLabel.empty() ? config_.hostId : config_.nmosLabel) << ",\"backend\":\"" << config_.backend
            << "\",\"cuda_compiled\":" << (runtime_.cudaCompiled() ? "true" : "false") << ",\"cuda_devices\":" << runtime_.cudaDevices()
            << ",\"max_inputs\":" << config_.maxInputs << ",\"outputs\":" << config_.outputs << ",\"grid\":" << config_.grid << ",\"preview_fps\":"
            << config_.previewFps << ",\"hold_ms\":" << config_.holdMs << ",\"node_id\":\"" << ids.node << "\",\"device_id\":\""
            << ids.device << "\",\"domain_id\":\"" << (config_.outputDomainId.empty() ? ids.domain : config_.outputDomainId) << "\",\"overlay_blend2d\":"
            << (overlayUsesBlend2d() ? "true" : "false") << ",\"receivers\":[";
        for (int i = 1; i <= config_.maxInputs; ++i)
        {
            if (i != 1)
            {
                out << ',';
            }
            out << "{\"input\":" << i << ",\"video\":\"" << ids.videoReceiver(i) << "\",\"audio\":\"" << ids.audioReceiver(i) << "\"}";
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }
    if (path == "/api/v1/inputs" && request.method == "GET")
    {
        std::ostringstream out;
        out << "{\"inputs\":[";
        auto const inputs = runtime_.inputs();
        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            if (i != 0)
            {
                out << ',';
            }
            auto const& input = inputs[i];
            out << "{\"index\":" << input.index << ",\"video\":" << legJson(input.video) << ",\"audio\":" << legJson(input.audio) << ",\"tally\":" << input.tally
                << ",\"tsl_text\":" << quote(input.tslText) << ",\"tsl_lh\":" << input.tslLh << ",\"tsl_rh\":" << input.tslRh
                << ",\"tsl_text_tally\":" << input.tslTextTally << ",\"ppm_dbfs\":[";
            numbers(out, input.ppmDbfs);
            out << "],\"hold_dbfs\":[";
            numbers(out, input.holdDbfs);
            out << "],\"rms_dbfs\":[";
            numbers(out, input.rmsDbfs);
            out << "],\"clip\":[";
            for (std::size_t c = 0; c < input.clip.size(); ++c)
            {
                out << (c != 0 ? "," : "") << (input.clip[c] ? "true" : "false");
            }
            out << "],\"alarms\":{\"no_signal\":" << (input.alarmNoSignal ? "true" : "false") << ",\"black\":" << (input.alarmBlack ? "true" : "false")
                << ",\"freeze\":" << (input.alarmFreeze ? "true" : "false") << ",\"silence\":" << (input.alarmSilence ? "true" : "false")
                << ",\"clip\":" << (input.alarmClip ? "true" : "false") << ",\"format_mismatch\":" << (input.alarmFormat ? "true" : "false") << "}}";
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }
    if (path == "/api/v1/outputs" && request.method == "GET")
    {
        std::ostringstream out;
        out << "{\"outputs\":[";
        auto const outputs = runtime_.outputs();
        for (std::size_t i = 0; i < outputs.size(); ++i)
        {
            if (i != 0)
            {
                out << ',';
            }
            auto const& output = outputs[i];
            out << "{\"index\":" << output.index << ",\"format\":" << quote(output.format) << ",\"layout\":" << quote(output.layout)
                << ",\"backend\":" << quote(output.backend) << ",\"video_flow_id\":" << quote(output.videoFlowId) << ",\"audio_flow_id\":" << quote(output.audioFlowId)
                << ",\"domain_id\":" << quote(output.domainId) << ",\"frames\":" << output.frames << ",\"late\":" << output.late << ",\"missed\":" << output.missed
                << ",\"compose_ms\":" << output.composeMs << ",\"audio_follow\":" << output.audioFollow << ",\"audio_channels\":" << output.audioChannels << "}";
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }
    if (path == "/api/v1/layouts" && request.method == "GET")
    {
        return jsonResponse(200, layouts_.json());
    }
    if (path == "/api/v1/presets" && request.method == "GET")
    {
        // Today's built-in presets, for "preset defaults" in the editor.
        LayoutBook presets;
        presets.layouts = builtinPresets(config_.maxInputs);
        return jsonResponse(200, bookToJson(presets));
    }
    if (path.rfind("/api/v1/layouts/", 0) == 0)
    {
        auto name = path.substr(std::string("/api/v1/layouts/").size());
        auto const slash = name.find('/');
        std::string action;
        if (slash != std::string::npos)
        {
            action = name.substr(slash + 1);
            name = name.substr(0, slash);
        }
        name = urlDecode(name);
        if (request.method == "POST" && action == "activate")
        {
            if (!layouts_.activate(name, config_.outputs))
            {
                return jsonResponse(404, "{\"error\":\"layout not found\"}");
            }
            for (int head = 1; head <= config_.outputs; ++head)
            {
                runtime_.setHeadLayout(head, name);
            }
            return jsonResponse(200, "{\"active\":" + quote(name) + "}");
        }
        if (request.method == "PUT" && action.empty())
        {
            Layout layout;
            if (auto const problem = parseLayout(request.body, layout, config_.maxInputs))
            {
                return jsonResponse(400, "{\"error\":" + quote(*problem) + "}");
            }
            layout.name = name;
            if (auto const problem = layouts_.upsert(layout))
            {
                return jsonResponse(400, "{\"error\":" + quote(*problem) + "}");
            }
            return jsonResponse(200, layoutToJson(layout));
        }
        if (request.method == "DELETE" && action.empty())
        {
            for (int head = 1; head <= config_.outputs; ++head)
            {
                if (runtime_.headLayout(head) == name)
                {
                    return jsonResponse(409, "{\"error\":" + quote("the layout is on output " + std::to_string(head)) + "}");
                }
            }
            if (auto const problem = layouts_.erase(name))
            {
                return jsonResponse(409, "{\"error\":" + quote(*problem) + "}");
            }
            return jsonResponse(200, "{\"deleted\":" + quote(name) + "}");
        }
    }
    if (path.rfind("/api/v1/outputs/", 0) == 0 && request.method == "PUT")
    {
        auto const indexText = path.substr(std::string("/api/v1/outputs/").size());
        int const index = std::atoi(indexText.c_str());
        if (index < 1 || index > config_.outputs)
        {
            return jsonResponse(404, "{\"error\":\"head not found\"}");
        }
        std::string error;
        auto const root = json::parse(request.body, error);
        if (!error.empty() || !root.is<picojson::object>())
        {
            return jsonResponse(400, "{\"error\":\"invalid json\"}");
        }
        auto const& obj = root.get<picojson::object>();
        // Layout and audio_follow are checked before either changes.
        auto const layout = json::fieldString(root, "layout");
        if (layout && !layouts_.has(*layout))
        {
            return jsonResponse(404, "{\"error\":\"layout not found\"}");
        }
        std::optional<int> follow;
        if (obj.count("audio_follow") != 0)
        {
            auto const& value = obj.at("audio_follow");
            if (!value.is<double>() || value.get<double>() != std::floor(value.get<double>()) || value.get<double>() < 0 ||
                value.get<double>() > config_.maxInputs)
            {
                return jsonResponse(400, "{\"error\":" + quote("audio_follow must be 0 (off) to " + std::to_string(config_.maxInputs)) + "}");
            }
            follow = static_cast<int>(value.get<double>());
        }
        if (layout)
        {
            runtime_.setHeadLayout(index, *layout);
            layouts_.saveHead(index, *layout);
        }
        if (follow)
        {
            runtime_.setHeadAudio(index, *follow, runtime_.headAudioChannels(index));
        }
        if (auto const formatText = json::fieldString(root, "format"))
        {
            try
            {
                auto const format = parseVideoFormat(*formatText);
                runtime_.setHeadFormat(index, format);
                if (onFlow_)
                {
                    OutputFlowNote note;
                    note.head = index;
                    note.format = format;
                    onFlow_(note);
                }
            }
            catch (ConfigError const& ex)
            {
                return jsonResponse(400, "{\"error\":" + quote(ex.what()) + "}");
            }
        }
        return jsonResponse(200, "{\"ok\":true}");
    }
    if (path == "/api/v1/alarms" && request.method == "GET")
    {
        std::ostringstream out;
        out << "{\"alarms\":[";
        bool first = true;
        for (auto const& input : runtime_.inputs())
        {
            // §6.3: red for no signal, black, freeze, and clip; amber for silence and format.
            auto add = [&](char const* name, bool active, AlarmIndex index, char const* severity) {
                if (!active)
                {
                    return;
                }
                if (!first)
                {
                    out << ',';
                }
                first = false;
                out << "{\"input\":" << input.index << ",\"name\":" << quote(name) << ",\"active\":true,\"severity\":\"" << severity
                    << "\",\"since\":" << input.alarmSinceMs[index] << "}";
            };
            add("no_signal", input.alarmNoSignal, kAlarmNoSignal, "red");
            add("black", input.alarmBlack, kAlarmBlack, "red");
            add("freeze", input.alarmFreeze, kAlarmFreeze, "red");
            add("silence", input.alarmSilence, kAlarmSilence, "amber");
            add("clip", input.alarmClip, kAlarmClip, "red");
            add("format_mismatch", input.alarmFormat, kAlarmFormat, "amber");
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }
    if (path == "/api/v1/config" && request.method == "GET")
    {
        std::ostringstream out;
        out << "{\"settings\":[";
        bool first = true;
        for (auto const& def : settingSchema())
        {
            if (!first)
            {
                out << ',';
            }
            first = false;
            auto const value = store_.effectiveValue(def.name).value_or("");
            out << "{\"key\":" << quote(def.name) << ",\"value\":" << quote(value) << ",\"source\":" << quote(settingSourceName(store_.sourceOf(def.name)))
                << ",\"restart\":" << (def.restart ? "true" : "false") << ",\"editable\":" << (store_.sourceOf(def.name) == SettingSource::Env ? "false" : "true")
                << "}";
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }
    if (path == "/api/v1/config" && request.method == "PUT")
    {
        std::string error;
        auto const root = json::parse(request.body, error);
        if (!error.empty() || !root.is<picojson::object>())
        {
            return jsonResponse(400, "{\"error\":\"invalid json\"}");
        }
        std::map<std::string, std::optional<std::string>> changes;
        for (auto const& [key, value] : root.get<picojson::object>())
        {
            if (value.is<picojson::null>())
            {
                changes[key] = std::nullopt;
            }
            else if (value.is<std::string>())
            {
                changes[key] = value.get<std::string>();
            }
            else
            {
                return jsonResponse(400, "{\"error\":\"values must be strings\"}");
            }
        }
        auto const updated = store_.update(changes);
        if (std::holds_alternative<std::string>(updated))
        {
            return jsonResponse(400, "{\"error\":" + quote(std::get<std::string>(updated)) + "}");
        }
        auto const& result = std::get<ConfigStore::UpdateResult>(updated);
        std::ostringstream out;
        out << "{\"restart_required\":" << (result.restartRequired.empty() ? "false" : "true") << ",\"keys\":[";
        for (std::size_t i = 0; i < result.restartRequired.size(); ++i)
        {
            if (i != 0)
            {
                out << ',';
            }
            out << quote(result.restartRequired[i]);
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }
    if (path == "/api/v1/config/export" && request.method == "GET")
    {
        std::ostringstream out;
        out << "{\"version\":1,\"secrets\":false,\"settings\":{";
        bool first = true;
        for (auto const& def : settingSchema())
        {
            if (std::string(def.name).rfind("MV_OUT", 0) == 0 && store_.sourceOf(def.name) == SettingSource::Default)
            {
                continue;
            }
            if (!first)
            {
                out << ',';
            }
            first = false;
            auto value = store_.effectiveValue(def.name).value_or("");
            if (std::string(def.name) == "HOST_ID" && store_.sourceOf(def.name) == SettingSource::Default)
            {
                value = config_.hostId;
            }
            if (std::string(def.name) == "NMOS_SEED" && store_.sourceOf(def.name) == SettingSource::Default)
            {
                value = config_.nmosSeed;
            }
            if (std::string(def.name) == "NMOS_HOST_ADDRESS" && value.empty())
            {
                value = config_.nmosHostAddress;
            }
            out << quote(def.name) << ':' << quote(value);
        }
        out << "},\"layouts\":" << layouts_.json() << ",\"routes\":";
        std::string routes = "{\"routes\":[]}";
        {
            std::ifstream in(std::filesystem::path(config_.stateDir) / "routes.json");
            if (in)
            {
                std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::string parseError;
                auto const parsed = json::parse(body, parseError);
                if (parseError.empty() && parsed.is<picojson::object>())
                {
                    routes = parsed.serialize();
                }
            }
        }
        out << routes << "}";
        return jsonResponse(200, out.str());
    }
    if (path == "/api/v1/config/import" && request.method == "POST")
    {
        std::string error;
        auto const root = json::parse(request.body, error);
        if (!error.empty() || !root.is<picojson::object>())
        {
            return jsonResponse(400, "{\"error\":\"import body must be a JSON object\"}");
        }
        auto const& obj = root.get<picojson::object>();
        std::vector<std::string> skipped;
        std::vector<int> moved;
        if (auto const settings = obj.find("settings"); settings != obj.end())
        {
            if (!settings->second.is<picojson::object>())
            {
                return jsonResponse(400, "{\"error\":\"settings must be an object of strings\"}");
            }
            std::map<std::string, std::optional<std::string>> changes;
            for (auto const& [key, value] : settings->second.get<picojson::object>())
            {
                if (!value.is<std::string>())
                {
                    return jsonResponse(400, "{\"error\":\"settings values must be strings\"}");
                }
                // An export carries every setting; the environment wins over the file anyway,
                // so keys set there are skipped instead of failing the whole import.
                if (store_.sourceOf(key) == SettingSource::Env)
                {
                    skipped.push_back(key);
                    continue;
                }
                changes.emplace(key, value.get<std::string>());
            }
            auto const updated = store_.update(changes);
            if (std::holds_alternative<std::string>(updated))
            {
                return jsonResponse(400, "{\"error\":" + quote(std::get<std::string>(updated)) + "}");
            }
        }
        if (auto const layouts = obj.find("layouts"); layouts != obj.end())
        {
            if (auto const problem = layouts_.replaceJson(layouts->second.serialize()))
            {
                return jsonResponse(400, "{\"error\":" + quote(*problem) + "}");
            }
            // A head takes the imported book's saved layout for it; a head whose layout the
            // import dropped shows the book's active layout. Either is saved for the next start.
            for (int head = 1; head <= config_.outputs; ++head)
            {
                if (auto const saved = layouts_.savedHead(head))
                {
                    runtime_.setHeadLayout(head, *saved);
                }
                else if (!layouts_.has(runtime_.headLayout(head)))
                {
                    runtime_.setHeadLayout(head, layouts_.activeName());
                    layouts_.saveHead(head, layouts_.activeName());
                    moved.push_back(head);
                }
            }
        }
        bool routesRestart = false;
        if (auto const routes = obj.find("routes"); routes != obj.end())
        {
            if (!routes->second.is<picojson::object>())
            {
                return jsonResponse(400, "{\"error\":\"routes must be an object\"}");
            }
            std::error_code ec;
            std::filesystem::create_directories(config_.stateDir, ec);
            auto const path = std::filesystem::path(config_.stateDir) / "routes.json";
            auto const tmp = path.string() + ".tmp";
            std::ofstream out(tmp, std::ios::trunc);
            if (!out)
            {
                return jsonResponse(500, "{\"error\":\"cannot write routes\"}");
            }
            out << routes->second.serialize();
            out.close();
            std::filesystem::rename(tmp, path, ec);
            if (ec)
            {
                return jsonResponse(500, "{\"error\":\"cannot write routes\"}");
            }
            routesRestart = true;
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"secrets\":false,\"routes_restart\":" << (routesRestart ? "true" : "false") << ",\"skipped\":[";
        for (std::size_t i = 0; i < skipped.size(); ++i)
        {
            out << (i != 0 ? "," : "") << quote(skipped[i]);
        }
        out << "],\"heads_moved\":[";
        for (std::size_t i = 0; i < moved.size(); ++i)
        {
            out << (i != 0 ? "," : "") << moved[i];
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }
    if (path == "/api/v1/config/env" && request.method == "GET")
    {
        HttpResponse response;
        response.contentType = "text/plain";
        response.body = store_.renderEnvBlock();
        return response;
    }
    if (path == "/livez")
    {
        auto const age = taiNowNs() - runtime_.heartbeatNs();
        if (runtime_.heartbeatNs() != 0 && age < 5000000000ull)
        {
            return jsonResponse(200, "{\"status\":\"ok\"}");
        }
        return jsonResponse(503, "{\"status\":\"stale\"}");
    }
    if (path == "/readyz")
    {
        auto const age = taiNowNs() - runtime_.heartbeatNs();
        bool const beating = runtime_.heartbeatNs() != 0 && age < 5000000000ull;
        bool const nmosOk = !config_.nmosEnable || config_.nmosRegistryAddress.empty() || runtime_.nmosUp();
        if (beating && nmosOk)
        {
            return jsonResponse(200, "{\"status\":\"ready\"}");
        }
        return jsonResponse(503, std::string("{\"beating\":") + (beating ? "true" : "false") + ",\"nmos\":" + (nmosOk ? "true" : "false") + "}");
    }
    if (path == "/statusz")
    {
        return handle(HttpRequest{"GET", "/api/v1/inputs", {}, {}, {}});
    }
    if (path == "/metrics")
    {
        HttpResponse response;
        response.contentType = "text/plain; version=0.0.4";
        response.body = metrics_.render();
        return response;
    }
    return jsonResponse(404, "{\"error\":\"not found\"}");
}

std::string Api::eventsJson() const
{
    auto* self = const_cast<Api*>(this);
    auto const inputs = self->handle(HttpRequest{"GET", "/api/v1/inputs", {}, {}, {}});
    auto const outputs = self->handle(HttpRequest{"GET", "/api/v1/outputs", {}, {}, {}});
    auto const alarms = self->handle(HttpRequest{"GET", "/api/v1/alarms", {}, {}, {}});
    auto strip = [](std::string const& body) {
        if (body.size() >= 2 && body.front() == '{' && body.back() == '}')
        {
            return body.substr(1, body.size() - 2);
        }
        return body;
    };
    return "{" + strip(inputs.body) + "," + strip(outputs.body) + "," + strip(alarms.body) + "}";
}
} // namespace mv
