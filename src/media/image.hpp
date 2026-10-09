#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "layout/model.hpp"

namespace mv
{
// Pictures of image tiles (§6.2): PNG, JPEG, GIF (animated GIFs play), and WebP, from an
// http(s) URL or stored in the config volume. These limits apply to both.
inline constexpr std::size_t kImageMaxBytes = 8u * 1024u * 1024u;
inline constexpr int kImageMaxSide = 4096;
// Pixels over all frames, decoded and again after scaling to the tile (128 MiB as RGBA).
inline constexpr long long kImageMaxPixels = 32LL * 1024 * 1024;

enum class ImageType
{
    Png,
    Jpeg,
    Gif,
    Webp
};

char const* imageTypeName(ImageType type);
// A stored picture's name: 1–100 letters, digits, '.', '_', '-', not starting with '.'.
bool imageName(std::string const& name);
// The type the first bytes (magic number) say, if it is one of the four.
std::optional<ImageType> sniffImage(std::string_view bytes);
// The type of an HTTP Content-Type (`image/png; …`), if it is one of the four.
std::optional<ImageType> imageTypeOf(std::string_view contentType);

// Straight RGBA frames of one size; `delaysMs` per frame (one frame: a still picture).
struct DecodedImage
{
    ImageType type = ImageType::Png;
    int width = 0;
    int height = 0;
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<int> delaysMs;
};

// Checks size, magic, and the pixel limits (before decoding), then decodes. Error text on failure.
std::optional<std::string> decodeImage(std::string_view bytes, DecodedImage& out);

// The picture of an image tile, scaled once for a tile size: premultiplied BGRA pixels
// (Blend2D PRGB32), placed at (x, y) in the tile, transparent around it.
struct ScaledImage
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::vector<std::uint32_t>> frames;
    std::vector<int> delaysMs;
};

// Fit or fill into a tileWidth × tileHeight tile (area average when shrinking, bilinear when
// growing). Error text when the frames would pass kImageMaxPixels.
std::optional<std::string> scaleImage(DecodedImage const& image, int tileWidth, int tileHeight, ScaleMode mode, ScaledImage& out);

// The frame an animation shows `elapsedMs` after it started; it loops.
std::size_t imageFrameAt(std::vector<int> const& delaysMs, std::int64_t elapsedMs);
} // namespace mv
