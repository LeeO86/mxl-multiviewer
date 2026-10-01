#include "media/flowtext.hpp"

#include "util/logging.hpp"

namespace mv
{
std::string videoFlowDefinition(std::string const& id, std::string const& label, std::string const& group, int width, int height, int rateNum, int rateDen)
{
    return std::string("{\"id\":\"") + id + "\",\"format\":\"urn:x-nmos:format:video\",\"label\":\"" + jsonEscape(label) +
           "\",\"description\":\"" + jsonEscape(label) + "\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" + jsonEscape(group) +
           "\"]},\"parents\":[],\"media_type\":\"video/v210\",\"grain_rate\":{\"numerator\":" + std::to_string(rateNum) + ",\"denominator\":" +
           std::to_string(rateDen) + "},\"frame_width\":" + std::to_string(width) + ",\"frame_height\":" + std::to_string(height) +
           ",\"interlace_mode\":\"progressive\",\"colorspace\":\"BT709\",\"components\":[{\"name\":\"Y\",\"width\":" + std::to_string(width) +
           ",\"height\":" + std::to_string(height) + ",\"bit_depth\":10},{\"name\":\"Cb\",\"width\":" + std::to_string(width / 2) + ",\"height\":" +
           std::to_string(height) + ",\"bit_depth\":10},{\"name\":\"Cr\",\"width\":" + std::to_string(width / 2) + ",\"height\":" + std::to_string(height) +
           ",\"bit_depth\":10}]}";
}

std::string audioFlowDefinition(std::string const& id, std::string const& label, std::string const& group, int channels)
{
    return std::string("{\"id\":\"") + id + "\",\"format\":\"urn:x-nmos:format:audio\",\"label\":\"" + jsonEscape(label) +
           "\",\"description\":\"" + jsonEscape(label) + "\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"" + jsonEscape(group) +
           "\"]},\"parents\":[],\"media_type\":\"audio/float32\",\"sample_rate\":{\"numerator\":48000,\"denominator\":1},\"channel_count\":" +
           std::to_string(channels) + ",\"bit_depth\":32}";
}
} // namespace mv
