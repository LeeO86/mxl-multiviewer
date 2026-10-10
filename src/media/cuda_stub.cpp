#include "media/cuda_compose.hpp"

namespace mv
{
bool cudaSupportCompiled()
{
    return false;
}

int cudaDeviceCount()
{
    return 0;
}

bool cudaRuntimeAvailable()
{
    return false;
}

void cudaDeviceMemory(std::uint64_t* freeBytes, std::uint64_t* totalBytes)
{
    if (freeBytes != nullptr)
    {
        *freeBytes = 0;
    }
    if (totalBytes != nullptr)
    {
        *totalBytes = 0;
    }
}

std::shared_ptr<CudaFrame const> cudaUploadFrame(std::uint8_t const*, int, std::uint8_t const*, int, int, int)
{
    return nullptr;
}

std::shared_ptr<CudaOverlay const> cudaUploadOverlay(std::shared_ptr<CudaOverlay const> const&, std::uint8_t const*, int, int, CudaRect const*, int)
{
    return nullptr;
}

void cudaReleaseHostMemory()
{
}

CudaComposeStatus cudaComposeFrame(CudaComposeDesc const&)
{
    return CudaComposeStatus::Unavailable;
}

bool cudaPreviewTile(int, int, std::uint8_t*, float*)
{
    return false;
}

bool cudaPreviewContext(void**, void**)
{
    return false;
}
} // namespace mv
