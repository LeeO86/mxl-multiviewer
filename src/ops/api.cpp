#include "ops/api.hpp"

#include "media/overlay.hpp"
#include "util/logging.hpp"
#include "media/timebase.hpp"
#include "nmos/ids.hpp"
#include "util/jsonutil.hpp"
#include "version.hpp"

#include <cstdlib>
#include <sstream>

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
        response.body = runtime_.preview();
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
        out << "{\"version\":\"" << MV_VERSION << "\",\"mxl_revision\":\"" << MV_MXL_REVISION << "\",\"backend\":\"" << config_.backend
            << "\",\"cuda_compiled\":" << (runtime_.cudaCompiled() ? "true" : "false") << ",\"cuda_devices\":" << runtime_.cudaDevices()
            << ",\"max_inputs\":" << config_.maxInputs << ",\"outputs\":" << config_.outputs << ",\"node_id\":\"" << ids.node << "\",\"device_id\":\""
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
                << ",\"tsl_text\":" << quote(input.tslText) << ",\"ppm_dbfs\":[";
            for (int c = 0; c < 16; ++c)
            {
                if (c != 0)
                {
                    out << ',';
                }
                out << input.ppmDbfs[static_cast<std::size_t>(c)];
            }
            out << "],\"rms_dbfs\":[";
            for (int c = 0; c < 16; ++c)
            {
                if (c != 0)
                {
                    out << ',';
                }
                out << input.rmsDbfs[static_cast<std::size_t>(c)];
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
        if (request.method == "POST" && action == "activate")
        {
            if (!layouts_.activate(name))
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
        if (auto const layout = json::fieldString(root, "layout"))
        {
            if (layouts_.layout(*layout) == nullptr)
            {
                return jsonResponse(404, "{\"error\":\"layout not found\"}");
            }
            runtime_.setHeadLayout(index, *layout);
        }
        if (obj.count("audio_follow") != 0 && obj.at("audio_follow").is<double>())
        {
            runtime_.setHeadAudio(index, static_cast<int>(obj.at("audio_follow").get<double>()), runtime_.headAudioChannels(index));
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
            auto add = [&](char const* name, bool active) {
                if (!active)
                {
                    return;
                }
                if (!first)
                {
                    out << ',';
                }
                first = false;
                out << "{\"input\":" << input.index << ",\"name\":" << quote(name) << ",\"active\":true}";
            };
            add("no_signal", input.alarmNoSignal);
            add("black", input.alarmBlack);
            add("freeze", input.alarmFreeze);
            add("silence", input.alarmSilence);
            add("clip", input.alarmClip);
            add("format_mismatch", input.alarmFormat);
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
