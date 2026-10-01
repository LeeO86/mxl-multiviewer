#pragma once

#include <string>

namespace mv
{
inline constexpr char kUuidNamespaceUrl[] = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";

struct NmosIds
{
    std::string seed;
    std::string node;
    std::string device;
    std::string domain;
    [[nodiscard]] std::string videoReceiver(int input) const;
    [[nodiscard]] std::string audioReceiver(int input) const;
    [[nodiscard]] std::string videoSource(int head) const;
    [[nodiscard]] std::string videoSender(int head) const;
    [[nodiscard]] std::string videoFlow(int head, std::string const& formatToken) const;
    [[nodiscard]] std::string audioSource(int head) const;
    [[nodiscard]] std::string audioSender(int head) const;
    [[nodiscard]] std::string audioFlow(int head, int channels) const;
};

NmosIds makeNmosIds(std::string const& seed);
} // namespace mv
