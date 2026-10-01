// CUDA compose path. Built only when the CUDA toolkit is present.
#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace
{
__device__ unsigned sample10(unsigned word, int shift)
{
    return (word >> shift) & 0x3ffu;
}

__global__ void unpackV210Kernel(std::uint8_t const* src, int rowBytes, int width, int height, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr)
{
    int const pixel = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y;
    if (row >= height || pixel >= width)
    {
        return;
    }
    int const group = pixel / 6;
    int const within = pixel % 6;
    unsigned const* words = reinterpret_cast<unsigned const*>(src + row * rowBytes + group * 16);
    unsigned const w0 = words[0];
    unsigned const w1 = words[1];
    unsigned const w2 = words[2];
    unsigned const w3 = words[3];
    unsigned ys[6] = {sample10(w0, 10), sample10(w1, 0), sample10(w1, 20), sample10(w2, 10), sample10(w3, 0), sample10(w3, 20)};
    y[row * width + pixel] = static_cast<std::uint16_t>(ys[within]);
    if ((pixel & 1) == 0)
    {
        unsigned cbs[3] = {sample10(w0, 0), sample10(w1, 10), sample10(w2, 20)};
        unsigned crs[3] = {sample10(w0, 20), sample10(w2, 0), sample10(w3, 10)};
        int const c = pixel / 2;
        cb[row * (width / 2) + c] = static_cast<std::uint16_t>(cbs[within / 2]);
        cr[row * (width / 2) + c] = static_cast<std::uint16_t>(crs[within / 2]);
    }
}
} // namespace

extern "C" int mvCudaDeviceCount()
{
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess)
    {
        return 0;
    }
    return count;
}

extern "C" int mvCudaComposeAvailable()
{
    return mvCudaDeviceCount() > 0;
}
