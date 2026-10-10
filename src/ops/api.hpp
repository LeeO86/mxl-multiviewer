#pragma once

#include "app/runtime.hpp"
#include "config/store.hpp"
#include "layout/book.hpp"
#include "media/imagestore.hpp"
#include "media/preview.hpp"
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
    // Stored pictures of image tiles (/api/v1/images); without it those routes are 404.
    void setImages(ImageStore& images);
    // The page served for /widget/<id> (the embedded UI).
    void setIndexPage(std::string page);
    // MV_PREVIEW_MODE=webrtc: the publish state for /statusz (§8.4).
    void setPreviewStatus(std::function<PreviewStatus()> status);
    [[nodiscard]] HttpResponse handle(HttpRequest const& request);
    [[nodiscard]] std::string eventsJson() const;

private:
    Config config_;
    ConfigStore& store_;
    LayoutBookStore& layouts_;
    RuntimeModel& runtime_;
    Metrics& metrics_;
    std::function<void(OutputFlowNote const&)> onFlow_;
    ImageStore* images_ = nullptr;
    std::string indexPage_;
    std::function<PreviewStatus()> previewStatus_;

    [[nodiscard]] std::string previewJson() const;
};
} // namespace mv
