#pragma once

#include "app/runtime.hpp"
#include "config/store.hpp"
#include "layout/book.hpp"
#include "ops/httpserver.hpp"
#include "ops/metrics.hpp"

#include <functional>
#include <string>

namespace mv
{
struct OutputFlowNote
{
    int head = 1;
    std::string videoFlowId;
    std::string audioFlowId;
    VideoFormat format;
};

class Api
{
public:
    Api(Config config, ConfigStore& store, LayoutBookStore& layouts, RuntimeModel& runtime, Metrics& metrics);
    void setFlowCallback(std::function<void(OutputFlowNote const&)> callback);
    [[nodiscard]] HttpResponse handle(HttpRequest const& request);
    [[nodiscard]] std::string eventsJson() const;

private:
    Config config_;
    ConfigStore& store_;
    LayoutBookStore& layouts_;
    RuntimeModel& runtime_;
    Metrics& metrics_;
    std::function<void(OutputFlowNote const&)> onFlow_;
};
} // namespace mv
