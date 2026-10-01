#pragma once

#include <functional>
#include <string>

#include "app/runtime.hpp"
#include "config/config.hpp"
#include "layout/book.hpp"
#include "ops/metrics.hpp"

namespace mv
{
class Engine
{
public:
    Engine(Config config, RuntimeModel& runtime, LayoutBookStore& layouts, Metrics& metrics);
    ~Engine();

    void start();
    void stop();
    void setRoute(int input, bool video, bool enable, std::string domainId, std::string flowId, std::string senderId);
    [[nodiscard]] std::string domainId() const;
    void setFlowCallback(std::function<void(int head, std::string const& videoFlow, std::string const& audioFlow, VideoFormat const& format)> callback);

private:
    struct Impl;
    Impl* impl_;
};
} // namespace mv
