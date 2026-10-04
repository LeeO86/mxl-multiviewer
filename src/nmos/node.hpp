#pragma once

#include <functional>
#include <memory>
#include <string>

#include "config/config.hpp"

namespace mv
{
class NmosNode
{
public:
    using RouteFn = std::function<void(int input, bool video, bool enable, std::string domainId, std::string flowId, std::string senderId)>;

    NmosNode(Config config, RouteFn route);
    ~NmosNode();

    void start();
    void stop();
    [[nodiscard]] bool registered() const;
    [[nodiscard]] std::string summary() const;
    void updateOutputFlow(int head, std::string const& videoFlowId, std::string const& audioFlowId, VideoFormat const& format);
    // Shows a route restored at startup in the receiver's IS-05 active and staged
    // documents and its IS-04 subscription. Does not call the route callback.
    void restoreRoute(int input, bool video, bool enable, std::string const& domainId, std::string const& flowId, std::string const& senderId);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mv
