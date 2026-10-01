#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

namespace
{
std::atomic<bool> gRun{true};
void onSignal(int) { gRun = false; }

std::uint32_t v210Row(int width) { return static_cast<std::uint32_t>(((width + 47) / 48) * 128); }

void packSolid(std::uint8_t* dst, int width, int height, std::uint16_t y, std::uint16_t cb, std::uint16_t cr)
{
    int const rowBytes = static_cast<int>(v210Row(width));
    for (int row = 0; row < height; ++row)
    {
        auto* line = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes);
        std::memset(line, 0, static_cast<std::size_t>(rowBytes));
        int const groups = (width + 5) / 6;
        for (int group = 0; group < groups; ++group)
        {
            std::uint32_t words[4];
            words[0] = (cb & 0x3ffu) | (static_cast<std::uint32_t>(y) << 10) | (static_cast<std::uint32_t>(cr) << 20);
            words[1] = (y & 0x3ffu) | (static_cast<std::uint32_t>(cb) << 10) | (static_cast<std::uint32_t>(y) << 20);
            words[2] = (cr & 0x3ffu) | (static_cast<std::uint32_t>(y) << 10) | (static_cast<std::uint32_t>(cb) << 20);
            words[3] = (y & 0x3ffu) | (static_cast<std::uint32_t>(cr) << 10) | (static_cast<std::uint32_t>(y) << 20);
            std::memcpy(line + static_cast<std::size_t>(group) * 16u, words, sizeof(words));
        }
    }
}

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
} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    auto const domain = arg(argc, argv, "--domain", "");
    auto const flow = arg(argc, argv, "--flow", "");
    int const width = std::stoi(arg(argc, argv, "--width", "192"));
    int const height = std::stoi(arg(argc, argv, "--height", "108"));
    int const rate = std::stoi(arg(argc, argv, "--rate", "50"));
    auto const y = static_cast<std::uint16_t>(std::stoi(arg(argc, argv, "--y", "512")));
    auto const cb = static_cast<std::uint16_t>(std::stoi(arg(argc, argv, "--cb", "512")));
    auto const cr = static_cast<std::uint16_t>(std::stoi(arg(argc, argv, "--cr", "512")));
    auto const audio = arg(argc, argv, "--audio-flow", "");
    if (domain.empty() || flow.empty())
    {
        std::cerr << "usage: mxl-mv-writer --domain DIR --flow UUID [--width W --height H --rate N --y Y --cb Cb --cr Cr --audio-flow UUID]\n";
        return 2;
    }
    auto* instance = mxlCreateInstance(domain.c_str(), nullptr);
    if (instance == nullptr)
    {
        std::cerr << "domain open failed\n";
        return 1;
    }
    std::string def = std::string("{\"id\":\"") + flow +
                      "\",\"format\":\"urn:x-nmos:format:video\",\"label\":\"pattern\",\"description\":\"pattern\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"Pattern:Video\"]},\"parents\":[],"
                      "\"media_type\":\"video/v210\",\"grain_rate\":{\"numerator\":" +
                      std::to_string(rate) + ",\"denominator\":1},\"frame_width\":" + std::to_string(width) + ",\"frame_height\":" + std::to_string(height) +
                      ",\"interlace_mode\":\"progressive\",\"colorspace\":\"BT709\",\"components\":[]}";
    mxlFlowWriter writer = nullptr;
    if (mxlCreateFlowWriter(instance, def.c_str(), "{\"maxCommitBatchSizeHint\":1}", &writer, nullptr, nullptr) != MXL_STATUS_OK)
    {
        std::cerr << "writer failed\n";
        return 1;
    }
    mxlFlowWriter audioWriter = nullptr;
    if (!audio.empty())
    {
        std::string audioDef = std::string("{\"id\":\"") + audio +
                               "\",\"format\":\"urn:x-nmos:format:audio\",\"label\":\"pattern-audio\",\"description\":\"pattern\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"Pattern:Audio\"]},\"parents\":[],"
                               "\"media_type\":\"audio/float32\",\"sample_rate\":{\"numerator\":48000,\"denominator\":1},\"channel_count\":2,\"bit_depth\":32}";
        mxlCreateFlowWriter(instance, audioDef.c_str(), "{\"maxCommitBatchSizeHint\":480}", &audioWriter, nullptr, nullptr);
    }
    mxlRational grainRate{rate, 1};
    std::uint64_t index = mxlGetCurrentIndex(&grainRate);
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(v210Row(width)) * static_cast<std::size_t>(height));
    packSolid(frame.data(), width, height, y, cb, cr);
    while (gRun.load())
    {
        auto const now = mxlGetCurrentIndex(&grainRate);
        if (now > index)
        {
            index = now;
        }
        mxlSleepUntil(mxlIndexToTimestamp(&grainRate, index));
        mxlGrainInfo grain{};
        std::uint8_t* payload = nullptr;
        if (mxlFlowWriterOpenGrain(writer, index, &grain, &payload) == MXL_STATUS_OK && payload != nullptr)
        {
            std::memcpy(payload, frame.data(), frame.size());
            grain.validSlices = grain.totalSlices;
            grain.flags = 0;
            mxlFlowWriterCommitGrain(writer, &grain);
        }
        if (audioWriter != nullptr)
        {
            std::size_t count = 48000 / std::max(1, rate);
            std::size_t maxWrite = count;
            mxlFlowWriterGetMaxWriteLengthSamples(audioWriter, &maxWrite);
            count = std::min(count, maxWrite);
            mxlRational audioRate{48000, 1};
            std::uint64_t const end = mxlTimestampToIndex(&audioRate, mxlIndexToTimestamp(&grainRate, index)) + count;
            mxlMutableWrappedMultiBufferSlice slice{};
            if (count > 0 && mxlFlowWriterOpenSamples(audioWriter, end, count, &slice) == MXL_STATUS_OK)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    std::size_t filled = 0;
                    for (int frag = 0; frag < 2 && filled < count; ++frag)
                    {
                        auto* pointer = static_cast<char*>(slice.base.fragments[frag].pointer);
                        if (pointer == nullptr)
                        {
                            continue;
                        }
                        auto* dst = reinterpret_cast<float*>(pointer + static_cast<std::size_t>(ch) * slice.stride);
                        std::size_t const room = slice.base.fragments[frag].size / sizeof(float);
                        std::size_t const take = std::min(room, count - filled);
                        for (std::size_t s = 0; s < take; ++s)
                        {
                            dst[s] = 0.1f;
                        }
                        filled += take;
                    }
                }
                mxlFlowWriterCommitSamples(audioWriter);
            }
        }
        ++index;
    }
    mxlReleaseFlowWriter(instance, writer);
    if (audioWriter != nullptr)
    {
        mxlReleaseFlowWriter(instance, audioWriter);
    }
    mxlDestroyInstance(instance);
    return 0;
}
