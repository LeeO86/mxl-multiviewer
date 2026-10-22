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

// The v210a key plane (MXL FlowParser): 3 × 10-bit samples per little-endian 32-bit word,
// (width + 2) / 3 words per line, the lines after the fill plane.
std::uint32_t keyRow(int width) { return static_cast<std::uint32_t>((width + 2) / 3 * 4); }

void packKey(std::uint8_t* dst, int width, int height, std::uint16_t key)
{
    std::uint32_t const word = (key & 0x3ffu) | (static_cast<std::uint32_t>(key & 0x3ffu) << 10) | (static_cast<std::uint32_t>(key & 0x3ffu) << 20);
    for (std::size_t i = 0; i < static_cast<std::size_t>(keyRow(width)) * static_cast<std::size_t>(height); i += 4)
    {
        std::memcpy(dst + i, &word, 4);
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
    // Interlaced (1.4.4): MXL stores the flow as fields, one per grain at twice --rate (25 or
    // 30000/1001 only: pass 25). Grain 2k is the first field: top for tff, bottom for bff.
    auto const interlace = arg(argc, argv, "--interlace", "");
    // The lower half of the frame (frame lines >= height / 2) in this luma, so a reader that
    // takes a field for a frame shows the edge in the wrong place.
    int const y2 = std::stoi(arg(argc, argv, "--y2", "-1"));
    // A v210a flow (1.4.5) with this key (0 transparent .. 1023 opaque) on every pixel; progressive only.
    int const key = std::stoi(arg(argc, argv, "--key", "-1"));
    if (domain.empty() || flow.empty())
    {
        std::cerr << "usage: mxl-mv-writer --domain DIR --flow UUID [--width W --height H --rate N --y Y --cb Cb --cr Cr --y2 Y --interlace tff|bff "
                     "--key K --audio-flow UUID]\n";
        return 2;
    }
    bool const fields = interlace == "tff" || interlace == "bff";
    bool const bff = interlace == "bff";
    if (!interlace.empty() && !fields)
    {
        std::cerr << "--interlace is tff or bff\n";
        return 2;
    }
    if (key > 1023 || (key >= 0 && fields))
    {
        std::cerr << "--key is 0..1023, progressive only\n";
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
                      "\"media_type\":\"" + std::string(key >= 0 ? "video/v210a" : "video/v210") + "\",\"grain_rate\":{\"numerator\":" +
                      std::to_string(rate) + ",\"denominator\":1},\"frame_width\":" + std::to_string(width) + ",\"frame_height\":" + std::to_string(height) +
                      ",\"interlace_mode\":\"" + (fields ? std::string(bff ? "interlaced_bff" : "interlaced_tff") : std::string("progressive")) + "\",\"colorspace\":\"BT709\",\"components\":[]}";
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
    mxlRational grainRate{fields ? rate * 2 : rate, 1};
    std::uint64_t index = mxlGetCurrentIndex(&grainRate);
    std::size_t const rowBytes = v210Row(width);
    std::size_t const fillBytes = rowBytes * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> frame(fillBytes + (key >= 0 ? static_cast<std::size_t>(keyRow(width)) * static_cast<std::size_t>(height) : 0));
    packSolid(frame.data(), width, height, y, cb, cr);
    if (y2 >= 0)
    {
        packSolid(frame.data() + rowBytes * static_cast<std::size_t>(height / 2), width, height - height / 2, static_cast<std::uint16_t>(y2), cb, cr);
    }
    if (key >= 0)
    {
        packKey(frame.data() + fillBytes, width, height, static_cast<std::uint16_t>(key));
    }
    // One packed grain per field parity (0 top: even frame lines, 1 bottom: odd), or the frame.
    std::vector<std::uint8_t> field[2];
    for (int parity = 0; parity < 2 && fields; ++parity)
    {
        field[parity].resize(rowBytes * static_cast<std::size_t>(height / 2));
        for (int r = 0; r < height / 2; ++r)
        {
            std::memcpy(field[parity].data() + rowBytes * static_cast<std::size_t>(r), frame.data() + rowBytes * static_cast<std::size_t>(2 * r + parity), rowBytes);
        }
    }
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
            auto const& picture = fields ? field[static_cast<int>(index & 1u) ^ (bff ? 1 : 0)] : frame;
            std::memcpy(payload, picture.data(), std::min<std::size_t>(picture.size(), grain.grainSize));
            grain.validSlices = grain.totalSlices;
            grain.flags = 0;
            mxlFlowWriterCommitGrain(writer, &grain);
        }
        if (audioWriter != nullptr)
        {
            std::size_t count = 48000 / std::max(1, static_cast<int>(grainRate.numerator));
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
