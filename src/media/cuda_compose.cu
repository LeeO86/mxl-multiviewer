#include "media/cuda_compose.hpp"

#include <cuda_runtime.h>

#include <cstring>

namespace mv
{
namespace
{
constexpr int kMaxTiles = 128;

__device__ int clampIndex(int v, int limit)
{
    if (limit <= 1)
    {
        return 0;
    }
    if (v < 0)
    {
        return 0;
    }
    if (v >= limit)
    {
        return limit - 1;
    }
    return v;
}

__device__ unsigned short round10(float v)
{
    if (v < 0.f)
    {
        v = 0.f;
    }
    if (v > 1023.f)
    {
        v = 1023.f;
    }
    return static_cast<unsigned short>(v + 0.5f);
}

__device__ unsigned short bilerp(unsigned short const* plane, int stride, int width, int height, float x, float y)
{
    if (width <= 1)
    {
        x = 0.f;
    }
    else if (x < 0.f)
    {
        x = 0.f;
    }
    else if (x > static_cast<float>(width - 1))
    {
        x = static_cast<float>(width - 1);
    }
    if (height <= 1)
    {
        y = 0.f;
    }
    else if (y < 0.f)
    {
        y = 0.f;
    }
    else if (y > static_cast<float>(height - 1))
    {
        y = static_cast<float>(height - 1);
    }
    int const x0 = static_cast<int>(x);
    int const y0 = static_cast<int>(y);
    int const x1 = clampIndex(x0 + 1, width);
    int const y1 = clampIndex(y0 + 1, height);
    float const fx = x - static_cast<float>(x0);
    float const fy = y - static_cast<float>(y0);
    float const v00 = plane[y0 * stride + x0];
    float const v10 = plane[y0 * stride + x1];
    float const v01 = plane[y1 * stride + x0];
    float const v11 = plane[y1 * stride + x1];
    float const v0 = v00 + (v10 - v00) * fx;
    float const v1 = v01 + (v11 - v01) * fx;
    return round10(v0 + (v1 - v0) * fy);
}

__device__ unsigned short bilerpBob(unsigned short const* plane, int stride, int width, int height, float x, float y)
{
    if (width <= 1)
    {
        x = 0.f;
    }
    else if (x < 0.f)
    {
        x = 0.f;
    }
    else if (x > static_cast<float>(width - 1))
    {
        x = static_cast<float>(width - 1);
    }
    float const yLimit = static_cast<float>(height > 1 ? height - 1 : 0);
    if (y < 0.f)
    {
        y = 0.f;
    }
    else if (y > yLimit)
    {
        y = yLimit;
    }
    int y0 = static_cast<int>(y) & ~1;
    y0 = clampIndex(y0, height);
    int y1 = clampIndex(y0 + 2, height);
    if ((y1 & 1) != 0)
    {
        y1 = clampIndex(y1 - 1, height);
    }
    float fy = (y - static_cast<float>(y0)) * 0.5f;
    if (fy < 0.f)
    {
        fy = 0.f;
    }
    if (fy > 1.f)
    {
        fy = 1.f;
    }
    int const x0 = static_cast<int>(x);
    int const x1 = clampIndex(x0 + 1, width);
    float const fx = x - static_cast<float>(x0);
    float const v00 = plane[y0 * stride + x0];
    float const v10 = plane[y0 * stride + x1];
    float const v01 = plane[y1 * stride + x0];
    float const v11 = plane[y1 * stride + x1];
    float const v0 = v00 + (v10 - v00) * fx;
    float const v1 = v01 + (v11 - v01) * fx;
    return round10(v0 + (v1 - v0) * fy);
}

__device__ unsigned sample10(unsigned word, int shift)
{
    return (word >> shift) & 0x3ffu;
}

__global__ void fill422Kernel(unsigned short* y, unsigned short* cb, unsigned short* cr, int width, int height, unsigned short yv, unsigned short cbv, unsigned short crv)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y * blockDim.y + threadIdx.y;
    if (row >= height || x >= width)
    {
        return;
    }
    y[row * width + x] = yv;
    if ((x & 1) == 0 && x / 2 < width / 2)
    {
        cb[row * (width / 2) + x / 2] = cbv;
        cr[row * (width / 2) + x / 2] = crv;
    }
}

__global__ void unpackV210Kernel(std::uint8_t const* src, int rowBytes, int width, int height, unsigned short* y, unsigned short* cb, unsigned short* cr)
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
    unsigned const ys[6] = {sample10(w0, 10), sample10(w1, 0), sample10(w1, 20), sample10(w2, 10), sample10(w3, 0), sample10(w3, 20)};
    y[row * width + pixel] = static_cast<unsigned short>(ys[within]);
    if ((pixel & 1) == 0)
    {
        unsigned const cbs[3] = {sample10(w0, 0), sample10(w1, 10), sample10(w2, 20)};
        unsigned const crs[3] = {sample10(w0, 20), sample10(w2, 0), sample10(w3, 10)};
        int const c = pixel / 2;
        cb[row * (width / 2) + c] = static_cast<unsigned short>(cbs[within / 2]);
        cr[row * (width / 2) + c] = static_cast<unsigned short>(crs[within / 2]);
    }
}

__global__ void unpackAlphaKernel(std::uint8_t const* src, int rowBytes, int width, int height, unsigned short* a)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y;
    if (row >= height || x >= width)
    {
        return;
    }
    int const wordIndex = x / 3;
    int const shift = (x % 3) * 10;
    unsigned const word = *reinterpret_cast<unsigned const*>(src + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes) + static_cast<std::size_t>(wordIndex) * 4u);
    a[row * width + x] = static_cast<unsigned short>(sample10(word, shift));
}

struct DevScale
{
    unsigned short* planeY;
    unsigned short* planeCb;
    unsigned short* planeCr;
    int canvasW;
    int originX;
    int originY;
    int spanW;
    int spanH;
    float windowX;
    float windowY;
    float windowW;
    float windowH;
    unsigned short const* inY;
    unsigned short const* inCb;
    unsigned short const* inCr;
    unsigned short const* inA;
    int inW;
    int inH;
    int bob;
    int solid;
    unsigned short solidY;
    unsigned short solidCb;
    unsigned short solidCr;
};

__global__ void scaleTileKernel(DevScale args)
{
    int const dx = blockIdx.x * blockDim.x + threadIdx.x;
    int const dy = blockIdx.y * blockDim.y + threadIdx.y;
    if (dx >= args.spanW || dy >= args.spanH)
    {
        return;
    }
    int const x = args.originX + dx;
    int const y = args.originY + dy;
    if (x < 0 || y < 0 || x >= args.canvasW)
    {
        return;
    }
    int const cw = args.canvasW / 2;
    if (args.solid || args.inY == nullptr || args.inW <= 0 || args.inH <= 0 || args.spanW <= 0 || args.spanH <= 0)
    {
        args.planeY[y * args.canvasW + x] = args.solidY;
        if ((x & 1) == 0 && x / 2 < cw)
        {
            args.planeCb[y * cw + x / 2] = args.solidCb;
            args.planeCr[y * cw + x / 2] = args.solidCr;
        }
        return;
    }
    float const sy = args.windowY + (static_cast<float>(dy) + 0.5f) * args.windowH / static_cast<float>(args.spanH) - 0.5f;
    float const sx = args.windowX + (static_cast<float>(dx) + 0.5f) * args.windowW / static_cast<float>(args.spanW) - 0.5f;
    unsigned short ySample = args.bob ? bilerpBob(args.inY, args.inW, args.inW, args.inH, sx, sy) : bilerp(args.inY, args.inW, args.inW, args.inH, sx, sy);
    unsigned short alpha = 1023;
    if (args.inA != nullptr)
    {
        alpha = args.bob ? bilerpBob(args.inA, args.inW, args.inW, args.inH, sx, sy) : bilerp(args.inA, args.inW, args.inW, args.inH, sx, sy);
    }
    unsigned short& outY = args.planeY[y * args.canvasW + x];
    if (alpha >= 1023)
    {
        outY = ySample;
    }
    else if (alpha > 0)
    {
        outY = static_cast<unsigned short>((static_cast<int>(ySample) * alpha + static_cast<int>(outY) * (1023 - alpha) + 511) / 1023);
    }
    if ((x & 1) == 0 && x / 2 < cw)
    {
        int const scw = args.inW / 2;
        float const cx = args.windowX * 0.5f + (static_cast<float>(dx / 2) + 0.5f) * (args.windowW * 0.5f) / static_cast<float>(args.spanW > 1 ? args.spanW / 2 : 1) - 0.5f;
        unsigned short cb = args.bob ? bilerpBob(args.inCb, scw, scw, args.inH, cx, sy) : bilerp(args.inCb, scw, scw, args.inH, cx, sy);
        unsigned short cr = args.bob ? bilerpBob(args.inCr, scw, scw, args.inH, cx, sy) : bilerp(args.inCr, scw, scw, args.inH, cx, sy);
        if (alpha < 1023 && alpha > 0)
        {
            unsigned short& outCb = args.planeCb[y * cw + x / 2];
            unsigned short& outCr = args.planeCr[y * cw + x / 2];
            outCb = static_cast<unsigned short>((static_cast<int>(cb) * alpha + static_cast<int>(outCb) * (1023 - alpha) + 511) / 1023);
            outCr = static_cast<unsigned short>((static_cast<int>(cr) * alpha + static_cast<int>(outCr) * (1023 - alpha) + 511) / 1023);
        }
        else if (alpha >= 1023)
        {
            args.planeCb[y * cw + x / 2] = cb;
            args.planeCr[y * cw + x / 2] = cr;
        }
    }
}

__global__ void blendRgbaKernel(unsigned short* y, unsigned short* cb, unsigned short* cr, int width, int height, std::uint8_t const* rgba, int stride)
{
    int const x = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y * blockDim.y + threadIdx.y;
    if (row >= height || x >= width)
    {
        return;
    }
    std::uint8_t const* px = rgba + row * stride + x * 4;
    int const a = px[3];
    if (a == 0)
    {
        return;
    }
    int const r = px[0];
    int const g = px[1];
    int const b = px[2];
    float const y8 = 16.f + (65.481f * r + 128.553f * g + 24.966f * b) / 255.f;
    int y10 = static_cast<int>(y8 * 4.f + 0.5f);
    if (y10 < 0)
    {
        y10 = 0;
    }
    if (y10 > 1023)
    {
        y10 = 1023;
    }
    auto& dstY = y[row * width + x];
    dstY = static_cast<unsigned short>((y10 * a + dstY * (255 - a) + 127) / 255);
    if ((x & 1) == 0 && x / 2 < width / 2)
    {
        int r2 = r;
        int g2 = g;
        int b2 = b;
        int a2 = a;
        if (x + 1 < width)
        {
            std::uint8_t const* nx = px + 4;
            r2 = (r + nx[0]) / 2;
            g2 = (g + nx[1]) / 2;
            b2 = (b + nx[2]) / 2;
            a2 = (a + nx[3]) / 2;
        }
        if (a2 != 0)
        {
            float const cb8 = 128.f + (-37.797f * r2 - 74.203f * g2 + 112.f * b2) / 255.f;
            float const cr8 = 128.f + (112.f * r2 - 93.786f * g2 - 18.214f * b2) / 255.f;
            int cb10 = static_cast<int>(cb8 * 4.f + 0.5f);
            int cr10 = static_cast<int>(cr8 * 4.f + 0.5f);
            if (cb10 < 0)
            {
                cb10 = 0;
            }
            if (cb10 > 1023)
            {
                cb10 = 1023;
            }
            if (cr10 < 0)
            {
                cr10 = 0;
            }
            if (cr10 > 1023)
            {
                cr10 = 1023;
            }
            auto& dstCb = cb[row * (width / 2) + x / 2];
            auto& dstCr = cr[row * (width / 2) + x / 2];
            dstCb = static_cast<unsigned short>((cb10 * a2 + dstCb * (255 - a2) + 127) / 255);
            dstCr = static_cast<unsigned short>((cr10 * a2 + dstCr * (255 - a2) + 127) / 255);
        }
    }
}

__global__ void packV210Kernel(unsigned short const* y, unsigned short const* cb, unsigned short const* cr, int width, int height, std::uint8_t* dst, int rowBytes)
{
    int const group = blockIdx.x * blockDim.x + threadIdx.x;
    int const row = blockIdx.y;
    int const groups = (width + 5) / 6;
    if (row >= height || group >= groups)
    {
        return;
    }
    int const cw = width / 2;
    unsigned short yv[6] = {};
    unsigned short cbv[3] = {};
    unsigned short crv[3] = {};
    for (int i = 0; i < 6; ++i)
    {
        int const x = group * 6 + i;
        if (x < width)
        {
            yv[i] = y[row * width + x];
            if ((i % 2) == 0)
            {
                int const cx = x / 2;
                if (cx < cw)
                {
                    cbv[i / 2] = cb[row * cw + cx];
                    crv[i / 2] = cr[row * cw + cx];
                }
            }
        }
    }
    unsigned words[4];
    words[0] = (cbv[0] & 0x3ffu) | (static_cast<unsigned>(yv[0] & 0x3ffu) << 10) | (static_cast<unsigned>(crv[0] & 0x3ffu) << 20);
    words[1] = (yv[1] & 0x3ffu) | (static_cast<unsigned>(cbv[1] & 0x3ffu) << 10) | (static_cast<unsigned>(yv[2] & 0x3ffu) << 20);
    words[2] = (crv[1] & 0x3ffu) | (static_cast<unsigned>(yv[3] & 0x3ffu) << 10) | (static_cast<unsigned>(cbv[2] & 0x3ffu) << 20);
    words[3] = (yv[4] & 0x3ffu) | (static_cast<unsigned>(crv[2] & 0x3ffu) << 10) | (static_cast<unsigned>(yv[5] & 0x3ffu) << 20);
    unsigned* out = reinterpret_cast<unsigned*>(dst + row * rowBytes + group * 16);
    out[0] = words[0];
    out[1] = words[1];
    out[2] = words[2];
    out[3] = words[3];
}

struct Mem
{
    void* ptr = nullptr;
    std::size_t cap = 0;
    bool host = false;

    void* ensure(std::size_t bytes, bool pinned)
    {
        if (bytes == 0)
        {
            return ptr;
        }
        if (ptr != nullptr && bytes <= cap && host == pinned)
        {
            return ptr;
        }
        if (ptr != nullptr)
        {
            if (host)
            {
                cudaFreeHost(ptr);
            }
            else
            {
                cudaFree(ptr);
            }
            ptr = nullptr;
            cap = 0;
        }
        cudaError_t err = pinned ? cudaMallocHost(&ptr, bytes) : cudaMalloc(&ptr, bytes);
        if (err != cudaSuccess)
        {
            ptr = nullptr;
            cap = 0;
            return nullptr;
        }
        host = pinned;
        cap = bytes;
        return ptr;
    }
};

struct Slot
{
    Mem y;
    Mem cb;
    Mem cr;
    Mem a;
    Mem packed;
    Mem alpha;
};

struct Session
{
    bool ready = false;
    cudaStream_t copy = nullptr;
    cudaStream_t compute = nullptr;
    cudaEvent_t uploaded[2] = {};
    cudaEvent_t computed[2] = {};
    Mem canvasY;
    Mem canvasCb;
    Mem canvasCr;
    Mem rgba;
    Mem v210;
    Mem pin[2];
    Mem outPin;
    Slot slot[2];
    bool used[2] = {};

    bool open()
    {
        if (ready)
        {
            return true;
        }
        if (cudaSetDevice(0) != cudaSuccess)
        {
            return false;
        }
        if (cudaStreamCreate(&copy) != cudaSuccess || cudaStreamCreate(&compute) != cudaSuccess)
        {
            return false;
        }
        for (int i = 0; i < 2; ++i)
        {
            if (cudaEventCreateWithFlags(&uploaded[i], cudaEventDisableTiming) != cudaSuccess ||
                cudaEventCreateWithFlags(&computed[i], cudaEventDisableTiming) != cudaSuccess)
            {
                return false;
            }
        }
        ready = true;
        return true;
    }
};

Session& session()
{
    thread_local Session value;
    return value;
}

bool upload(Session& gpu, int pinIndex, void* device, void const* host, std::size_t bytes)
{
    if (bytes == 0)
    {
        return true;
    }
    if (cudaStreamSynchronize(gpu.copy) != cudaSuccess)
    {
        return false;
    }
    void* pin = gpu.pin[pinIndex].ensure(bytes, true);
    if (pin == nullptr || device == nullptr)
    {
        return false;
    }
    std::memcpy(pin, host, bytes);
    return cudaMemcpyAsync(device, pin, bytes, cudaMemcpyHostToDevice, gpu.copy) == cudaSuccess;
}

dim3 tiles2d(int w, int h)
{
    return dim3((w + 15) / 16, (h + 15) / 16);
}

bool launchScale(Session& gpu, DevScale args)
{
    if (args.spanW <= 0 || args.spanH <= 0)
    {
        return true;
    }
    scaleTileKernel<<<tiles2d(args.spanW, args.spanH), dim3(16, 16), 0, gpu.compute>>>(args);
    return cudaGetLastError() == cudaSuccess;
}
} // namespace

bool cudaSupportCompiled()
{
    return true;
}

int cudaDeviceCount()
{
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess)
    {
        return 0;
    }
    return count;
}

bool cudaRuntimeAvailable()
{
    return cudaDeviceCount() > 0;
}

void cudaDeviceMemory(std::uint64_t* freeBytes, std::uint64_t* totalBytes)
{
    std::size_t freeB = 0;
    std::size_t totalB = 0;
    if (cudaMemGetInfo(&freeB, &totalB) != cudaSuccess)
    {
        freeB = 0;
        totalB = 0;
    }
    if (freeBytes != nullptr)
    {
        *freeBytes = freeB;
    }
    if (totalBytes != nullptr)
    {
        *totalBytes = totalB;
    }
}

CudaComposeStatus cudaComposeFrame(CudaComposeDesc const& desc)
{
    if (!cudaRuntimeAvailable())
    {
        return CudaComposeStatus::Unavailable;
    }
    if (desc.width < 2 || desc.height < 2 || (desc.width & 1) != 0 || desc.v210Out == nullptr || desc.v210RowBytes < 16 || desc.tileCount < 0 ||
        desc.tileCount > kMaxTiles)
    {
        return CudaComposeStatus::Failed;
    }
    auto& gpu = session();
    if (!gpu.open())
    {
        return CudaComposeStatus::Failed;
    }
    int const cw = desc.width / 2;
    std::size_t const yBytes = static_cast<std::size_t>(desc.width) * static_cast<std::size_t>(desc.height) * sizeof(unsigned short);
    std::size_t const cBytes = static_cast<std::size_t>(cw) * static_cast<std::size_t>(desc.height) * sizeof(unsigned short);
    auto* dstY = static_cast<unsigned short*>(gpu.canvasY.ensure(yBytes, false));
    auto* dstCb = static_cast<unsigned short*>(gpu.canvasCb.ensure(cBytes, false));
    auto* dstCr = static_cast<unsigned short*>(gpu.canvasCr.ensure(cBytes, false));
    if (dstY == nullptr || dstCb == nullptr || dstCr == nullptr)
    {
        return CudaComposeStatus::Failed;
    }
    bool const fullBackground = desc.backgroundY != nullptr && desc.backgroundCb != nullptr && desc.backgroundCr != nullptr && desc.backgroundWidth == desc.width &&
                                 desc.backgroundHeight == desc.height;
    if (fullBackground)
    {
        if (!upload(gpu, 0, dstY, desc.backgroundY, yBytes) || !upload(gpu, 0, dstCb, desc.backgroundCb, cBytes) || !upload(gpu, 0, dstCr, desc.backgroundCr, cBytes))
        {
            return CudaComposeStatus::Failed;
        }
        if (cudaEventRecord(gpu.uploaded[0], gpu.copy) != cudaSuccess || cudaStreamWaitEvent(gpu.compute, gpu.uploaded[0], 0) != cudaSuccess)
        {
            return CudaComposeStatus::Failed;
        }
    }
    else
    {
        fill422Kernel<<<tiles2d(desc.width, desc.height), dim3(16, 16), 0, gpu.compute>>>(dstY, dstCb, dstCr, desc.width, desc.height, desc.bgY, desc.bgCb, desc.bgCr);
        if (cudaGetLastError() != cudaSuccess)
        {
            return CudaComposeStatus::Failed;
        }
    }
    gpu.used[0] = false;
    gpu.used[1] = false;
    for (int i = 0; i < desc.tileCount; ++i)
    {
        auto const& tile = desc.tiles[i];
        int const slot = i & 1;
        DevScale args{};
        args.planeY = dstY;
        args.planeCb = dstCb;
        args.planeCr = dstCr;
        args.canvasW = desc.width;
        args.originX = tile.dstX;
        args.originY = tile.dstY;
        args.spanW = tile.dstW;
        args.spanH = tile.dstH;
        args.windowX = tile.srcX;
        args.windowY = tile.srcY;
        args.windowW = tile.srcW;
        args.windowH = tile.srcH;
        args.inW = tile.srcWidth;
        args.inH = tile.srcHeight;
        args.bob = tile.bob ? 1 : 0;
        args.solid = tile.solid || (tile.y == nullptr && tile.v210 == nullptr) ? 1 : 0;
        args.solidY = desc.bgY;
        args.solidCb = desc.bgCb;
        args.solidCr = desc.bgCr;
        if (args.solid)
        {
            if (!launchScale(gpu, args))
            {
                return CudaComposeStatus::Failed;
            }
            continue;
        }
        if (gpu.used[slot])
        {
            if (cudaStreamWaitEvent(gpu.copy, gpu.computed[slot], 0) != cudaSuccess)
            {
                return CudaComposeStatus::Failed;
            }
        }
        std::size_t const srcYBytes = static_cast<std::size_t>(tile.srcWidth) * static_cast<std::size_t>(tile.srcHeight) * sizeof(unsigned short);
        std::size_t const srcCBytes = static_cast<std::size_t>(tile.srcWidth / 2) * static_cast<std::size_t>(tile.srcHeight) * sizeof(unsigned short);
        auto* srcY = static_cast<unsigned short*>(gpu.slot[slot].y.ensure(srcYBytes, false));
        auto* srcCb = static_cast<unsigned short*>(gpu.slot[slot].cb.ensure(srcCBytes, false));
        auto* srcCr = static_cast<unsigned short*>(gpu.slot[slot].cr.ensure(srcCBytes, false));
        if (srcY == nullptr || srcCb == nullptr || srcCr == nullptr || (tile.srcWidth & 1) != 0)
        {
            return CudaComposeStatus::Failed;
        }
        if (tile.v210 != nullptr && tile.v210RowBytes > 0)
        {
            std::size_t const packedBytes = static_cast<std::size_t>(tile.v210RowBytes) * static_cast<std::size_t>(tile.srcHeight);
            auto* packed = gpu.slot[slot].packed.ensure(packedBytes, false);
            if (packed == nullptr || !upload(gpu, slot, packed, tile.v210, packedBytes))
            {
                return CudaComposeStatus::Failed;
            }
            if (cudaEventRecord(gpu.uploaded[slot], gpu.copy) != cudaSuccess || cudaStreamWaitEvent(gpu.compute, gpu.uploaded[slot], 0) != cudaSuccess)
            {
                return CudaComposeStatus::Failed;
            }
            dim3 const grid((tile.srcWidth + 255) / 256, tile.srcHeight);
            unpackV210Kernel<<<grid, 256, 0, gpu.compute>>>(static_cast<std::uint8_t*>(packed), tile.v210RowBytes, tile.srcWidth, tile.srcHeight, srcY, srcCb, srcCr);
            if (tile.alpha10 != nullptr && tile.alphaRowBytes > 0)
            {
                std::size_t const alphaBytes = static_cast<std::size_t>(tile.alphaRowBytes) * static_cast<std::size_t>(tile.srcHeight);
                auto* alphaPacked = gpu.slot[slot].alpha.ensure(alphaBytes, false);
                auto* alphaPlane = static_cast<unsigned short*>(gpu.slot[slot].a.ensure(srcYBytes, false));
                if (alphaPacked == nullptr || alphaPlane == nullptr || !upload(gpu, slot, alphaPacked, tile.alpha10, alphaBytes))
                {
                    return CudaComposeStatus::Failed;
                }
                if (cudaEventRecord(gpu.uploaded[slot], gpu.copy) != cudaSuccess || cudaStreamWaitEvent(gpu.compute, gpu.uploaded[slot], 0) != cudaSuccess)
                {
                    return CudaComposeStatus::Failed;
                }
                unpackAlphaKernel<<<grid, 256, 0, gpu.compute>>>(static_cast<std::uint8_t*>(alphaPacked), tile.alphaRowBytes, tile.srcWidth, tile.srcHeight, alphaPlane);
                args.inA = alphaPlane;
            }
        }
        else
        {
            if (tile.y == nullptr || tile.cb == nullptr || tile.cr == nullptr)
            {
                return CudaComposeStatus::Failed;
            }
            if (!upload(gpu, slot, srcY, tile.y, srcYBytes) || !upload(gpu, slot, srcCb, tile.cb, srcCBytes) || !upload(gpu, slot, srcCr, tile.cr, srcCBytes))
            {
                return CudaComposeStatus::Failed;
            }
            unsigned short* alphaPlane = nullptr;
            if (tile.a != nullptr)
            {
                alphaPlane = static_cast<unsigned short*>(gpu.slot[slot].a.ensure(srcYBytes, false));
                if (alphaPlane == nullptr || !upload(gpu, slot, alphaPlane, tile.a, srcYBytes))
                {
                    return CudaComposeStatus::Failed;
                }
                args.inA = alphaPlane;
            }
            if (cudaEventRecord(gpu.uploaded[slot], gpu.copy) != cudaSuccess || cudaStreamWaitEvent(gpu.compute, gpu.uploaded[slot], 0) != cudaSuccess)
            {
                return CudaComposeStatus::Failed;
            }
        }
        args.inY = srcY;
        args.inCb = srcCb;
        args.inCr = srcCr;
        if (!launchScale(gpu, args))
        {
            return CudaComposeStatus::Failed;
        }
        if (cudaEventRecord(gpu.computed[slot], gpu.compute) != cudaSuccess)
        {
            return CudaComposeStatus::Failed;
        }
        gpu.used[slot] = true;
    }
    if (desc.rgba != nullptr && desc.rgbaStride >= desc.width * 4)
    {
        std::size_t const rgbaBytes = static_cast<std::size_t>(desc.rgbaStride) * static_cast<std::size_t>(desc.height);
        auto* deviceRgba = gpu.rgba.ensure(rgbaBytes, false);
        if (deviceRgba == nullptr || !upload(gpu, 0, deviceRgba, desc.rgba, rgbaBytes))
        {
            return CudaComposeStatus::Failed;
        }
        if (cudaEventRecord(gpu.uploaded[0], gpu.copy) != cudaSuccess || cudaStreamWaitEvent(gpu.compute, gpu.uploaded[0], 0) != cudaSuccess)
        {
            return CudaComposeStatus::Failed;
        }
        blendRgbaKernel<<<tiles2d(desc.width, desc.height), dim3(16, 16), 0, gpu.compute>>>(dstY, dstCb, dstCr, desc.width, desc.height,
            static_cast<std::uint8_t*>(deviceRgba), desc.rgbaStride);
        if (cudaGetLastError() != cudaSuccess)
        {
            return CudaComposeStatus::Failed;
        }
    }
    std::size_t const outBytes = static_cast<std::size_t>(desc.v210RowBytes) * static_cast<std::size_t>(desc.height);
    auto* deviceV210 = gpu.v210.ensure(outBytes, false);
    void* outPin = gpu.outPin.ensure(outBytes, true);
    if (deviceV210 == nullptr || outPin == nullptr)
    {
        return CudaComposeStatus::Failed;
    }
    if (cudaMemsetAsync(deviceV210, 0, outBytes, gpu.compute) != cudaSuccess)
    {
        return CudaComposeStatus::Failed;
    }
    int const groups = (desc.width + 5) / 6;
    packV210Kernel<<<dim3((groups + 255) / 256, desc.height), 256, 0, gpu.compute>>>(dstY, dstCb, dstCr, desc.width, desc.height, static_cast<std::uint8_t*>(deviceV210),
        desc.v210RowBytes);
    if (cudaGetLastError() != cudaSuccess)
    {
        return CudaComposeStatus::Failed;
    }
    if (cudaEventRecord(gpu.computed[0], gpu.compute) != cudaSuccess || cudaStreamWaitEvent(gpu.copy, gpu.computed[0], 0) != cudaSuccess)
    {
        return CudaComposeStatus::Failed;
    }
    if (cudaMemcpyAsync(outPin, deviceV210, outBytes, cudaMemcpyDeviceToHost, gpu.copy) != cudaSuccess)
    {
        return CudaComposeStatus::Failed;
    }
    if (cudaStreamSynchronize(gpu.copy) != cudaSuccess || cudaStreamSynchronize(gpu.compute) != cudaSuccess)
    {
        return CudaComposeStatus::Failed;
    }
    std::memcpy(desc.v210Out, outPin, outBytes);
    return CudaComposeStatus::Ok;
}
} // namespace mv
