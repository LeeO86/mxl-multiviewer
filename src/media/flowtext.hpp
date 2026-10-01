#pragma once

#include <string>

namespace mv
{
std::string videoFlowDefinition(std::string const& id, std::string const& label, std::string const& group, int width, int height, int rateNum, int rateDen);
std::string audioFlowDefinition(std::string const& id, std::string const& label, std::string const& group, int channels);
} // namespace mv
