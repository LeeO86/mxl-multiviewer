// The black and freeze alarm numbers on the GPU (cudaAlarmScan, 1.4.2) against the CPU functions
// (v210LumaSum, lumaHash) on random and edge frames. Exit 0 when all are equal, 1 on a
// difference, 77 (skip) without a usable GPU, as in CI. 1.4.4: each even-height frame is also
// split into its two fields and woven on the device (cudaUploadFields, interlaced input); the
// device numbers of that frame must equal the CPU numbers of the original frame.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "media/cuda_compose.hpp"
#include "media/frame.hpp"

int main()
{
    if (!mv::cudaSupportCompiled() || !mv::cudaRuntimeAvailable())
    {
        std::puts("SKIP: no CUDA device");
        return 77;
    }
    std::mt19937 rng(1234);
    struct Case
    {
        int width;
        int height;
        char const* fill; // random, zero, max
    };
    Case const cases[] = {{1920, 1080, "random"}, {1920, 1080, "zero"}, {1920, 1080, "max"}, {1280, 720, "random"}, {3840, 2160, "random"},
        {1922, 1081, "random"}, {720, 576, "random"}, {6, 2, "random"}};
    int failures = 0;
    std::uint64_t tag = 0;
    for (auto const& c : cases)
    {
        int const rowBytes = static_cast<int>(mv::v210RowBytes(c.width));
        std::vector<std::uint8_t> v210(static_cast<std::size_t>(rowBytes) * c.height);
        for (std::size_t i = 0; i + 4 <= v210.size(); i += 4)
        {
            std::uint32_t const word = c.fill[0] == 'z' ? 0u : c.fill[0] == 'm' ? 0x3fffffffu : (rng() & 0x3fffffffu);
            std::memcpy(v210.data() + i, &word, 4);
        }
        int count = 0;
        auto const sum = mv::v210LumaSum(v210.data(), rowBytes, c.width, c.height, 32, &count);
        auto const hash = mv::lumaHash(v210.data(), rowBytes, c.width, c.height);
        auto const check = [&](std::shared_ptr<mv::CudaFrame const> const& frame, char const* how) {
            if (frame == nullptr || !mv::cudaAlarmScan(*frame, 32, ++tag))
            {
                std::printf("FAIL %s %dx%d %s: upload or scan not queued\n", how, c.width, c.height, c.fill);
                ++failures;
                return;
            }
            mv::CudaAlarmScan gpu;
            bool got = false;
            for (int i = 0; i < 2000 && !(got = mv::cudaAlarmTake(gpu)); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            bool const same = got && gpu.tag == tag && gpu.sum == sum && gpu.count == count && gpu.hash == hash;
            std::printf("%s %s %dx%d %s: sum %llu/%llu count %d/%d hash %016llx/%016llx\n", same ? "ok  " : "FAIL", how, c.width, c.height, c.fill,
                static_cast<unsigned long long>(gpu.sum), static_cast<unsigned long long>(sum), gpu.count, count,
                static_cast<unsigned long long>(gpu.hash), static_cast<unsigned long long>(hash));
            failures += same ? 0 : 1;
        };
        check(mv::cudaUploadFrame(v210.data(), rowBytes, nullptr, 0, c.width, c.height), "frame ");
        if (c.height % 2 == 0)
        {
            std::size_t const fieldBytes = static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(c.height / 2);
            std::vector<std::uint8_t> top(fieldBytes);
            std::vector<std::uint8_t> bottom(fieldBytes);
            for (int r = 0; r < c.height / 2; ++r)
            {
                std::memcpy(top.data() + static_cast<std::size_t>(r) * rowBytes, v210.data() + static_cast<std::size_t>(2 * r) * rowBytes, rowBytes);
                std::memcpy(bottom.data() + static_cast<std::size_t>(r) * rowBytes, v210.data() + static_cast<std::size_t>(2 * r + 1) * rowBytes, rowBytes);
            }
            check(mv::cudaUploadFields({top.data(), nullptr}, {bottom.data(), nullptr}, rowBytes, 0, c.width, c.height), "fields");
        }
    }
    return failures == 0 ? 0 : 1;
}
