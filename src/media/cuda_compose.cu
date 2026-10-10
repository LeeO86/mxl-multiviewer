#include "media/cuda_compose.hpp"
#include "media/frame.hpp"
#include "media/nv12scale.hpp"

#include <cuda.h>
#include <cuda_runtime.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mv
{
// The grain as it is in MXL (packed v210, optional packed 10-bit key). The scaler reads
// the samples it needs in place, so a 4×4 tile touches a sixteenth of the frame instead
// of every input being unpacked in full.
struct CudaFrame
{
    int width = 0;
    int height = 0;
    bool alpha = false;
    std::uint8_t* v210 = nullptr;
    int v210RowBytes = 0;
    std::uint8_t* alpha10 = nullptr;
    int alphaRowBytes = 0;
    // Recorded after the upload; compose waits on it.
    cudaEvent_t ready = nullptr;
};

struct CudaOverlay
{
    int width = 0;
    int height = 0;
    std::uint8_t* rgba = nullptr;
    cudaEvent_t ready = nullptr;
};

namespace
{
constexpr int kMaxTiles = 128;

enum PlaneKind
{
    kSamples,
    kV210Luma,
    kV210Cb,
    kV210Cr,
    kAlpha10
};

// One 10-bit plane for the scaler: 16-bit samples, or read in place from packed v210
// or packed 10-bit alpha.
struct Plane
{
    unsigned short const* samples = nullptr;
    std::uint8_t const* packed = nullptr;
    int stride = 0;
    int kind = kSamples;

    __host__ __device__ bool valid() const
    {
        return samples != nullptr || packed != nullptr;
    }

    __device__ unsigned short at(int x, int y) const
    {
        if (kind == kSamples)
        {
            return samples[y * stride + x];
        }
        auto const* words = reinterpret_cast<unsigned const*>(packed + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride));
        int word = 0;
        int shift = 0;
        if (kind == kV210Luma)
        {
            // Y0..Y5 of a 6-pixel group: word 0 bits 10, word 1 bits 0 and 20, word 2 bits 10, word 3 bits 0 and 20.
            int const i = x % 6;
            word = (x / 6) * 4 + (i == 0 ? 0 : i <= 2 ? 1 : i == 3 ? 2 : 3);
            shift = (i == 1 || i == 4) ? 0 : (i == 0 || i == 3) ? 10 : 20;
        }
        else if (kind == kV210Cb)
        {
            int const j = x % 3;
            word = (x / 3) * 4 + j;
            shift = j * 10;
        }
        else if (kind == kV210Cr)
        {
            int const j = x % 3;
            word = (x / 3) * 4 + (j == 0 ? 0 : j + 1);
            shift = j == 0 ? 20 : j == 1 ? 0 : 10;
        }
        else
        {
            word = x / 3;
            shift = (x % 3) * 10;
        }
        return static_cast<unsigned short>((words[word] >> shift) & 0x3ffu);
    }
};

Plane samplesPlane(unsigned short const* samples, int stride)
{
    Plane plane;
    plane.samples = samples;
    plane.stride = stride;
    return plane;
}

Plane packedPlane(std::uint8_t const* packed, int rowBytes, int kind)
{
    Plane plane;
    plane.packed = packed;
    plane.stride = rowBytes;
    plane.kind = kind;
    return plane;
}

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

__device__ unsigned short bilerp(Plane const& plane, int width, int height, float x, float y)
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
    float const v00 = plane.at(x0, y0);
    float const v10 = plane.at(x1, y0);
    float const v01 = plane.at(x0, y1);
    float const v11 = plane.at(x1, y1);
    float const v0 = v00 + (v10 - v00) * fx;
    float const v1 = v01 + (v11 - v01) * fx;
    return round10(v0 + (v1 - v0) * fy);
}

__device__ unsigned short bilerpBob(Plane const& plane, int width, int height, float x, float y)
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
    float const v00 = plane.at(x0, y0);
    float const v10 = plane.at(x1, y0);
    float const v01 = plane.at(x0, y1);
    float const v11 = plane.at(x1, y1);
    float const v0 = v00 + (v10 - v00) * fx;
    float const v1 = v01 + (v11 - v01) * fx;
    return round10(v0 + (v1 - v0) * fy);
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

// One thread per 2×2 block of the WebRTC preview tile (§8.4), from the composed canvas.
__global__ void nv12TileKernel(unsigned short const* y, unsigned short const* cb, unsigned short const* cr, int srcW, int srcH, std::uint8_t* dst, int dstW,
    int dstH)
{
    int const bx = blockIdx.x * blockDim.x + threadIdx.x;
    int const by = blockIdx.y * blockDim.y + threadIdx.y;
    if (bx >= dstW / 2 || by >= dstH / 2)
    {
        return;
    }
    nv12::block(y, cb, cr, srcW, srcH, dstW, dstH, bx, by, dst, dstW, dst + static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH), dstW);
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
    Plane inY;
    Plane inCb;
    Plane inCr;
    Plane inA;
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
    if (args.solid || !args.inY.valid() || args.inW <= 0 || args.inH <= 0 || args.spanW <= 0 || args.spanH <= 0)
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
    unsigned short ySample = args.bob ? bilerpBob(args.inY, args.inW, args.inH, sx, sy) : bilerp(args.inY, args.inW, args.inH, sx, sy);
    unsigned short alpha = 1023;
    if (args.inA.valid())
    {
        alpha = args.bob ? bilerpBob(args.inA, args.inW, args.inH, sx, sy) : bilerp(args.inA, args.inW, args.inH, sx, sy);
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
        unsigned short cb = args.bob ? bilerpBob(args.inCb, scw, args.inH, cx, sy) : bilerp(args.inCb, scw, args.inH, cx, sy);
        unsigned short cr = args.bob ? bilerpBob(args.inCr, scw, args.inH, cx, sy) : bilerp(args.inCr, scw, args.inH, cx, sy);
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

bool readOnlyLockSupported()
{
    static int const supported = [] {
        int device = 0;
        int value = 0;
        if (cudaGetDevice(&device) != cudaSuccess || cudaDeviceGetAttribute(&value, cudaDevAttrHostRegisterReadOnlySupported, device) != cudaSuccess)
        {
            cudaGetLastError();
            return 0;
        }
        return value;
    }();
    return supported != 0;
}

// MXL grain memory this thread page-locked. A copy from or to locked memory is a
// direct DMA; pageable memory goes through a CPU copy first. Each grain is its own
// mapping that lives as long as the reader or writer, so it is locked once.
struct HostMemory
{
    std::unordered_map<void const*, std::size_t> locked;
    std::unordered_set<void const*> shared;
    std::unordered_set<void const*> refused;

    HostMemory() = default;
    HostMemory(HostMemory const&) = delete;
    HostMemory& operator=(HostMemory const&) = delete;

    ~HostMemory()
    {
        release();
    }

    bool ensure(void const* ptr, std::size_t bytes, bool readOnly)
    {
        auto const it = locked.find(ptr);
        if (it != locked.end() && it->second >= bytes)
        {
            return true;
        }
        if (shared.count(ptr) != 0)
        {
            return true;
        }
        if (refused.count(ptr) != 0)
        {
            return false;
        }
        if (it != locked.end())
        {
            cudaHostUnregister(const_cast<void*>(ptr));
            locked.erase(it);
        }
        unsigned flags = cudaHostRegisterPortable;
        if (readOnly && readOnlyLockSupported())
        {
            flags |= cudaHostRegisterReadOnly;
        }
        cudaError_t const err = cudaHostRegister(const_cast<void*>(ptr), bytes, flags);
        if (err == cudaSuccess)
        {
            locked.emplace(ptr, bytes);
            return true;
        }
        cudaGetLastError();
        if (err == cudaErrorHostMemoryAlreadyRegistered)
        {
            // Locked by another thread; that thread also unlocks it.
            shared.insert(ptr);
            return true;
        }
        refused.insert(ptr);
        return false;
    }

    void release()
    {
        for (auto const& entry : locked)
        {
            cudaHostUnregister(const_cast<void*>(entry.first));
        }
        locked.clear();
        shared.clear();
        refused.clear();
    }
};

HostMemory& hostMemory()
{
    thread_local HostMemory value;
    return value;
}

// Device frames are recycled: cudaMalloc per grain would serialise the device.
class FramePool
{
public:
    std::shared_ptr<CudaFrame const> acquire(int width, int height, bool alpha, cudaStream_t stream)
    {
        CudaFrame* frame = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto it = free_.begin(); it != free_.end(); ++it)
            {
                if ((*it)->width == width && (*it)->height == height && (*it)->alpha == alpha)
                {
                    frame = *it;
                    free_.erase(it);
                    break;
                }
            }
        }
        if (frame != nullptr)
        {
            // The upload that last wrote this frame may still be queued on another stream.
            if (cudaStreamWaitEvent(stream, frame->ready, 0) != cudaSuccess)
            {
                destroy(frame);
                return nullptr;
            }
        }
        else
        {
            frame = create(width, height, alpha);
            if (frame == nullptr)
            {
                return nullptr;
            }
        }
        return std::shared_ptr<CudaFrame const>(frame, [this](CudaFrame const* done) { recycle(const_cast<CudaFrame*>(done)); });
    }

private:
    static constexpr std::size_t kMaxFree = 64;
    std::mutex mu_;
    std::vector<CudaFrame*> free_;

    static CudaFrame* create(int width, int height, bool alpha)
    {
        auto* frame = new CudaFrame;
        frame->width = width;
        frame->height = height;
        frame->alpha = alpha;
        frame->v210RowBytes = static_cast<int>(v210RowBytes(width));
        frame->alphaRowBytes = alpha ? static_cast<int>(alpha10RowBytes(width)) : 0;
        std::size_t const fill = static_cast<std::size_t>(frame->v210RowBytes) * static_cast<std::size_t>(height);
        std::size_t const key = static_cast<std::size_t>(frame->alphaRowBytes) * static_cast<std::size_t>(height);
        void* base = nullptr;
        if (cudaMalloc(&base, fill + key) != cudaSuccess || cudaEventCreateWithFlags(&frame->ready, cudaEventDisableTiming) != cudaSuccess)
        {
            cudaGetLastError();
            if (base != nullptr)
            {
                cudaFree(base);
            }
            delete frame;
            return nullptr;
        }
        frame->v210 = static_cast<std::uint8_t*>(base);
        frame->alpha10 = alpha ? frame->v210 + fill : nullptr;
        return frame;
    }

    static void destroy(CudaFrame* frame)
    {
        cudaFree(frame->v210);
        cudaEventDestroy(frame->ready);
        delete frame;
    }

    void recycle(CudaFrame* frame)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (free_.size() < kMaxFree)
            {
                free_.push_back(frame);
                return;
            }
        }
        destroy(frame);
    }
};

FramePool& framePool()
{
    // Never destroyed: frames can come back while static destruction runs.
    static auto* pool = new FramePool;
    return *pool;
}

// Device overlays are recycled like frames; a few per head are alive at a time.
class OverlayPool
{
public:
    std::shared_ptr<CudaOverlay const> acquire(int width, int height, cudaStream_t stream)
    {
        CudaOverlay* overlay = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto it = free_.begin(); it != free_.end(); ++it)
            {
                if ((*it)->width == width && (*it)->height == height)
                {
                    overlay = *it;
                    free_.erase(it);
                    break;
                }
            }
        }
        if (overlay != nullptr)
        {
            if (cudaStreamWaitEvent(stream, overlay->ready, 0) != cudaSuccess)
            {
                destroy(overlay);
                return nullptr;
            }
        }
        else
        {
            overlay = new CudaOverlay;
            overlay->width = width;
            overlay->height = height;
            if (cudaMalloc(reinterpret_cast<void**>(&overlay->rgba), static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u) != cudaSuccess ||
                cudaEventCreateWithFlags(&overlay->ready, cudaEventDisableTiming) != cudaSuccess)
            {
                cudaGetLastError();
                destroy(overlay);
                return nullptr;
            }
        }
        return std::shared_ptr<CudaOverlay const>(overlay, [this](CudaOverlay const* done) { recycle(const_cast<CudaOverlay*>(done)); });
    }

private:
    static constexpr std::size_t kMaxFree = 8;
    std::mutex mu_;
    std::vector<CudaOverlay*> free_;

    static void destroy(CudaOverlay* overlay)
    {
        if (overlay->rgba != nullptr)
        {
            cudaFree(overlay->rgba);
        }
        if (overlay->ready != nullptr)
        {
            cudaEventDestroy(overlay->ready);
        }
        delete overlay;
    }

    void recycle(CudaOverlay* overlay)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (free_.size() < kMaxFree)
            {
                free_.push_back(overlay);
                return;
            }
        }
        destroy(overlay);
    }
};

OverlayPool& overlayPool()
{
    static auto* pool = new OverlayPool;
    return *pool;
}

// Host to device on `stream`: straight from locked memory, else through a pinned
// staging buffer (the CPU copy first waits for the previous use of that buffer).
bool copyToDevice(cudaStream_t stream, void* device, void const* host, std::size_t bytes, Mem& staging)
{
    if (hostMemory().ensure(host, bytes, true))
    {
        return cudaMemcpyAsync(device, host, bytes, cudaMemcpyHostToDevice, stream) == cudaSuccess;
    }
    if (cudaStreamSynchronize(stream) != cudaSuccess)
    {
        return false;
    }
    void* pin = staging.ensure(bytes, true);
    if (pin == nullptr)
    {
        return false;
    }
    std::memcpy(pin, host, bytes);
    return cudaMemcpyAsync(device, pin, bytes, cudaMemcpyHostToDevice, stream) == cudaSuccess;
}

// One per input thread: its grains upload on its own stream, so inputs
// copy in parallel and never wait for compose.
struct Uploader
{
    bool ready = false;
    cudaStream_t stream = nullptr;
    Mem staging;
    Mem stagingAlpha;

    bool open()
    {
        if (ready)
        {
            return true;
        }
        if (cudaSetDevice(0) != cudaSuccess || cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess)
        {
            return false;
        }
        ready = true;
        return true;
    }
};

Uploader& uploader()
{
    thread_local Uploader value;
    return value;
}

struct Slot
{
    Mem y;
    Mem cb;
    Mem cr;
    Mem a;
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
    Mem backgroundY;
    Mem backgroundCb;
    Mem backgroundCr;
    std::uint64_t backgroundVersion = 0;
    Mem rgba;
    std::uint64_t rgbaVersion = 0;
    Mem v210;
    Mem pin[2];
    Mem outPin;
    Slot slot[2];
    bool used[2] = {};
    // Stage boundaries for CudaComposeTiming: start, background, tiles, overlay, pack, download.
    cudaEvent_t mark[6] = {};
    // The canvas of the last composed frame stays on the device for the WebRTC preview tile.
    int lastWidth = 0;
    int lastHeight = 0;
    Mem previewTile;
    Mem previewPin;

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
        for (auto& event : mark)
        {
            if (cudaEventCreate(&event) != cudaSuccess)
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
    cudaEventRecord(gpu.mark[0], gpu.compute);
    if (fullBackground)
    {
        void const* before = gpu.backgroundY.ptr;
        auto* bgY = gpu.backgroundY.ensure(yBytes, false);
        auto* bgCb = gpu.backgroundCb.ensure(cBytes, false);
        auto* bgCr = gpu.backgroundCr.ensure(cBytes, false);
        if (bgY == nullptr || bgCb == nullptr || bgCr == nullptr)
        {
            return CudaComposeStatus::Failed;
        }
        // The background only changes with the configuration: keep it on the device.
        if (desc.backgroundVersion == 0 || desc.backgroundVersion != gpu.backgroundVersion || bgY != before)
        {
            if (!upload(gpu, 0, bgY, desc.backgroundY, yBytes) || !upload(gpu, 0, bgCb, desc.backgroundCb, cBytes) || !upload(gpu, 0, bgCr, desc.backgroundCr, cBytes))
            {
                return CudaComposeStatus::Failed;
            }
            if (cudaEventRecord(gpu.uploaded[0], gpu.copy) != cudaSuccess || cudaStreamWaitEvent(gpu.compute, gpu.uploaded[0], 0) != cudaSuccess)
            {
                return CudaComposeStatus::Failed;
            }
            gpu.backgroundVersion = desc.backgroundVersion;
        }
        if (cudaMemcpyAsync(dstY, bgY, yBytes, cudaMemcpyDeviceToDevice, gpu.compute) != cudaSuccess ||
            cudaMemcpyAsync(dstCb, bgCb, cBytes, cudaMemcpyDeviceToDevice, gpu.compute) != cudaSuccess ||
            cudaMemcpyAsync(dstCr, bgCr, cBytes, cudaMemcpyDeviceToDevice, gpu.compute) != cudaSuccess)
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
    cudaEventRecord(gpu.mark[1], gpu.compute);
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
        args.solid = tile.solid || (tile.y == nullptr && tile.frame == nullptr) ? 1 : 0;
        args.solidY = desc.solidY;
        args.solidCb = desc.solidCb;
        args.solidCr = desc.solidCr;
        if (args.solid)
        {
            if (!launchScale(gpu, args))
            {
                return CudaComposeStatus::Failed;
            }
            continue;
        }
        if (tile.frame != nullptr)
        {
            // Already on the device: wait for its upload on the GPU, not on the CPU.
            if (cudaStreamWaitEvent(gpu.compute, tile.frame->ready, 0) != cudaSuccess)
            {
                return CudaComposeStatus::Failed;
            }
            args.inY = packedPlane(tile.frame->v210, tile.frame->v210RowBytes, kV210Luma);
            args.inCb = packedPlane(tile.frame->v210, tile.frame->v210RowBytes, kV210Cb);
            args.inCr = packedPlane(tile.frame->v210, tile.frame->v210RowBytes, kV210Cr);
            if (tile.frame->alpha)
            {
                args.inA = packedPlane(tile.frame->alpha10, tile.frame->alphaRowBytes, kAlpha10);
            }
            args.inW = tile.frame->width;
            args.inH = tile.frame->height;
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
        // A host frame (the CPU unpacked it because its upload failed): copy the planes.
        if (tile.cb == nullptr || tile.cr == nullptr)
        {
            return CudaComposeStatus::Failed;
        }
        if (!upload(gpu, slot, srcY, tile.y, srcYBytes) || !upload(gpu, slot, srcCb, tile.cb, srcCBytes) || !upload(gpu, slot, srcCr, tile.cr, srcCBytes))
        {
            return CudaComposeStatus::Failed;
        }
        if (tile.a != nullptr)
        {
            auto* alphaPlane = static_cast<unsigned short*>(gpu.slot[slot].a.ensure(srcYBytes, false));
            if (alphaPlane == nullptr || !upload(gpu, slot, alphaPlane, tile.a, srcYBytes))
            {
                return CudaComposeStatus::Failed;
            }
            args.inA = samplesPlane(alphaPlane, tile.srcWidth);
        }
        if (cudaEventRecord(gpu.uploaded[slot], gpu.copy) != cudaSuccess || cudaStreamWaitEvent(gpu.compute, gpu.uploaded[slot], 0) != cudaSuccess)
        {
            return CudaComposeStatus::Failed;
        }
        args.inY = samplesPlane(srcY, tile.srcWidth);
        args.inCb = samplesPlane(srcCb, tile.srcWidth / 2);
        args.inCr = samplesPlane(srcCr, tile.srcWidth / 2);
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
    cudaEventRecord(gpu.mark[2], gpu.compute);
    if (desc.overlay != nullptr && desc.overlay->width == desc.width && desc.overlay->height == desc.height)
    {
        // Uploaded by the overlay thread; normally finished long ago.
        if (cudaStreamWaitEvent(gpu.compute, desc.overlay->ready, 0) != cudaSuccess)
        {
            return CudaComposeStatus::Failed;
        }
        blendRgbaKernel<<<tiles2d(desc.width, desc.height), dim3(16, 16), 0, gpu.compute>>>(dstY, dstCb, dstCr, desc.width, desc.height, desc.overlay->rgba,
            desc.width * 4);
        if (cudaGetLastError() != cudaSuccess)
        {
            return CudaComposeStatus::Failed;
        }
    }
    else if (desc.rgba != nullptr && desc.rgbaStride >= desc.width * 4)
    {
        std::size_t const rgbaBytes = static_cast<std::size_t>(desc.rgbaStride) * static_cast<std::size_t>(desc.height);
        void const* before = gpu.rgba.ptr;
        auto* deviceRgba = gpu.rgba.ensure(rgbaBytes, false);
        if (deviceRgba == nullptr)
        {
            return CudaComposeStatus::Failed;
        }
        // The overlay is redrawn at MV_OVERLAY_HZ, not per frame: upload only a new one.
        if (desc.rgbaVersion == 0 || desc.rgbaVersion != gpu.rgbaVersion || deviceRgba != before)
        {
            if (!upload(gpu, 0, deviceRgba, desc.rgba, rgbaBytes))
            {
                return CudaComposeStatus::Failed;
            }
            if (cudaEventRecord(gpu.uploaded[0], gpu.copy) != cudaSuccess || cudaStreamWaitEvent(gpu.compute, gpu.uploaded[0], 0) != cudaSuccess)
            {
                return CudaComposeStatus::Failed;
            }
            gpu.rgbaVersion = desc.rgbaVersion;
        }
        blendRgbaKernel<<<tiles2d(desc.width, desc.height), dim3(16, 16), 0, gpu.compute>>>(dstY, dstCb, dstCr, desc.width, desc.height,
            static_cast<std::uint8_t*>(deviceRgba), desc.rgbaStride);
        if (cudaGetLastError() != cudaSuccess)
        {
            return CudaComposeStatus::Failed;
        }
    }
    cudaEventRecord(gpu.mark[3], gpu.compute);
    std::size_t const outBytes = static_cast<std::size_t>(desc.v210RowBytes) * static_cast<std::size_t>(desc.height);
    auto* deviceV210 = gpu.v210.ensure(outBytes, false);
    // Write the MXL grain directly when it can be locked; otherwise through a pinned buffer.
    bool const direct = desc.v210OutIsGrain && hostMemory().ensure(desc.v210Out, outBytes, false);
    void* outHost = direct ? static_cast<void*>(desc.v210Out) : gpu.outPin.ensure(outBytes, true);
    if (deviceV210 == nullptr || outHost == nullptr)
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
    cudaEventRecord(gpu.mark[4], gpu.compute);
    if (cudaEventRecord(gpu.computed[0], gpu.compute) != cudaSuccess || cudaStreamWaitEvent(gpu.copy, gpu.computed[0], 0) != cudaSuccess)
    {
        return CudaComposeStatus::Failed;
    }
    if (cudaMemcpyAsync(outHost, deviceV210, outBytes, cudaMemcpyDeviceToHost, gpu.copy) != cudaSuccess)
    {
        return CudaComposeStatus::Failed;
    }
    cudaEventRecord(gpu.mark[5], gpu.copy);
    if (cudaStreamSynchronize(gpu.copy) != cudaSuccess || cudaStreamSynchronize(gpu.compute) != cudaSuccess)
    {
        return CudaComposeStatus::Failed;
    }
    if (!direct)
    {
        std::memcpy(desc.v210Out, outHost, outBytes);
    }
    if (desc.timing != nullptr)
    {
        cudaEventElapsedTime(&desc.timing->background, gpu.mark[0], gpu.mark[1]);
        cudaEventElapsedTime(&desc.timing->tiles, gpu.mark[1], gpu.mark[2]);
        cudaEventElapsedTime(&desc.timing->overlay, gpu.mark[2], gpu.mark[3]);
        cudaEventElapsedTime(&desc.timing->pack, gpu.mark[3], gpu.mark[4]);
        cudaEventElapsedTime(&desc.timing->download, gpu.mark[4], gpu.mark[5]);
    }
    gpu.lastWidth = desc.width;
    gpu.lastHeight = desc.height;
    return CudaComposeStatus::Ok;
}

bool cudaPreviewTile(int width, int height, std::uint8_t* nv12, float* gpuMs)
{
    auto& gpu = session();
    if (!gpu.ready || gpu.lastWidth < 2 || gpu.lastHeight < 2 || width < 2 || height < 2 || (width & 1) != 0 || (height & 1) != 0 || nv12 == nullptr)
    {
        return false;
    }
    std::size_t const bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 / 2;
    auto* tile = static_cast<std::uint8_t*>(gpu.previewTile.ensure(bytes, false));
    auto* pin = static_cast<std::uint8_t*>(gpu.previewPin.ensure(bytes, true));
    if (tile == nullptr || pin == nullptr)
    {
        return false;
    }
    // Compose has finished (it synchronises), so its stage events are free here.
    cudaEventRecord(gpu.mark[0], gpu.compute);
    nv12TileKernel<<<tiles2d(width / 2, height / 2), dim3(16, 16), 0, gpu.compute>>>(static_cast<unsigned short const*>(gpu.canvasY.ptr),
        static_cast<unsigned short const*>(gpu.canvasCb.ptr), static_cast<unsigned short const*>(gpu.canvasCr.ptr), gpu.lastWidth, gpu.lastHeight, tile, width,
        height);
    if (cudaGetLastError() != cudaSuccess || cudaMemcpyAsync(pin, tile, bytes, cudaMemcpyDeviceToHost, gpu.compute) != cudaSuccess ||
        cudaEventRecord(gpu.mark[1], gpu.compute) != cudaSuccess || cudaStreamSynchronize(gpu.compute) != cudaSuccess)
    {
        cudaGetLastError();
        return false;
    }
    if (gpuMs != nullptr)
    {
        cudaEventElapsedTime(gpuMs, gpu.mark[0], gpu.mark[1]);
    }
    std::memcpy(nv12, pin, bytes);
    return true;
}

bool cudaPreviewContext(void** context, void** stream)
{
    // cudaFree(nullptr) makes the runtime's primary context current on this thread.
    if (context == nullptr || stream == nullptr || !cudaRuntimeAvailable() || cudaSetDevice(0) != cudaSuccess || cudaFree(nullptr) != cudaSuccess)
    {
        return false;
    }
    // The driver API without linking libcuda (it comes from the NVIDIA container toolkit at run time).
    void* getCurrent = nullptr;
    cudaDriverEntryPointQueryResult found{};
    CUcontext current = nullptr;
    if (cudaGetDriverEntryPointByVersion("cuCtxGetCurrent", &getCurrent, 12000, cudaEnableDefault, &found) != cudaSuccess || getCurrent == nullptr ||
        reinterpret_cast<CUresult (*)(CUcontext*)>(getCurrent)(&current) != CUDA_SUCCESS || current == nullptr)
    {
        cudaGetLastError();
        return false;
    }
    cudaStream_t created = nullptr;
    if (cudaStreamCreateWithFlags(&created, cudaStreamNonBlocking) != cudaSuccess)
    {
        cudaGetLastError();
        return false;
    }
    *context = current;
    *stream = created;
    return true;
}

std::shared_ptr<CudaFrame const> cudaUploadFrame(std::uint8_t const* v210, int v210RowBytes, std::uint8_t const* alpha10, int alphaRowBytes, int width,
    int height)
{
    if (v210 == nullptr || width < 2 || (width & 1) != 0 || height < 1 || v210RowBytes < 16 || !cudaRuntimeAvailable())
    {
        return nullptr;
    }
    auto& up = uploader();
    if (!up.open())
    {
        return nullptr;
    }
    bool const alpha = alpha10 != nullptr && alphaRowBytes > 0;
    auto frame = framePool().acquire(width, height, alpha, up.stream);
    if (frame == nullptr || v210RowBytes != frame->v210RowBytes || (alpha && alphaRowBytes != frame->alphaRowBytes))
    {
        return nullptr;
    }
    // The grain stays packed on the device; the scaler decodes the samples it needs.
    std::size_t const bytes = static_cast<std::size_t>(v210RowBytes) * static_cast<std::size_t>(height);
    bool ok = copyToDevice(up.stream, frame->v210, v210, bytes, up.staging);
    if (ok && alpha)
    {
        std::size_t const keyBytes = static_cast<std::size_t>(alphaRowBytes) * static_cast<std::size_t>(height);
        ok = copyToDevice(up.stream, frame->alpha10, alpha10, keyBytes, up.stagingAlpha);
    }
    // Record even on failure: the pool waits on this event before it reuses the frame.
    if (cudaEventRecord(frame->ready, up.stream) != cudaSuccess || !ok)
    {
        return nullptr;
    }
    return frame;
}

std::shared_ptr<CudaOverlay const> cudaUploadOverlay(std::shared_ptr<CudaOverlay const> const& previous, std::uint8_t const* rgba, int width, int height,
    CudaRect const* changes, int changeCount)
{
    if (rgba == nullptr || width < 1 || height < 1 || !cudaRuntimeAvailable())
    {
        return nullptr;
    }
    auto& up = uploader();
    if (!up.open())
    {
        return nullptr;
    }
    auto overlay = overlayPool().acquire(width, height, up.stream);
    if (overlay == nullptr)
    {
        return nullptr;
    }
    std::size_t const stride = static_cast<std::size_t>(width) * 4u;
    std::size_t const bytes = stride * static_cast<std::size_t>(height);
    bool ok = true;
    if (previous != nullptr && previous->width == width && previous->height == height && changes != nullptr)
    {
        ok = cudaStreamWaitEvent(up.stream, previous->ready, 0) == cudaSuccess &&
             cudaMemcpyAsync(overlay->rgba, previous->rgba, bytes, cudaMemcpyDeviceToDevice, up.stream) == cudaSuccess;
        for (int i = 0; ok && i < changeCount; ++i)
        {
            auto const& area = changes[i];
            std::size_t const offset = static_cast<std::size_t>(area.y) * stride + static_cast<std::size_t>(area.x) * 4u;
            ok = cudaMemcpy2DAsync(overlay->rgba + offset, stride, rgba + offset, stride, static_cast<std::size_t>(area.w) * 4u, static_cast<std::size_t>(area.h),
                     cudaMemcpyHostToDevice, up.stream) == cudaSuccess;
        }
    }
    else
    {
        ok = cudaMemcpyAsync(overlay->rgba, rgba, bytes, cudaMemcpyHostToDevice, up.stream) == cudaSuccess;
    }
    // Finish here, on the overlay thread: the buffers it read or wrote can then be
    // recycled by any head without pending copies.
    ok = cudaEventRecord(overlay->ready, up.stream) == cudaSuccess && ok;
    ok = cudaStreamSynchronize(up.stream) == cudaSuccess && ok;
    return ok ? overlay : nullptr;
}

void cudaReleaseHostMemory()
{
    // No queued copy may still use the memory when it is unlocked.
    auto& up = uploader();
    if (up.ready)
    {
        cudaStreamSynchronize(up.stream);
    }
    auto& gpu = session();
    if (gpu.ready)
    {
        cudaStreamSynchronize(gpu.copy);
        cudaStreamSynchronize(gpu.compute);
    }
    hostMemory().release();
}
} // namespace mv
