#pragma once

#include <string>

#include "media/frame.hpp"

namespace mv
{
std::string encodePreviewJpeg(Frame422 const& frame, int outWidth, int quality);
// The same preview read straight from packed v210: only the preview's samples are
// decoded, not the whole frame.
std::string encodePreviewJpeg(std::uint8_t const* v210, int rowBytes, int width, int height, int outWidth, int quality);
bool loadImageFile(std::string const& path, Frame422& frame);
} // namespace mv
