#include "media/imagestore.hpp"

#include "util/fetch.hpp"
#include "util/logging.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace mv
{
namespace
{
// A URL gets 3 s to connect and 10 s in all (§6.2).
constexpr int kConnectMs = 3000;
constexpr int kFetchMs = 10000;
constexpr auto kRetry = std::chrono::seconds(30);
constexpr auto kKeep = std::chrono::seconds(60);

std::string sourceKey(std::string const& url, std::string const& file)
{
    return url.empty() ? "file:" + file : "url:" + url;
}

std::optional<std::string> readFile(std::filesystem::path const& path, std::size_t maxBytes)
{
    std::error_code ec;
    auto const size = std::filesystem::file_size(path, ec);
    if (ec || size > maxBytes)
    {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return in || in.eof() ? std::optional<std::string>(std::move(bytes)) : std::nullopt;
}
} // namespace

ImageStore::ImageStore(std::string dir)
    : dir_(std::move(dir))
    , worker_([this] { work(); })
{
}

ImageStore::~ImageStore()
{
    {
        std::lock_guard lock{mutex_};
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

std::vector<ImageStore::Info> ImageStore::list() const
{
    std::vector<Info> out;
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator(dir_, ec))
    {
        auto const name = entry.path().filename().string();
        if (!entry.is_regular_file(ec) || !imageName(name))
        {
            continue;
        }
        Info info;
        info.name = name;
        info.bytes = static_cast<std::size_t>(entry.file_size(ec));
        std::ifstream in(entry.path(), std::ios::binary);
        std::string head(12, '\0');
        in.read(head.data(), static_cast<std::streamsize>(head.size()));
        auto const type = sniffImage(std::string_view(head.data(), static_cast<std::size_t>(in.gcount())));
        info.type = type ? imageTypeName(*type) : "";
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](Info const& a, Info const& b) { return a.name < b.name; });
    return out;
}

std::variant<ImageStore::Info, std::string> ImageStore::save(std::string const& name, std::string const& contentType, std::string const& bytes)
{
    if (!imageName(name))
    {
        return std::string("a picture name has 1-100 letters, digits, '.', '_' or '-' and does not start with '.'");
    }
    auto const declared = imageTypeOf(contentType);
    if (!declared)
    {
        return std::string("Content-Type must be image/png, image/jpeg, image/gif, or image/webp");
    }
    if (sniffImage(bytes) != declared)
    {
        return std::string("the file is not ") + imageTypeName(*declared);
    }
    DecodedImage image;
    if (auto const problem = decodeImage(bytes, image))
    {
        return *problem;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    auto const path = std::filesystem::path(dir_) / name;
    auto const tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            return std::string("cannot write the picture");
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        return std::string("cannot write the picture");
    }
    forget(name);
    return Info{name, imageTypeName(image.type), bytes.size(), image.width, image.height, static_cast<int>(image.frames.size())};
}

bool ImageStore::remove(std::string const& name)
{
    std::error_code ec;
    bool const removed = imageName(name) && std::filesystem::remove(std::filesystem::path(dir_) / name, ec);
    forget(name);
    return removed;
}

std::optional<std::pair<std::string, std::string>> ImageStore::read(std::string const& name) const
{
    if (!imageName(name))
    {
        return std::nullopt;
    }
    auto bytes = readFile(std::filesystem::path(dir_) / name, kImageMaxBytes);
    if (!bytes)
    {
        return std::nullopt;
    }
    auto const type = sniffImage(*bytes);
    return std::pair{std::move(*bytes), std::string(type ? imageTypeName(*type) : "application/octet-stream")};
}

void ImageStore::forget(std::string const& name)
{
    std::lock_guard lock{mutex_};
    auto const key = sourceKey({}, name);
    sources_.erase(key);
    std::erase_if(scaled_, [&](auto const& entry) { return entry.second.source == key && !entry.second.queued; });
}

ImageStore::Picture ImageStore::get(std::string const& url, std::string const& file, int width, int height, ScaleMode mode)
{
    if (url.empty() && file.empty())
    {
        return {nullptr, "no picture is set"};
    }
    auto const source = sourceKey(url, file);
    auto const key = source + "|" + std::to_string(width) + "x" + std::to_string(height) + (mode == ScaleMode::Fill ? "|fill" : "|fit");
    auto const now = Clock::now();
    std::lock_guard lock{mutex_};
    auto& entry = scaled_[key];
    entry.used = now;
    if (auto const it = sources_.find(source); it != sources_.end())
    {
        it->second.used = now;
    }
    if (entry.image == nullptr && !entry.queued && (entry.error.empty() || now >= entry.retryAt))
    {
        entry.source = source;
        entry.width = width;
        entry.height = height;
        entry.mode = mode;
        entry.queued = true;
        jobs_.push_back(key);
        wake_.notify_one();
    }
    return {entry.image, entry.error};
}

std::variant<DecodedImage, std::string> ImageStore::load(std::string const& source) const
{
    std::string bytes;
    std::string declared;
    if (source.rfind("url:", 0) == 0)
    {
        auto const result = fetchUrl(source.substr(4), kImageMaxBytes, kConnectMs, kFetchMs);
        if (!result.error.empty())
        {
            return result.error;
        }
        if (result.status != 200)
        {
            return "HTTP " + std::to_string(result.status);
        }
        auto const type = imageTypeOf(result.contentType);
        if (!type)
        {
            return "not a picture (Content-Type " + (result.contentType.empty() ? std::string("missing") : result.contentType) + ")";
        }
        if (sniffImage(result.body) != type)
        {
            return std::string("the file is not ") + imageTypeName(*type);
        }
        bytes = result.body;
    }
    else
    {
        auto const name = source.substr(5);
        auto file = imageName(name) ? readFile(std::filesystem::path(dir_) / name, kImageMaxBytes) : std::nullopt;
        if (!file)
        {
            return "no stored picture " + name;
        }
        bytes = std::move(*file);
    }
    DecodedImage image;
    if (auto const problem = decodeImage(bytes, image))
    {
        return *problem;
    }
    return image;
}

void ImageStore::work()
{
    for (;;)
    {
        std::string key;
        Scaled job;
        std::shared_ptr<DecodedImage const> decoded;
        bool reload = false;
        {
            std::unique_lock lock{mutex_};
            wake_.wait_for(lock, std::chrono::seconds(5), [&] { return stop_ || !jobs_.empty(); });
            if (stop_)
            {
                return;
            }
            // Pictures no tile asked for in a while go.
            auto const now = Clock::now();
            std::erase_if(scaled_, [&](auto const& entry) { return !entry.second.queued && now - entry.second.used > kKeep; });
            std::erase_if(sources_, [&](auto const& entry) { return now - entry.second.used > kKeep; });
            if (jobs_.empty())
            {
                continue;
            }
            key = jobs_.front();
            jobs_.pop_front();
            job = scaled_[key];
            auto& source = sources_[job.source];
            source.used = now;
            decoded = source.image;
            reload = decoded == nullptr && (source.error.empty() || now >= source.retryAt);
        }
        std::string error;
        if (reload)
        {
            auto loaded = load(job.source);
            std::lock_guard lock{mutex_};
            auto& source = sources_[job.source];
            source.used = Clock::now();
            if (auto* image = std::get_if<DecodedImage>(&loaded))
            {
                source.image = std::make_shared<DecodedImage const>(std::move(*image));
                source.error.clear();
            }
            else
            {
                source.error = std::get<std::string>(loaded);
                source.retryAt = Clock::now() + kRetry;
                logWarn("image_failed", {{"source", job.source}, {"error", source.error}});
            }
            decoded = source.image;
            error = source.error;
        }
        else if (decoded == nullptr)
        {
            std::lock_guard lock{mutex_};
            error = sources_[job.source].error;
        }
        std::shared_ptr<ScaledImage const> image;
        if (decoded != nullptr)
        {
            ScaledImage scaled;
            if (auto const problem = scaleImage(*decoded, job.width, job.height, job.mode, scaled))
            {
                error = *problem;
            }
            else
            {
                image = std::make_shared<ScaledImage const>(std::move(scaled));
            }
        }
        std::lock_guard lock{mutex_};
        auto& entry = scaled_[key];
        entry.queued = false;
        entry.image = image;
        entry.error = image != nullptr ? std::string{} : error;
        entry.retryAt = Clock::now() + kRetry;
    }
}
} // namespace mv
