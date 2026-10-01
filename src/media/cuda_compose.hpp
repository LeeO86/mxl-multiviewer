#pragma once

#include <cstddef>
#include <cstdint>

namespace mv
{
struct CudaTileView
{
    int dstX = 0;
    int dstY = 0;
    int dstW = 0;
    int dstH = 0;
    float srcX = 0;
    float srcY = 0;
    float srcW = 0;
    float srcH = 0;
    int srcWidth = 0;
    int srcHeight = 0;
    std::uint16_t const* y = nullptr;
    std::uint16_t const* cb = nullptr;
    std::uint16_t const* cr = nullptr;
    std::uint16_t const* a = nullptr;
    std::uint8_t const* v210 = nullptr;
    int v210RowBytes = 0;
    std::uint8_t const* alpha10 = nullptr;
    int alphaRowBytes = 0;
    bool bob = false;
    bool solid = false;
};

struct CudaComposeDesc
{
    int width = 0;
    int height = 0;
    std::uint16_t bgY = 64;
    std::uint16_t bgCb = 512;
    std::uint16_t bgCr = 512;
    std::uint16_t const* backgroundY = nullptr;
    std::uint16_t const* backgroundCb = nullptr;
    std::uint16_t const* backgroundCr = nullptr;
    int backgroundWidth = 0;
    int backgroundHeight = 0;
    CudaTileView const* tiles = nullptr;
    int tileCount = 0;
    std::uint8_t const* rgba = nullptr;
    int rgbaStride = 0;
    std::uint8_t* v210Out = nullptr;
    int v210RowBytes = 0;
};

enum class CudaComposeStatus
{
    Ok,
    Unavailable,
    Failed
};

// True when this binary was compiled with nvcc.
bool cudaSupportCompiled();

int cudaDeviceCount();
bool cudaRuntimeAvailable();
void cudaDeviceMemory(std::uint64_t* freeBytes, std::uint64_t* totalBytes);

// Unpack, scale, composite, blend, and pack on the device. `v210Out` receives one packed frame.
CudaComposeStatus cudaComposeFrame(CudaComposeDesc const& desc);
} // namespace mv
