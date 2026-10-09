#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app/runtime.hpp"
#include "config/config.hpp"
#include "layout/book.hpp"
#include "media/imagestore.hpp"
#include "ops/metrics.hpp"

namespace mv
{
// One input route as restored from routes.json or set over IS-05.
struct RouteState
{
    int input = 0;
    bool video = true;
    bool enable = false;
    std::string domainId;
    std::string flowId;
    std::string senderId;
};

class Engine
{
public:
    Engine(Config config, RuntimeModel& runtime, LayoutBookStore& layouts, Metrics& metrics, ImageStore& images);
    ~Engine();

    void start();
    void stop();
    // Deletes this process's output domain directory. Refuses the scan root and mirror domains.
    void removeOwnDomain();
    void setRoute(int input, bool video, bool enable, std::string domainId, std::string flowId, std::string senderId);
    [[nodiscard]] std::string domainId() const;
    // Routes that are enabled or name a flow (a copy, taken under the route lock).
    [[nodiscard]] std::vector<RouteState> routes() const;
    void setFlowCallback(std::function<void(int head, std::string const& videoFlow, std::string const& audioFlow, VideoFormat const& format)> callback);

private:
    struct Impl;
    Impl* impl_;
};
} // namespace mv
