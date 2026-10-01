#pragma once

#include <string>

#include "media/frame.hpp"

namespace mv
{
std::string encodePreviewJpeg(Frame422 const& frame, int outWidth, int quality);
bool loadImageFile(std::string const& path, Frame422& frame);
} // namespace mv
