#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mv
{
// One source grain on the device, packed as in MXL. The input thread uploads each
// grain once; every output head composes from the same frame.
struct CudaFrame;

// One head's overlay (straight RGBA) on the device, uploaded by its overlay thread.
struct CudaOverlay;

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
    // Device-resident source; when set, y/cb/cr/a are not used.
    CudaFrame const* frame = nullptr;
    bool bob = false;
    bool solid = false;
};

struct CudaRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// GPU time per compose stage in milliseconds, from CUDA events.
struct CudaComposeTiming
{
    float background = 0;
    float tiles = 0;
    float overlay = 0;
    float pack = 0;
    float download = 0;
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
    // Bump when the background or the RGBA overlay changes; the device keeps its copy
    // until then. 0 uploads every frame.
    std::uint64_t backgroundVersion = 0;
    CudaTileView const* tiles = nullptr;
    int tileCount = 0;
    // The overlay already on the device (preferred), else the host copy, uploaded here
    // when rgbaVersion changes.
    CudaOverlay const* overlay = nullptr;
    std::uint8_t const* rgba = nullptr;
    int rgbaStride = 0;
    std::uint64_t rgbaVersion = 0;
    std::uint8_t* v210Out = nullptr;
    int v210RowBytes = 0;
    // v210Out is an MXL grain of the calling thread's writer: it is page-locked on first
    // use and written by DMA (release with cudaReleaseHostMemory before the writer).
    bool v210OutIsGrain = false;
    // Filled when set and compose succeeds.
    CudaComposeTiming* timing = nullptr;
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

// Upload one packed grain (v210 fill, optional packed 10-bit key) on the calling
// thread's own stream. Returns once the copy is queued; compose waits for it on the
// GPU and reads the packed samples in place. The MXL grain memory is page-locked on
// first use, so the copy is a direct DMA without a CPU copy. nullptr when CUDA is not
// usable or the grain is not standard v210 row layout.
std::shared_ptr<CudaFrame const> cudaUploadFrame(std::uint8_t const* v210, int v210RowBytes, std::uint8_t const* alpha10, int alphaRowBytes, int width,
    int height);

// The device copy of a new overlay version, made on the calling thread's stream so
// compose never waits for overlay copies: `previous` (the device copy of the version
// before) is copied on the device and only `changes` are uploaded; without `previous`
// the whole overlay is uploaded. nullptr when CUDA is not usable.
std::shared_ptr<CudaOverlay const> cudaUploadOverlay(std::shared_ptr<CudaOverlay const> const& previous, std::uint8_t const* rgba, int width, int height,
    CudaRect const* changes, int changeCount);

// Unlock the MXL grain memory the calling thread passed to the functions here. Call it
// before releasing that MXL reader or writer.
void cudaReleaseHostMemory();

// Scale, composite, blend, and pack on the device. `v210Out` receives one packed frame.
CudaComposeStatus cudaComposeFrame(CudaComposeDesc const& desc);
} // namespace mv
