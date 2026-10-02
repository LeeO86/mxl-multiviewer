# mxl-multiviewer — Implementation Plan

This document records how `SPECIFICATION.md` is implemented, including every deviation. It was written before the code, then updated where the pinned APIs or the build forced a change. `docs/audit.md` is the reason the Rust/GStreamer tree was replaced rather than extended.

## 1. Pins

| Component | Pin |
| --- | --- |
| MXL | `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, `-DMXL_ENABLE_FABRICS_OFI=OFF`. One `MXL_REF` in `docker/Dockerfile` and `.github/workflows/ci.yaml`. |
| nmos-cpp | `fe303849527394b03bdedc8f161f377fe458bb62` (`NMOS_CPP_REF`), same commit as mxl-decklink, mxl-fabrics-agent, and mxl-webrtc-monitor |
| Font | DejaVu Sans 2.37, `third_party/dejavu/DejaVuSans.ttf`, drawn by Blend2D 0.21.2. The 8×8 bitmap remains when `MV_WITH_BLEND2D=OFF` |
| UI | Vue 3 + Vite, one embedded HTML file. No CDN |
| JPEG | stb_image / stb_image_write (public domain), vendored |

Grain index math follows `lib/internal/include/mxl-internal/IndexConversion.hpp` at that MXL pin:

```
index = (timestamp * numerator + 500000000 * denominator) / (1000000000 * denominator)
```

with `__int128` rounding. The process calls `mxlTimestampToIndex` / `mxlGetTime` on the media path so it cannot drift from readers. The same formula is compiled into the unit-test binary (no libmxl) and checked against the vectors in MXL's `test_time.cpp` (index 0 at t=0, index 1 at the rounded 30000/1001 period).

## 2. Overlay library

**Blend2D.** It rasterises on the CPU into an RGBA buffer the compositor already blends, it is Zlib licensed, and it does not open a second GPU context. Skia was the larger alternative. The default build fetches Blend2D 0.21.2 (the source tarball, which includes its asmjit) and links it statically. Text is DejaVu Sans 2.37, compiled into the binary from `third_party/dejavu/DejaVuSans.ttf`. Nothing is downloaded at runtime. `MV_WITH_BLEND2D=OFF` keeps the public-domain 8×8 bitmap path. `overlayUsesBlend2d()` reports which path this binary uses. The composer still only calls `renderOverlay`.

## 3. Source layout

```
src/
  main.cpp
  version.hpp
  config/          env > file > default, validation
  domain/          MXL root scan, mirror marker
  layout/          model, presets, geometry
  media/           v210, scale, compose, overlay, ppm, alarm, jpeg, timebase
  media/cuda_compose.cu
  control/         TSL 5.0 and 3.1
  nmos/            ids + node (nmos-cpp behind MV_WITH_NMOS)
  mxlio/           readers, writers, composer threads (needs libmxl)
  ops/             HTTP, WebSocket, metrics, REST
  app/             shared runtime snapshot
web/               Vue 3 editor
tests/unit/       doctest
tests/integration/mosaic.sh
tests/tools/      pattern writer and pixel sampler
tests/nmos/amwa.sh
docker/ deploy/ assets/ third_party/
```

`mv-core` has no libmxl and no nmos-cpp. Unit tests link only that library. `mxl-multiviewer`, `mxl-mv-writer`, and `mxl-mv-sample` are built when `find_package(mxl)` succeeds.

## 4. Deviations

1. **Rewrite.** The Rust/GStreamer 2×2 code is deleted. See `docs/audit.md`. Nothing in it implemented v210, NMOS, or TAI indexing.
2. **v210a key is used as straight alpha.** The prompt allowed "ignored or used". Using it matches a fill+key input on a wall. Missing or full-scale key is opaque.
3. **Interlaced inputs are bobbed** (field 0, even lines) onto the progressive output. There is no motion-adaptive deinterlacer.
4. **Query API port is `NMOS_REGISTRY_PORT + 1`.** Same as mxl-webrtc-monitor and the nmos-cpp registry defaults.
5. **Ring depth is the domain `history_duration` option**, not a per-flow setting. This process writes `options.json` only when it creates the output domain. It does not rewrite a domain it did not create.
6. **Layouts are a versioned JSON document** (`MV_LAYOUTS_FILE`), not a flat env blob. Flat `KEY=value` remains the config model for everything in the configuration table. The settings view exports `KEY=value` and a separate layout export.
6a. **Blend2D is the default overlay.** See §2. `MV_WITH_BLEND2D=OFF` still builds the 8×8 bitmap renderer. The first cut left Blend2D unlinked because asmjit is a second C++ build; that cut is reversed. The published image and CI use Blend2D.
7. **`MV_OUTPUTS` is implemented** up to 3. The prompt allowed deferring it. The composer loop is per head, so the extra heads are the same code path.
8. **TSL 3.1 is implemented** behind `TSL_V31` (default false). TSL 5.0 is always the primary parser.
9. **CPU inner loops are scalar plus SSE2 clear/blend** on x86_64, with one thread per tile. A third-party scaler is not linked. The planar `uint16_t` layout is the SIMD-friendly form. Hand-written AVX2 v210 unpack is not in this round; `docs/performance.md` is where a miss against the CPU target would be recorded.
10. **CUDA is in the published image, and optional on a from-source build.** `docker/Dockerfile` builds on `nvidia/cuda:12.8.2-devel-ubuntu24.04` and statically links `libcudart`. Architectures are `sm_75`, `sm_86` (RTX A4000), and `sm_89` (L4), with PTX for the last so a newer GPU can JIT. The runtime image is still Ubuntu 24.04 and does not contain the NVIDIA driver. `MV_BACKEND=auto` calls `cudaGetDeviceCount`; that succeeds only when the NVIDIA container toolkit (Docker `--gpus all` / `docker/docker-compose.gpu.yaml`) or a Kubernetes `nvidia` runtime (`deploy/mxl-multiviewer-gpu.yaml`) has injected `libcuda`. Otherwise the same binary uses the CPU path and still starts. `MV_BACKEND=cuda` with no device exits 75. `MV_BACKEND=cuda` on a binary built without nvcc (the `ci.yaml` job) exits 78. Kernels unpack v210 and the v210a key, bilinear-scale, blend the RGBA overlay, and pack v210. Two streams and pinned host buffers overlap upload, compute, and download. Readers still unpack on the CPU so alarms, the luma hash, and the CPU fallback share one frame. A CUDA failure on a single output frame falls back to that CPU path.
11. **AMWA NMOS Testing** is `tests/nmos/amwa.sh`, not a default CI job. The harness image is large and the suites are long. CI does start the node and PATCH an IS-05 receiver (the integration script), which is the activation behaviour the platform depends on.
12. **The demo registry is the Python stand-in** in `tests/integration/fake_registry.py`, same approach as mxl-webrtc-monitor. It implements the registration and query calls this process makes. A facility sets `NMOS_REGISTRY_ADDRESS` to nmos-cpp. The Compose file documents that.
13. **Performance targets are not measured here.** `docs/performance.md` says so. The numbers in the prompt stay the acceptance bar for a hardware run.
14. **GStreamer is not used in tests.** The prompt allowed it. The tests talk to libmxl and to HTTP instead.
15. **Output audio is a copy, not a mix.** Audio-follow copies one input. Channels beyond the output width are dropped. Fewer channels are padded with silence.
16. **Sender subscription updates.** nmos-cpp updates the IS-04 receiver `subscription` as part of connection activation at this pin. The node also writes `sender_id` and `active` explicitly after activation so a library change cannot leave the fabrics agent blind.
17. **`/readyz` does not require every input to be `running`.** A multiviewer with unrouted inputs is a working wall of slates. Readiness is the output domain, a fresh composer heartbeat, and registry registration when a registry is configured.
18. **Default TSL ports are 8910 and 8911.** The prompt did not assign numbers. These miss the host ports listed in the platform notes.
19. **Background images are JPEG or PNG** via stb. Other formats are rejected. The image is scaled to cover the canvas (`fill`) once per output raster, then copied under the tiles.
20. **DNS-SD off** sets nmos-cpp `pri` and `highest_pri` to the maximum integer, which disables advertisement and discovery. Same as mxl-webrtc-monitor.
21. **Unknown environment variables are ignored.** The config file still rejects unknown keys. CI exports `NMOS_CPP_REF` (and `MXL_REF`) on every step, including the process under test; treating every `NMOS_` name as configuration made that step exit 78.

## 5. Process

One thread per input reads MXL and publishes a `shared_ptr` snapshot (grain copy, audio window, format, state). One thread per output head paces on TAI, gathers snapshots, composites, and writes. The HTTP thread serves REST, the JPEG, and WebSocket clients. A TSL thread owns the UDP socket and the TCP accept loop. nmos-cpp runs on its own threads. The overlay mutex is separate from the grain mutex; the composer never waits on a reader syscall.

Layout activation stores a `shared_ptr<const Layout>`. The composer copies that pointer at the start of an index and uses it for the whole frame.

## 6. Tests

- Unit tests cover every item in specification §14 except the live MXL round-trip.
- `tests/integration/mosaic.sh` is the CPU integration test. It needs the binaries and a writable tmpfs (`/dev/shm`).
- Hardware procedure is `docs/performance.md`.

## 7. Container

Multi-stage Dockerfile:

1. Node image builds the Vue file.
2. `nvidia/cuda:12.8.2-devel-ubuntu24.04` builds MXL at `MXL_REF`, fetches nmos-cpp at `NMOS_CPP_REF`, builds this project with nvcc, runs unit tests. The unit tests do not call the GPU. `ARG CUDA_IMAGE` is declared before the first `FROM`; an `ARG` after the webui stage is not visible to the next `FROM`, and BuildKit then refuses the build with a blank base name.
3. Runtime image: Ubuntu 24.04, the binary, libmxl, nmos-cpp shared libraries. User 1000:1000. No GStreamer packages and no NVIDIA driver. `libcudart` is inside the binary.

`io.dmf.mxl.revision` is `MXL_REF`.

On a GPU host with the NVIDIA container toolkit the process needs the driver injected at start (`--gpus all`, `docker/docker-compose.gpu.yaml`, or `deploy/mxl-multiviewer-gpu.yaml`). The toolkit is what provides `libcuda`. The image already contains the compositor.
