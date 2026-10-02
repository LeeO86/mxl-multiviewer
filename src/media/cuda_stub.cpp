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

CudaComposeStatus cudaComposeFrame(CudaComposeDesc const&)
{
    return CudaComposeStatus::Unavailable;
}
} // namespace mv
