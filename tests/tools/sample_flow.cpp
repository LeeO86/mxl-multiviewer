#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

namespace
{
std::string arg(int argc, char** argv, char const* name, std::string const& fallback)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::string(argv[i]) == name)
        {
            return argv[i + 1];
        }
    }
    return fallback;
}

std::uint16_t sample10(std::uint32_t word, int shift)
{
    return static_cast<std::uint16_t>((word >> shift) & 0x3ffu);
}
} // namespace

int main(int argc, char** argv)
{
    auto const domain = arg(argc, argv, "--domain", "");
    auto const flow = arg(argc, argv, "--flow", "");
    int const x = std::stoi(arg(argc, argv, "--x", "0"));
    int const y = std::stoi(arg(argc, argv, "--y", "0"));
    int const width = std::stoi(arg(argc, argv, "--width", "192"));
    if (domain.empty() || flow.empty())
    {
        std::cerr << "usage: mxl-mv-sample --domain DIR --flow UUID --x X --y Y --width W\n";
        return 2;
    }
    auto* instance = mxlCreateInstance(domain.c_str(), nullptr);
    if (instance == nullptr)
    {
        return 1;
    }
    mxlFlowReader reader = nullptr;
    for (int attempt = 0; attempt < 50 && mxlCreateFlowReader(instance, flow.c_str(), nullptr, &reader) != MXL_STATUS_OK; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (reader == nullptr)
    {
        std::cerr << "no reader\n";
        return 1;
    }
    mxlGrainInfo grain{};
    std::uint8_t* payload = nullptr;
    mxlStatus status = MXL_ERR_FLOW_NOT_FOUND;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        mxlFlowInfo info{};
        if (mxlFlowReaderGetInfo(reader, &info) == MXL_STATUS_OK)
        {
            status = mxlFlowReaderGetGrain(reader, info.runtime.headIndex, 20000000, &grain, &payload);
            if (status == MXL_STATUS_OK && payload != nullptr && (grain.flags & MXL_GRAIN_FLAG_INVALID) == 0)
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    if (status != MXL_STATUS_OK || payload == nullptr)
    {
        std::cerr << "no grain\n";
        return 1;
    }
    int const rowBytes = ((width + 47) / 48) * 128;
    auto const* line = payload + static_cast<std::size_t>(y) * static_cast<std::size_t>(rowBytes);
    int const group = x / 6;
    int const within = x % 6;
    std::uint32_t words[4] = {};
    std::memcpy(words, line + static_cast<std::size_t>(group) * 16u, sizeof(words));
    std::uint16_t ys[6] = {sample10(words[0], 10), sample10(words[1], 0), sample10(words[1], 20), sample10(words[2], 10), sample10(words[3], 0), sample10(words[3], 20)};
    std::cout << ys[within] << "\n";
    mxlReleaseFlowReader(instance, reader);
    mxlDestroyInstance(instance);
    return 0;
}
