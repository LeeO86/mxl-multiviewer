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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mv
