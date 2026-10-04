#include "nmos/node.hpp"

#include "nmos/ids.hpp"
#include "util/httpclient.hpp"
#include "util/logging.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(MV_WITH_NMOS)
#include "cpprest/host_utils.h"
#include "nmos/capabilities.h"
#include "nmos/channels.h"
#include "nmos/clock_name.h"
#include "nmos/colorspace.h"
#include "nmos/connection_api.h"
#include "nmos/connection_resources.h"
#include "nmos/format.h"
#include "nmos/group_hint.h"
#include "nmos/interlace_mode.h"
#include "nmos/log_gate.h"
#include "nmos/media_type.h"
#include "nmos/model.h"
#include "nmos/mxl.h"
#include "nmos/node_interfaces.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/resources.h"
#include "nmos/server.h"
#include "nmos/settings.h"
#include "nmos/transfer_characteristic.h"
#include "nmos/transport.h"
#include "nmos/slog.h"
#include "sdp/json.h"
#endif

namespace mv
{
struct NmosNode::Impl
{
    Config config;
    RouteFn route;
    NmosIds ids;
    std::atomic<bool> running{false};
    std::mutex readyMu;
    std::condition_variable readyCv;
    bool ready = false;
    std::string error;
    std::mutex pendingMu;
    struct SubUpdate
    {
        std::string id;
        std::string sender;
        bool active = false;
    };
    std::vector<SubUpdate> pending;
#if defined(MV_WITH_NMOS)
    std::thread thread;
    nmos::node_model* model = nullptr;
#endif

    explicit Impl(Config cfg, RouteFn routeIn)
        : config(std::move(cfg))
        , route(std::move(routeIn))
        , ids(makeNmosIds(config.nmosSeed))
    {
    }
};

NmosNode::NmosNode(Config config, RouteFn route)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(route)))
{
}

NmosNode::~NmosNode()
{
    stop();
}

bool NmosNode::registered() const
{
    if (!impl_->config.nmosEnable)
    {
        return true;
    }
    if (impl_->config.nmosRegistryAddress.empty())
    {
        return impl_->running.load();
    }
    auto const url = "http://" + impl_->config.nmosQueryAddress + ":" + std::to_string(impl_->config.nmosQueryPort) + "/x-nmos/query/v1.3/nodes/" +
                     impl_->ids.node;
    return httpGet(url, 700).status == 200;
}

std::string NmosNode::summary() const
{
    return std::string("{\"node_id\":\"") + impl_->ids.node + "\",\"registered\":" + (registered() ? "true" : "false") + "}";
}

void NmosNode::updateOutputFlow(int head, std::string const& videoFlowId, std::string const& audioFlowId, VideoFormat const& format)
{
    (void)head;
    (void)videoFlowId;
    (void)audioFlowId;
    (void)format;
#if defined(MV_WITH_NMOS)
    if (impl_->model == nullptr)
    {
        return;
    }
    auto lock = impl_->model->write_lock();
    auto const videoSender = impl_->ids.videoSender(head);
    auto const videoFlow = impl_->ids.videoFlow(head, format.token());
    (void)videoFlow;
    nmos::modify_resource(impl_->model->connection_resources, utility::conversions::to_string_t(videoSender), [&](nmos::resource& connection) {
        connection.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = web::json::value::string(utility::conversions::to_string_t(videoFlowId));
        connection.data[U("staged")][U("transport_params")][0][U("mxl_flow_id")] = web::json::value::string(utility::conversions::to_string_t(videoFlowId));
    });
    if (!audioFlowId.empty())
    {
        auto const audioSender = impl_->ids.audioSender(head);
        nmos::modify_resource(impl_->model->connection_resources, utility::conversions::to_string_t(audioSender), [&](nmos::resource& connection) {
            connection.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = web::json::value::string(utility::conversions::to_string_t(audioFlowId));
        });
    }
    impl_->model->notify();
#endif
}

void NmosNode::restoreRoute(int input, bool video, bool enable, std::string const& domainId, std::string const& flowId, std::string const& senderId)
{
    (void)enable;
    (void)domainId;
    (void)flowId;
    (void)senderId;
#if defined(MV_WITH_NMOS)
    if (impl_->model == nullptr)
    {
        return;
    }
    auto const id = video ? impl_->ids.videoReceiver(input) : impl_->ids.audioReceiver(input);
    auto const text = [](std::string const& value) {
        return value.empty() ? web::json::value::null() : web::json::value::string(utility::conversions::to_string_t(value));
    };
    {
        auto lock = impl_->model->write_lock();
        nmos::modify_resource(impl_->model->connection_resources, utility::conversions::to_string_t(id), [&](nmos::resource& connection) {
            for (auto const* key : {U("active"), U("staged")})
            {
                auto& document = connection.data[key];
                document[U("master_enable")] = web::json::value::boolean(enable);
                document[U("sender_id")] = text(senderId);
                document[U("transport_params")][0][U("mxl_domain_id")] = text(domainId);
                document[U("transport_params")][0][U("mxl_flow_id")] = text(flowId);
            }
        });
        impl_->model->notify();
    }
    std::lock_guard lock{impl_->pendingMu};
    impl_->pending.push_back(Impl::SubUpdate{id, senderId, enable && !senderId.empty()});
#else
    (void)input;
    (void)video;
#endif
}

#if !defined(MV_WITH_NMOS)
void NmosNode::start()
{
    if (impl_->config.nmosEnable)
    {
        throw std::runtime_error("built without nmos-cpp");
    }
}

void NmosNode::stop() {}
#else
namespace
{
utility::string_t us(std::string const& text)
{
    return utility::conversions::to_string_t(text);
}

std::string su(utility::string_t const& text)
{
    return utility::conversions::to_utf8string(text);
}

void tagGroup(nmos::resource& resource, std::string const& group, std::string const& role)
{
    if (!resource.data.has_field(U("tags")))
    {
        resource.data[U("tags")] = web::json::value::object();
    }
    web::json::push_back(resource.data[U("tags")][U("urn:x-nmos:tag:grouphint/v1.0")], nmos::make_group_hint({us(group), us(role)}));
}

bool uuidOk(std::string const& text)
{
    return text.size() == 36 && text[8] == '-' && text[13] == '-';
}
} // namespace

void NmosNode::start()
{
    if (!impl_->config.nmosEnable)
    {
        return;
    }
    impl_->thread = std::thread([this] {
        nmos::experimental::log_model logModel;
        std::ostream errorLog(std::cerr.rdbuf());
        std::filebuf discarded;
        std::ostream accessLog(&discarded);
        nmos::experimental::log_gate gate(errorLog, accessLog, logModel);
        try
        {
            nmos::node_model nodeModel;
            impl_->model = &nodeModel;
            web::json::value settings = web::json::value::object();
            auto const nodeLabel = impl_->config.nmosLabel.empty() ? impl_->config.hostId : impl_->config.nmosLabel;
            settings[U("http_port")] = impl_->config.nmosPort;
            settings[U("label")] = web::json::value::string(us(nodeLabel));
            settings[U("description")] = web::json::value::string(U("mxl-multiviewer"));
            settings[U("seed_id")] = web::json::value::string(us(impl_->ids.node));
            settings[U("service_name_prefix")] = web::json::value::string(U("mxl-multiviewer"));
            settings[U("logging_level")] = 20;
            settings[U("control_protocol_ws_port")] = -1;
            if (!impl_->config.nmosHostAddress.empty())
            {
                settings[U("host_address")] = web::json::value::string(us(impl_->config.nmosHostAddress));
                settings[U("host_addresses")] = web::json::value::array({web::json::value::string(us(impl_->config.nmosHostAddress))});
            }
            if (!impl_->config.nmosDnsSd)
            {
                settings[U("pri")] = std::numeric_limits<int>::max();
                settings[U("highest_pri")] = std::numeric_limits<int>::max();
            }
            if (!impl_->config.nmosRegistryAddress.empty())
            {
                settings[U("registry_address")] = web::json::value::string(us(impl_->config.nmosRegistryAddress));
                settings[U("registration_port")] = impl_->config.nmosRegistryPort;
                if (!impl_->config.nmosQueryAddress.empty())
                {
                    settings[U("query_address")] = web::json::value::string(us(impl_->config.nmosQueryAddress));
                }
                settings[U("query_port")] = impl_->config.nmosQueryPort;
            }
            nodeModel.settings = settings;
            nmos::insert_node_default_settings(nodeModel.settings);
            logModel.settings = nodeModel.settings;
            logModel.level = nmos::fields::logging_level(logModel.settings);

            auto implementation =
                nmos::experimental::node_implementation()
                    .on_parse_transport_file([](nmos::resource const&, nmos::resource const&, utility::string_t const&, utility::string_t const&,
                                                 slog::base_gate&) -> web::json::value { throw std::runtime_error("MXL does not use a transport file"); })
                    .on_resolve_auto([](nmos::resource const&, nmos::resource const&, web::json::value& params) {
                        if (!params.is_array() || params.size() == 0)
                        {
                            return;
                        }
                        auto& leg = params.at(0);
                        nmos::details::resolve_auto(leg, U("mxl_domain_id"), [] { return web::json::value::string(U("00000000-0000-0000-0000-000000000000")); });
                        nmos::details::resolve_auto(leg, U("mxl_flow_id"), [] { return web::json::value::null(); });
                    })
                    .on_set_transportfile([](nmos::resource const&, nmos::resource const&, web::json::value& transportFile) { transportFile = web::json::value::null(); })
                    .on_connection_activated([this](nmos::resource const&, nmos::resource const& connection) {
                        auto const id = su(connection.id);
                        int input = 0;
                        bool video = true;
                        bool matched = false;
                        for (int i = 1; i <= impl_->config.maxInputs; ++i)
                        {
                            if (id == impl_->ids.videoReceiver(i))
                            {
                                input = i;
                                video = true;
                                matched = true;
                            }
                            else if (id == impl_->ids.audioReceiver(i))
                            {
                                input = i;
                                video = false;
                                matched = true;
                            }
                        }
                        if (!matched || !connection.data.has_field(U("active")))
                        {
                            return;
                        }
                        auto const& active = connection.data.at(U("active"));
                        bool enable = active.has_field(U("master_enable")) && active.at(U("master_enable")).as_bool();
                        std::string domain;
                        std::string flow;
                        std::string sender;
                        if (active.has_field(U("sender_id")) && active.at(U("sender_id")).is_string())
                        {
                            sender = su(active.at(U("sender_id")).as_string());
                        }
                        if (active.has_field(U("transport_params")) && active.at(U("transport_params")).is_array() && active.at(U("transport_params")).size() > 0)
                        {
                            auto const& params = active.at(U("transport_params")).at(0);
                            if (params.has_field(U("mxl_domain_id")) && params.at(U("mxl_domain_id")).is_string())
                            {
                                domain = su(params.at(U("mxl_domain_id")).as_string());
                            }
                            if (params.has_field(U("mxl_flow_id")) && params.at(U("mxl_flow_id")).is_string())
                            {
                                flow = su(params.at(U("mxl_flow_id")).as_string());
                            }
                        }
                        if ((!domain.empty() && !uuidOk(domain)) || (!flow.empty() && !uuidOk(flow)))
                        {
                            throw std::runtime_error("mxl_domain_id and mxl_flow_id must be UUIDs");
                        }
                        if (domain == "00000000-0000-0000-0000-000000000000")
                        {
                            domain.clear();
                        }
                        if (impl_->route)
                        {
                            impl_->route(input, video, enable, domain, flow, sender);
                        }
                        {
                            std::lock_guard lock{impl_->pendingMu};
                            impl_->pending.push_back(Impl::SubUpdate{id, sender, enable && !sender.empty()});
                        }
                        logInfo("nmos_activation", {{"input", std::to_string(input)}, {"kind", video ? "video" : "audio"}, {"flow", flow}});
                    });

            auto server = nmos::experimental::make_node_server(nodeModel, implementation, logModel, gate);
            server.thread_functions.push_back([this, &nodeModel, nodeLabel] {
                try
                {
                    auto lock = nodeModel.write_lock();
                    using web::json::value;
                    auto const clocks = web::json::value_of({nmos::make_internal_clock(nmos::clock_names::clk0)});
                    auto const interfaces = nmos::experimental::node_interfaces(nmos::get_host_interfaces(nodeModel.settings));
                    auto node = nmos::make_node(us(impl_->ids.node), clocks, nmos::make_node_interfaces(interfaces), nodeModel.settings);
                    node.data[U("label")] = value::string(us(nodeLabel));
                    node.data[U("description")] = value::string(U("MXL multiviewer"));
                    if (!node.data.has_field(U("tags")))
                    {
                        node.data[U("tags")] = value::object();
                    }
                    for (auto const& [name, values] : impl_->config.nmosTags)
                    {
                        web::json::value list = value::array();
                        for (auto const& item : values)
                        {
                            web::json::push_back(list, value::string(us(item)));
                        }
                        node.data[U("tags")][us(name)] = std::move(list);
                    }
                    nmos::insert_resource(nodeModel.node_resources, std::move(node));

                    std::vector<nmos::id> senders;
                    std::vector<nmos::id> receivers;
                    for (int head = 1; head <= impl_->config.outputs; ++head)
                    {
                        senders.push_back(us(impl_->ids.videoSender(head)));
                        if (impl_->config.heads[static_cast<std::size_t>(head - 1)].audioChannels > 0)
                        {
                            senders.push_back(us(impl_->ids.audioSender(head)));
                        }
                    }
                    for (int i = 1; i <= impl_->config.maxInputs; ++i)
                    {
                        receivers.push_back(us(impl_->ids.videoReceiver(i)));
                        receivers.push_back(us(impl_->ids.audioReceiver(i)));
                    }
                    auto device = nmos::make_device(us(impl_->ids.device), us(impl_->ids.node), senders, receivers, nodeModel.settings);
                    device.data[U("label")] = value::string(us(impl_->config.nmosLabel.empty() ? std::string("MXL Multiviewer") : impl_->config.nmosLabel + " multiviewer"));
                    if (!device.data.has_field(U("tags")))
                    {
                        device.data[U("tags")] = value::object();
                    }
                    for (auto const& [name, values] : impl_->config.nmosTags)
                    {
                        web::json::value list = value::array();
                        for (auto const& item : values)
                        {
                            web::json::push_back(list, value::string(us(item)));
                        }
                        device.data[U("tags")][us(name)] = std::move(list);
                    }
                    nmos::insert_resource(nodeModel.node_resources, std::move(device));

                    auto const domain = us(impl_->config.outputDomainId.empty() ? impl_->ids.domain : impl_->config.outputDomainId);
                    for (int head = 1; head <= impl_->config.outputs; ++head)
                    {
                        auto const& headCfg = impl_->config.heads[static_cast<std::size_t>(head - 1)];
                        auto const group = "MV Out " + std::to_string(head);
                        nmos::rational const rate{headCfg.format.rateNum, headCfg.format.rateDen};
                        auto const flowId = impl_->ids.videoFlow(head, headCfg.format.token());
                        auto source = nmos::make_video_source(us(impl_->ids.videoSource(head)), us(impl_->ids.device), nmos::clock_names::clk0, rate, nodeModel.settings);
                        auto flow = nmos::make_coded_video_flow(us(flowId), us(impl_->ids.videoSource(head)), us(impl_->ids.device), rate,
                            static_cast<unsigned>(headCfg.format.width), static_cast<unsigned>(headCfg.format.height), nmos::interlace_modes::progressive,
                            nmos::colorspaces::BT709, nmos::transfer_characteristics::SDR, sdp::samplings::YCbCr_4_2_2, 10, nmos::media_types::video_v210,
                            nodeModel.settings);
                        source.data[U("label")] = value::string(us(group + " Video"));
                        flow.data[U("label")] = value::string(us(group + " Video"));
                        tagGroup(source, group, "Video");
                        tagGroup(flow, group, "Video");
                        nmos::insert_resource(nodeModel.node_resources, std::move(source));
                        nmos::insert_resource(nodeModel.node_resources, std::move(flow));
                        auto sender = nmos::make_sender(us(impl_->ids.videoSender(head)), us(flowId), nmos::transports::mxl, us(impl_->ids.device), {}, {}, nodeModel.settings);
                        sender.data[U("label")] = value::string(us("MV Out " + std::to_string(head) + " Video"));
                        sender.data[U("subscription")][U("active")] = value::boolean(true);
                        tagGroup(sender, group, "Video");
                        nmos::insert_resource(nodeModel.node_resources, std::move(sender));
                        auto connection = nmos::make_connection_mxl_sender(us(impl_->ids.videoSender(head)), domain, us(flowId));
                        connection.data[U("active")][U("master_enable")] = value::boolean(true);
                        connection.data[U("staged")][U("master_enable")] = value::boolean(true);
                        connection.data[U("active")][U("transport_params")][0][U("mxl_domain_id")] = value::string(domain);
                        connection.data[U("staged")][U("transport_params")][0][U("mxl_domain_id")] = value::string(domain);
                        connection.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = value::string(us(flowId));
                        connection.data[U("staged")][U("transport_params")][0][U("mxl_flow_id")] = value::string(us(flowId));
                        nmos::insert_resource(nodeModel.connection_resources, std::move(connection));
                        if (headCfg.audioChannels > 0)
                        {
                            std::vector<nmos::channel> channels;
                            for (int c = 1; c <= headCfg.audioChannels; ++c)
                            {
                                channels.push_back(nmos::channel{us("Ch" + std::to_string(c)), nmos::channel_symbols::Undefined(static_cast<unsigned>(c))});
                            }
                            auto audioSource = nmos::make_audio_source(us(impl_->ids.audioSource(head)), us(impl_->ids.device), nmos::clock_names::clk0,
                                nmos::rational{48000, 1}, channels, nodeModel.settings);
                            auto const audioId = impl_->ids.audioFlow(head, headCfg.audioChannels);
                            auto audioFlow = nmos::make_raw_audio_flow(us(audioId), us(impl_->ids.audioSource(head)), us(impl_->ids.device), nmos::rational{48000, 1},
                                nmos::media_types::audio_float32, 32, nodeModel.settings);
                            audioFlow.data[U("channel_count")] = headCfg.audioChannels;
                            tagGroup(audioSource, group, "Audio");
                            tagGroup(audioFlow, group, "Audio");
                            nmos::insert_resource(nodeModel.node_resources, std::move(audioSource));
                            nmos::insert_resource(nodeModel.node_resources, std::move(audioFlow));
                            auto audioSender = nmos::make_sender(us(impl_->ids.audioSender(head)), us(audioId), nmos::transports::mxl, us(impl_->ids.device), {}, {},
                                nodeModel.settings);
                            audioSender.data[U("label")] = value::string(us("MV Out " + std::to_string(head) + " Audio"));
                            tagGroup(audioSender, group, "Audio");
                            nmos::insert_resource(nodeModel.node_resources, std::move(audioSender));
                            auto audioConnection = nmos::make_connection_mxl_sender(us(impl_->ids.audioSender(head)), domain, us(audioId));
                            audioConnection.data[U("active")][U("master_enable")] = value::boolean(true);
                            audioConnection.data[U("active")][U("transport_params")][0][U("mxl_domain_id")] = value::string(domain);
                            audioConnection.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = value::string(us(audioId));
                            nmos::insert_resource(nodeModel.connection_resources, std::move(audioConnection));
                        }
                    }

                    std::vector<nmos::rational> rates{{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}};
                    for (int i = 1; i <= impl_->config.maxInputs; ++i)
                    {
                        auto const group = "MV In " + std::to_string(i);
                        auto video = nmos::make_receiver(us(impl_->ids.videoReceiver(i)), us(impl_->ids.device), nmos::transports::mxl, {}, nmos::formats::video,
                            {nmos::media_types::video_v210, nmos::media_types::video_v210a}, nodeModel.settings);
                        video.data[U("label")] = value::string(us("MV In " + std::to_string(i) + " Video"));
                        tagGroup(video, group, "Video");
                        web::json::value set = web::json::value::object();
                        set[U("urn:x-nmos:cap:format:media_type")] =
                            nmos::make_caps_string_constraint({nmos::media_types::video_v210.name, nmos::media_types::video_v210a.name});
                        set[U("urn:x-nmos:cap:format:grain_rate")] = nmos::make_caps_rational_constraint(rates);
                        set[U("urn:x-nmos:cap:format:frame_width")] = nmos::make_caps_integer_constraint(std::vector<std::int64_t>{}, 2, 3840);
                        set[U("urn:x-nmos:cap:format:frame_height")] = nmos::make_caps_integer_constraint(std::vector<std::int64_t>{}, 2, 2160);
                        set[U("urn:x-nmos:cap:format:interlace_mode")] = nmos::make_caps_string_constraint(
                            {nmos::interlace_modes::progressive.name, nmos::interlace_modes::interlaced_tff.name, nmos::interlace_modes::interlaced_bff.name});
                        set[U("urn:x-nmos:cap:format:color_sampling")] = nmos::make_caps_string_constraint({sdp::samplings::YCbCr_4_2_2.name});
                        set[U("urn:x-nmos:cap:format:component_depth")] = nmos::make_caps_integer_constraint({std::int64_t{10}});
                        web::json::value sets = web::json::value::array();
                        web::json::push_back(sets, std::move(set));
                        web::json::value caps = web::json::value::object();
                        caps[U("constraint_sets")] = std::move(sets);
                        caps[U("version")] = web::json::value::string(nmos::make_version());
                        video.data[U("caps")] = std::move(caps);
                        nmos::insert_resource(nodeModel.node_resources, std::move(video));

                        auto audio = nmos::make_receiver(us(impl_->ids.audioReceiver(i)), us(impl_->ids.device), nmos::transports::mxl, {}, nmos::formats::audio,
                            {nmos::media_types::audio_float32}, nodeModel.settings);
                        audio.data[U("label")] = value::string(us("MV In " + std::to_string(i) + " Audio"));
                        tagGroup(audio, group, "Audio");
                        web::json::value audioSet = web::json::value::object();
                        audioSet[U("urn:x-nmos:cap:format:media_type")] = nmos::make_caps_string_constraint({nmos::media_types::audio_float32.name});
                        audioSet[U("urn:x-nmos:cap:format:channel_count")] = nmos::make_caps_integer_constraint(std::vector<std::int64_t>{}, 1, 64);
                        audioSet[U("urn:x-nmos:cap:format:sample_rate")] = nmos::make_caps_rational_constraint({nmos::rational{48000, 1}});
                        audioSet[U("urn:x-nmos:cap:format:sample_depth")] = nmos::make_caps_integer_constraint({std::int64_t{32}});
                        web::json::value audioSets = web::json::value::array();
                        web::json::push_back(audioSets, std::move(audioSet));
                        web::json::value audioCaps = web::json::value::object();
                        audioCaps[U("constraint_sets")] = std::move(audioSets);
                        audioCaps[U("version")] = web::json::value::string(nmos::make_version());
                        audio.data[U("caps")] = std::move(audioCaps);
                        nmos::insert_resource(nodeModel.node_resources, std::move(audio));

                        for (auto const& id : {impl_->ids.videoReceiver(i), impl_->ids.audioReceiver(i)})
                        {
                            auto connection = nmos::make_connection_mxl_receiver(us(id), utility::string_t{});
                            connection.data[U("active")][U("master_enable")] = web::json::value::boolean(false);
                            connection.data[U("staged")][U("master_enable")] = web::json::value::boolean(false);
                            connection.data[U("active")][U("transport_params")][0][U("mxl_domain_id")] = web::json::value::null();
                            connection.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = web::json::value::null();
                            nmos::insert_resource(nodeModel.connection_resources, std::move(connection));
                        }
                    }
                    nodeModel.notify();
                    lock.unlock();
                    {
                        std::lock_guard ready{impl_->readyMu};
                        impl_->ready = true;
                        impl_->running.store(true);
                        impl_->readyCv.notify_all();
                    }
                    auto waitLock = nodeModel.write_lock();
                    nodeModel.wait(waitLock, [&] { return nodeModel.shutdown; });
                }
                catch (std::exception const& ex)
                {
                    std::lock_guard ready{impl_->readyMu};
                    impl_->error = ex.what();
                    impl_->ready = true;
                    impl_->readyCv.notify_all();
                }
            });
            nmos::server_guard guard(server);
            {
                std::unique_lock readyLock{impl_->readyMu};
                impl_->readyCv.wait(readyLock, [&] { return impl_->ready; });
            }
            if (!impl_->error.empty())
            {
                throw std::runtime_error(impl_->error);
            }
            logInfo("nmos_node_ready", {{"port", std::to_string(impl_->config.nmosPort)}, {"node", impl_->ids.node}});
            while (impl_->running.load())
            {
                std::vector<Impl::SubUpdate> batch;
                {
                    std::lock_guard lock{impl_->pendingMu};
                    batch.swap(impl_->pending);
                }
                if (!batch.empty() && impl_->model != nullptr)
                {
                    auto lock = impl_->model->write_lock();
                    for (auto const& update : batch)
                    {
                        auto const id = us(update.id);
                        nmos::modify_resource(impl_->model->node_resources, id, [&](nmos::resource& resource) {
                            resource.data[U("subscription")][U("sender_id")] =
                                update.sender.empty() ? web::json::value::null() : web::json::value::string(us(update.sender));
                            resource.data[U("subscription")][U("active")] = web::json::value::boolean(update.active);
                            resource.data[U("version")] = web::json::value::string(nmos::make_version());
                        });
                    }
                    impl_->model->notify();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            {
                auto lock = nodeModel.write_lock();
                auto const announceable = [](nmos::type const& type) {
                    return type == nmos::types::device || type == nmos::types::source || type == nmos::types::flow || type == nmos::types::sender ||
                           type == nmos::types::receiver;
                };
                std::vector<nmos::id> ids;
                nmos::id nodeId;
                for (auto const& resource : nodeModel.node_resources)
                {
                    if (resource.type == nmos::types::node)
                    {
                        nodeId = resource.id;
                    }
                    else if (announceable(resource.type))
                    {
                        ids.push_back(resource.id);
                    }
                }
                for (auto const& id : ids)
                {
                    nmos::erase_resource(nodeModel.node_resources, id, false);
                }
                if (!nodeId.empty())
                {
                    nmos::erase_resource(nodeModel.node_resources, nodeId, false);
                }
                nodeModel.notify();
            }
            if (!impl_->config.nmosRegistryAddress.empty())
            {
                auto const url = "http://" + impl_->config.nmosQueryAddress + ":" + std::to_string(impl_->config.nmosQueryPort) +
                                 "/x-nmos/query/v1.3/nodes/" + impl_->ids.node;
                auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (httpGet(url, 500).status == 404)
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            {
                auto lock = nodeModel.write_lock();
                nodeModel.shutdown = true;
                nodeModel.notify();
            }
        }
        catch (std::exception const& ex)
        {
            std::lock_guard ready{impl_->readyMu};
            if (impl_->error.empty())
            {
                impl_->error = ex.what();
            }
            impl_->ready = true;
            impl_->readyCv.notify_all();
            logError("nmos_node_failed", {{"error", ex.what()}});
        }
        impl_->model = nullptr;
    });

    std::unique_lock readyLock{impl_->readyMu};
    impl_->readyCv.wait_for(readyLock, std::chrono::seconds(20), [&] { return impl_->ready; });
    if (!impl_->error.empty())
    {
        auto const message = impl_->error;
        readyLock.unlock();
        stop();
        throw std::runtime_error(message);
    }
    if (!impl_->ready)
    {
        readyLock.unlock();
        stop();
        throw std::runtime_error("NMOS node did not become ready");
    }
}

void NmosNode::stop()
{
    impl_->running.store(false);
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
}
#endif
} // namespace mv
