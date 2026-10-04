#include "media/jpeg.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace mv
{
namespace
{
void yuvToRgb(std::uint16_t y10, std::uint16_t cb10, std::uint16_t cr10, std::uint8_t* rgb)
{
    double const y = y10 / 4.0;
    double const cb = cb10 / 4.0 - 128.0;
    double const cr = cr10 / 4.0 - 128.0;
    auto const clamp = [](double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    rgb[0] = clamp(y + 1.5748 * cr);
    rgb[1] = clamp(y - 0.1873 * cb - 0.4681 * cr);
    rgb[2] = clamp(y + 1.8556 * cb);
}
} // namespace

namespace
{
// Nearest-sample preview: `sample(sx, sy, cx, rgb)` writes one pixel.
template <typename Sample>
std::string encodePreview(int width, int height, int outWidth, int quality, Sample sample)
{
    if (width <= 0 || height <= 0 || outWidth < 16)
    {
        return {};
    }
    int const outHeight = std::max(16, height * outWidth / width);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(outWidth * outHeight * 3));
    int const cw = width / 2;
    for (int y = 0; y < outHeight; ++y)
    {
        int const sy = std::min(height - 1, y * height / outHeight);
        for (int x = 0; x < outWidth; ++x)
        {
            int const sx = std::min(width - 1, x * width / outWidth);
            int const cx = std::min(cw - 1, sx / 2);
            sample(sx, sy, cx, rgb.data() + static_cast<std::size_t>((y * outWidth + x) * 3));
        }
    }
    std::string out;
    stbi_write_jpg_to_func(
        [](void* context, void* data, int size) {
            auto* buffer = static_cast<std::string*>(context);
            buffer->append(static_cast<char const*>(data), static_cast<std::size_t>(size));
        },
        &out, outWidth, outHeight, 3, rgb.data(), quality);
    return out;
}
} // namespace

std::string encodePreviewJpeg(Frame422 const& frame, int outWidth, int quality)
{
    int const cw = frame.chromaWidth();
    return encodePreview(frame.width, frame.height, outWidth, quality, [&](int sx, int sy, int cx, std::uint8_t* rgb) {
        yuvToRgb(frame.y[static_cast<std::size_t>(sy * frame.width + sx)], frame.cb[static_cast<std::size_t>(sy * cw + cx)],
            frame.cr[static_cast<std::size_t>(sy * cw + cx)], rgb);
    });
}

std::string encodePreviewJpeg(std::uint8_t const* v210, int rowBytes, int width, int height, int outWidth, int quality)
{
    return encodePreview(width, height, outWidth, quality, [&](int sx, int sy, int cx, std::uint8_t* rgb) {
        std::uint16_t cb = 0;
        std::uint16_t cr = 0;
        v210Chroma(v210, rowBytes, cx, sy, cb, cr);
        yuvToRgb(v210Luma(v210, rowBytes, width, sy * width + sx), cb, cr, rgb);
    });
}

bool loadImageFile(std::string const& path, Frame422& frame)
{
    int srcW = 0;
    int srcH = 0;
    int comp = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &srcW, &srcH, &comp, 3);
    if (pixels == nullptr || srcW < 2 || srcH < 2)
    {
        return false;
    }
    int const w = srcW & ~1;
    int const h = srcH;
    frame.allocate(w, h, false);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            auto const* px = pixels + static_cast<std::size_t>((y * srcW + x) * 3);
            double const r = px[0];
            double const g = px[1];
            double const b = px[2];
            double const y8 = 16.0 + (65.481 * r + 128.553 * g + 24.966 * b) / 255.0;
            frame.y[static_cast<std::size_t>(y * w + x)] = static_cast<std::uint16_t>(std::clamp(std::lround(y8 * 4.0), 0L, 1023L));
            if ((x & 1) == 0)
            {
                double const cb = 128.0 + (-37.797 * r - 74.203 * g + 112.0 * b) / 255.0;
                double const cr = 128.0 + (112.0 * r - 93.786 * g - 18.214 * b) / 255.0;
                frame.cb[static_cast<std::size_t>(y * (w / 2) + x / 2)] = static_cast<std::uint16_t>(std::clamp(std::lround(cb * 4.0), 0L, 1023L));
                frame.cr[static_cast<std::size_t>(y * (w / 2) + x / 2)] = static_cast<std::uint16_t>(std::clamp(std::lround(cr * 4.0), 0L, 1023L));
            }
        }
    }
    stbi_image_free(pixels);
    return true;
}
} // namespace mv
