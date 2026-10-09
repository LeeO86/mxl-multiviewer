#include "media/image.hpp"

#include "layout/geometry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>

#include "stb/stb_image.h"
#include <webp/demux.h>

namespace mv
{
namespace
{
unsigned byteAt(std::string_view bytes, std::size_t i)
{
    return static_cast<unsigned char>(bytes[i]);
}

// Logical screen size and frame count of a GIF from its block structure, without decoding,
// so a large animation is refused before its frames are allocated.
std::optional<std::string> gifShape(std::string_view b, int& width, int& height, int& frames)
{
    if (b.size() < 13)
    {
        return std::string("the GIF is cut short");
    }
    width = static_cast<int>(byteAt(b, 6) | byteAt(b, 7) << 8);
    height = static_cast<int>(byteAt(b, 8) | byteAt(b, 9) << 8);
    std::size_t pos = 13;
    if ((byteAt(b, 10) & 0x80u) != 0)
    {
        pos += 3u << ((byteAt(b, 10) & 7u) + 1u);
    }
    auto const skipSubBlocks = [&] {
        while (pos < b.size())
        {
            auto const size = byteAt(b, pos);
            pos += 1u + size;
            if (size == 0)
            {
                return true;
            }
        }
        return false;
    };
    frames = 0;
    while (pos < b.size())
    {
        auto const kind = byteAt(b, pos++);
        if (kind == 0x3B)
        {
            break;
        }
        if (kind == 0x21 && pos < b.size())
        {
            ++pos;
            if (!skipSubBlocks())
            {
                break;
            }
        }
        else if (kind == 0x2C && pos + 9 < b.size())
        {
            auto const packed = byteAt(b, pos + 8);
            pos += 9;
            if ((packed & 0x80u) != 0)
            {
                pos += 3u << ((packed & 7u) + 1u);
            }
            ++pos; // LZW minimum code size
            if (!skipSubBlocks())
            {
                break;
            }
            ++frames;
        }
        else
        {
            return std::string("the GIF has a broken block");
        }
    }
    return frames > 0 ? std::nullopt : std::optional<std::string>("the GIF has no frame");
}

std::optional<std::string> checkSize(int width, int height, long long frames)
{
    if (width < 1 || height < 1)
    {
        return std::string("the picture has no pixels");
    }
    if (width > kImageMaxSide || height > kImageMaxSide)
    {
        return "the picture is larger than " + std::to_string(kImageMaxSide) + " × " + std::to_string(kImageMaxSide) + " pixels";
    }
    if (frames * width * height > kImageMaxPixels)
    {
        return std::string("the animation has more than 32 megapixels in all frames");
    }
    return std::nullopt;
}

std::optional<std::string> decodeWebp(std::string_view bytes, DecodedImage& image)
{
    WebPData const data{reinterpret_cast<std::uint8_t const*>(bytes.data()), bytes.size()};
    WebPAnimDecoderOptions options;
    if (WebPAnimDecoderOptionsInit(&options) == 0)
    {
        return std::string("the WebP decoder is not available");
    }
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(WebPAnimDecoderNew(&data, &options), WebPAnimDecoderDelete);
    WebPAnimInfo info{};
    if (decoder == nullptr || WebPAnimDecoderGetInfo(decoder.get(), &info) == 0)
    {
        return std::string("the WebP picture does not decode");
    }
    image.width = static_cast<int>(info.canvas_width);
    image.height = static_cast<int>(info.canvas_height);
    if (auto const problem = checkSize(image.width, image.height, info.frame_count))
    {
        return problem;
    }
    auto const frameBytes = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u;
    int previous = 0;
    while (WebPAnimDecoderHasMoreFrames(decoder.get()) != 0)
    {
        std::uint8_t* pixels = nullptr;
        int timestamp = 0;
        if (WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp) == 0 || pixels == nullptr)
        {
            return std::string("the WebP picture does not decode");
        }
        image.frames.emplace_back(pixels, pixels + frameBytes);
        image.delaysMs.push_back(timestamp - previous);
        previous = timestamp;
    }
    return std::nullopt;
}

// Weights of the source samples behind each of `count` output samples that cover
// [start, start + length) of a source axis with `size` samples: the area average when
// shrinking, bilinear when growing.
struct Taps
{
    int first = 0;
    std::vector<float> weights;
};

std::vector<Taps> axisTaps(float start, float length, int size, int count)
{
    std::vector<Taps> taps(static_cast<std::size_t>(count));
    float const ratio = length / static_cast<float>(count);
    for (int i = 0; i < count; ++i)
    {
        auto& tap = taps[static_cast<std::size_t>(i)];
        if (ratio > 1.0f)
        {
            float const a = start + static_cast<float>(i) * ratio;
            float const b = a + ratio;
            int const i0 = std::clamp(static_cast<int>(std::floor(a)), 0, size - 1);
            int const i1 = std::clamp(static_cast<int>(std::ceil(b)) - 1, i0, size - 1);
            tap.first = i0;
            float sum = 0;
            for (int s = i0; s <= i1; ++s)
            {
                float const w = std::max(0.0f, std::min(b, static_cast<float>(s + 1)) - std::max(a, static_cast<float>(s)));
                tap.weights.push_back(w);
                sum += w;
            }
            for (auto& w : tap.weights)
            {
                w = sum > 0 ? w / sum : 1.0f / static_cast<float>(tap.weights.size());
            }
        }
        else
        {
            float const c = std::clamp(start + (static_cast<float>(i) + 0.5f) * ratio - 0.5f, 0.0f, static_cast<float>(size - 1));
            int const i0 = static_cast<int>(std::floor(c));
            float const f = c - static_cast<float>(i0);
            tap.first = i0;
            tap.weights = {1.0f - f};
            if (i0 + 1 < size && f > 0)
            {
                tap.weights.push_back(f);
            }
        }
    }
    return taps;
}
} // namespace

bool imageName(std::string const& name)
{
    if (name.empty() || name.size() > 100 || name.front() == '.')
    {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-'; });
}

char const* imageTypeName(ImageType type)
{
    switch (type)
    {
    case ImageType::Jpeg:
        return "image/jpeg";
    case ImageType::Gif:
        return "image/gif";
    case ImageType::Webp:
        return "image/webp";
    case ImageType::Png:
        return "image/png";
    }
    return "image/png";
}

std::optional<ImageType> sniffImage(std::string_view bytes)
{
    if (bytes.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8))
    {
        return ImageType::Png;
    }
    if (bytes.size() >= 3 && byteAt(bytes, 0) == 0xFF && byteAt(bytes, 1) == 0xD8 && byteAt(bytes, 2) == 0xFF)
    {
        return ImageType::Jpeg;
    }
    if (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a")
    {
        return ImageType::Gif;
    }
    if (bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP")
    {
        return ImageType::Webp;
    }
    return std::nullopt;
}

std::optional<ImageType> imageTypeOf(std::string_view contentType)
{
    std::string type(contentType.substr(0, contentType.find(';')));
    type.erase(std::remove_if(type.begin(), type.end(), [](unsigned char c) { return std::isspace(c) != 0; }), type.end());
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (auto const kind : {ImageType::Png, ImageType::Jpeg, ImageType::Gif, ImageType::Webp})
    {
        if (type == imageTypeName(kind))
        {
            return kind;
        }
    }
    return std::nullopt;
}

std::optional<std::string> decodeImage(std::string_view bytes, DecodedImage& out)
{
    if (bytes.size() > kImageMaxBytes)
    {
        return std::string("the file is larger than 8 MiB");
    }
    auto const type = sniffImage(bytes);
    if (!type)
    {
        return std::string("not a PNG, JPEG, GIF, or WebP picture");
    }
    DecodedImage image;
    image.type = *type;
    auto const* data = reinterpret_cast<stbi_uc const*>(bytes.data());
    int const length = static_cast<int>(bytes.size());
    if (*type == ImageType::Webp)
    {
        if (auto const problem = decodeWebp(bytes, image))
        {
            return problem;
        }
    }
    else if (*type == ImageType::Gif)
    {
        int frames = 0;
        if (auto const problem = gifShape(bytes, image.width, image.height, frames))
        {
            return problem;
        }
        if (auto const problem = checkSize(image.width, image.height, frames))
        {
            return problem;
        }
        int* delays = nullptr;
        int layers = 0;
        int comp = 0;
        stbi_uc* pixels = stbi_load_gif_from_memory(data, length, &delays, &image.width, &image.height, &layers, &comp, 4);
        if (pixels == nullptr)
        {
            return std::string("the GIF does not decode: ") + stbi_failure_reason();
        }
        auto const frameBytes = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u;
        for (int i = 0; i < layers; ++i)
        {
            image.frames.emplace_back(pixels + static_cast<std::size_t>(i) * frameBytes, pixels + static_cast<std::size_t>(i + 1) * frameBytes);
            image.delaysMs.push_back(delays != nullptr ? delays[i] : 0);
        }
        stbi_image_free(pixels);
        stbi_image_free(delays);
    }
    else
    {
        int comp = 0;
        if (stbi_info_from_memory(data, length, &image.width, &image.height, &comp) == 0)
        {
            return std::string("the picture does not decode: ") + stbi_failure_reason();
        }
        if (auto const problem = checkSize(image.width, image.height, 1))
        {
            return problem;
        }
        stbi_uc* pixels = stbi_load_from_memory(data, length, &image.width, &image.height, &comp, 4);
        if (pixels == nullptr)
        {
            return std::string("the picture does not decode: ") + stbi_failure_reason();
        }
        image.frames.emplace_back(pixels, pixels + static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u);
        image.delaysMs.push_back(0);
        stbi_image_free(pixels);
    }
    if (image.frames.empty())
    {
        return std::string("the picture has no frame");
    }
    // As browsers do: a frame time under 20 ms (often 0) plays as 100 ms.
    for (auto& delay : image.delaysMs)
    {
        delay = delay < 20 ? 100 : delay;
    }
    out = std::move(image);
    return std::nullopt;
}

std::optional<std::string> scaleImage(DecodedImage const& image, int tileWidth, int tileHeight, ScaleMode mode, ScaledImage& out)
{
    if (image.frames.empty() || tileWidth < 2 || tileHeight < 1)
    {
        return std::string("the tile is too small");
    }
    auto const place = placeTile(PixelRect{0, 0, tileWidth, tileHeight}, image.width, image.height, mode);
    int const w = std::min(place.dst.w, tileWidth);
    int const h = std::min(place.dst.h, tileHeight);
    if (static_cast<long long>(w) * h * static_cast<long long>(image.frames.size()) > kImageMaxPixels)
    {
        return std::string("the animation has more than 32 megapixels in all frames at this tile size");
    }
    auto const columns = axisTaps(place.srcX, place.srcW, image.width, w);
    auto const rows = axisTaps(place.srcY, place.srcH, image.height, h);
    ScaledImage scaled;
    scaled.x = std::max(0, place.dst.x);
    scaled.y = std::max(0, place.dst.y);
    scaled.width = w;
    scaled.height = h;
    scaled.delaysMs = image.delaysMs;
    for (auto const& frame : image.frames)
    {
        // Columns first, one source line at a time (premultiplied), kept while output lines need it.
        std::map<int, std::vector<float>> lines;
        auto const line = [&](int y) -> std::vector<float> const& {
            auto& done = lines[y];
            if (done.empty())
            {
                done.assign(static_cast<std::size_t>(w) * 4u, 0.0f);
                auto const* src = frame.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) * 4u;
                for (int x = 0; x < w; ++x)
                {
                    auto const& tap = columns[static_cast<std::size_t>(x)];
                    float* px = done.data() + static_cast<std::size_t>(x) * 4u;
                    for (std::size_t k = 0; k < tap.weights.size(); ++k)
                    {
                        auto const* s = src + static_cast<std::size_t>(tap.first + static_cast<int>(k)) * 4u;
                        float const a = s[3] * tap.weights[k];
                        px[0] += s[0] * a;
                        px[1] += s[1] * a;
                        px[2] += s[2] * a;
                        px[3] += a;
                    }
                }
            }
            return done;
        };
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        std::vector<float const*> sources;
        for (int y = 0; y < h; ++y)
        {
            auto const& tap = rows[static_cast<std::size_t>(y)];
            lines.erase(lines.begin(), lines.lower_bound(tap.first));
            sources.clear();
            for (std::size_t k = 0; k < tap.weights.size(); ++k)
            {
                sources.push_back(line(tap.first + static_cast<int>(k)).data());
            }
            for (int x = 0; x < w; ++x)
            {
                float r = 0;
                float g = 0;
                float b = 0;
                float a = 0;
                for (std::size_t k = 0; k < tap.weights.size(); ++k)
                {
                    auto const* px = sources[k] + static_cast<std::size_t>(x) * 4u;
                    r += px[0] * tap.weights[k];
                    g += px[1] * tap.weights[k];
                    b += px[2] * tap.weights[k];
                    a += px[3] * tap.weights[k];
                }
                // Premultiplied: colour × alpha / 255 per channel, never above alpha.
                auto const alpha = static_cast<std::uint32_t>(std::clamp(std::lround(a), 0L, 255L));
                auto const channel = [&](float v) { return std::min(alpha, static_cast<std::uint32_t>(std::clamp(std::lround(v / 255.0f), 0L, 255L))); };
                pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                    alpha << 24 | channel(r) << 16 | channel(g) << 8 | channel(b);
            }
        }
        scaled.frames.push_back(std::move(pixels));
    }
    out = std::move(scaled);
    return std::nullopt;
}

std::size_t imageFrameAt(std::vector<int> const& delaysMs, std::int64_t elapsedMs)
{
    std::int64_t total = 0;
    for (auto const delay : delaysMs)
    {
        total += std::max(1, delay);
    }
    if (delaysMs.size() < 2 || total <= 0)
    {
        return 0;
    }
    auto at = ((elapsedMs % total) + total) % total;
    for (std::size_t i = 0; i < delaysMs.size(); ++i)
    {
        at -= std::max(1, delaysMs[i]);
        if (at < 0)
        {
            return i;
        }
    }
    return 0;
}
} // namespace mv
