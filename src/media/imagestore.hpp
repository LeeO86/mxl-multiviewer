#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "media/image.hpp"

namespace mv
{
// Pictures of image tiles (§6.2): files stored in one folder of the config volume, and
// http(s) URLs. A worker thread fetches, decodes, and scales them; the overlay thread only
// looks results up, so a slow or failing URL never holds up a frame.
class ImageStore
{
public:
    explicit ImageStore(std::string dir);
    ~ImageStore();
    ImageStore(ImageStore const&) = delete;
    ImageStore& operator=(ImageStore const&) = delete;

    struct Info
    {
        std::string name;
        std::string type;
        std::size_t bytes = 0;
        int width = 0;
        int height = 0;
        int frames = 0;
    };
    // Stored pictures (name, type, size; the size in pixels only from save()).
    [[nodiscard]] std::vector<Info> list() const;
    // Checks the Content-Type, the magic bytes, the size, and that the picture decodes within
    // the limits, then writes it (atomically). Error text on failure.
    std::variant<Info, std::string> save(std::string const& name, std::string const& contentType, std::string const& bytes);
    bool remove(std::string const& name);
    // The stored file and its Content-Type.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> read(std::string const& name) const;

    struct Picture
    {
        std::shared_ptr<ScaledImage const> image;
        std::string error;
    };
    // The picture of `url` (else of the stored `file`) scaled to a tile. Never waits: the work
    // is queued and the result comes on a later call. No image and no error while it loads; a
    // failure is retried after 30 s. Results nobody asked for in 60 s are dropped.
    Picture get(std::string const& url, std::string const& file, int width, int height, ScaleMode mode);

private:
    using Clock = std::chrono::steady_clock;
    struct Source
    {
        std::shared_ptr<DecodedImage const> image;
        std::string error;
        Clock::time_point retryAt;
        Clock::time_point used;
    };
    struct Scaled
    {
        std::string source;
        int width = 0;
        int height = 0;
        ScaleMode mode = ScaleMode::Fit;
        std::shared_ptr<ScaledImage const> image;
        std::string error;
        Clock::time_point retryAt;
        Clock::time_point used;
        bool queued = false;
    };

    void work();
    [[nodiscard]] std::variant<DecodedImage, std::string> load(std::string const& source) const;
    void forget(std::string const& name);

    std::string dir_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, Source> sources_;
    std::map<std::string, Scaled> scaled_;
    std::deque<std::string> jobs_;
    bool stop_ = false;
    std::thread worker_;
};
} // namespace mv
