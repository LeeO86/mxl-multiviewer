# Performance

The targets in `SPECIFICATION.md` §13 have not been measured on an RTX A4000, an L4, or a Precision 3930-class CPU in this tree. CI runs the CPU backend on a small raster (`192x108p50`) and checks pixels, not the 16×1080p50 budget. One lab run on other hardware is recorded below. It misses the targets.

The CUDA compositor is compiled into the container image (unpack, scale, blend, pack, pinned buffers, two streams). A hardware run still has to time it. `mxl_multiviewer_compose_seconds` is labelled `backend=cuda` or `backend=cpu`, and `mxl_multiviewer_gpu_memory_bytes` is `cudaMemGetInfo` total minus free while CUDA is in use.

When a hardware run is taken, record:

- GPU, driver, and `MV_BACKEND`
- input count, raster, and rate, and the output raster
- `mxl_multiviewer_compose_seconds` p50/p95
- `mxl_multiviewer_output_frames_late_total` over the run
- input grain origin timestamp versus the output grain index that first showed it

Pass condition: compose time under half a frame (10 ms at 50p) and zero late output frames over one hour, with added latency at most two output frames plus `MV_INPUT_OFFSET_GRAINS`.

## Lab run 2026-10-03: NVIDIA A16, image 1.0.0

Not a target GPU. The A16 is four GA107 GPUs behind one PCIe switch, and each GPU has a PCIe Gen4 x4 link (`nvidia-smi -q`, Link Width 4x). The host is 2× Xeon Gold 6136 (Skylake-SP, 3.0 GHz), driver 595.84, `ghcr.io/leeo86/mxl-multiviewer:1.0.0`, one GPU visible (`device_ids: ["0"]`). Inputs 1–4 are mxl-test-player 1080p50 outputs (SMPTE RP 219 bars with burn-in), inputs 5–16 are `mxl-mv-writer` 1080p50 solids, all routed by IS-05. Each case ran 15 s warm-up and 30 s measured. `late` is `mxl_multiviewer_output_frames_late_total`, `missed` the frames skipped behind the clock.

| Backend | Inputs → output | Layout | Output frames / 1500 | Late | Missed | Compose mean | Compose p50 / p95 bucket | Process CPU | GPU util |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cuda | 1 → 1080p50 | 1 | 1505 | 0 | 0 | 10.3 ms | ≤ 10 / ≤ 20 ms | 1.0 core | 19 % |
| cuda | 2 → 1080p50 | 2x2 | 1505 | 0 | 0 | 12.7 ms | ≤ 20 / ≤ 20 ms | 1.6 cores | 23 % |
| cuda | 4 → 1080p50 | 2x2 | 1486 | 346 | 20 | 16.1 ms | ≤ 20 / ≤ 40 ms | 2.5 cores | 37 % |
| cuda | 9 → 1080p50 | 3x3 | 930 | 1505 | 575 | 28.1 ms | ≤ 40 / ≤ 40 ms | 4.5 cores | 41 % |
| cuda | 16 → 1080p50 | 4x4 | 581 | 1505 | 924 | 48.0 ms | ≤ 80 / ≤ 80 ms | 8.0 cores | 39 % |
| cuda | 16 → 2160p50 | 4x4 | 316 | 1508 | 1192 | 90.4 ms | > 80 / > 80 ms | 7.9 cores | 27 % |
| cpu | 1 → 1080p50 | 1 | 192 | 1501 | 1309 | 151.4 ms | > 80 / > 80 ms | 1.3 cores | – |
| cpu | 4 → 1080p50 | 2x2 | 501 | 1505 | 1004 | 57.4 ms | ≤ 80 / ≤ 80 ms | 3.9 cores | – |

Result: every §13 case fails on this host. CUDA keeps up to two 1080p50 inputs. Compose time grows by about 2.5 ms per input while the GPU stays below half load, so the limit is on the host side of `cudaComposeFrame`, not in the kernels. The added-latency criterion was not measured because no case passes the compose criterion.

Profile (`perf`, 16 inputs, CUDA):

- 39 % `unpackV210` and 6 % `Frame422::allocate` in the reader threads. Readers unpack every grain on the CPU even with CUDA, and allocate a new 8 MB planar frame and a new packed copy per grain.
- About 32 % `memcpy`/`memset` in libc.
- `upload()` waits for the whole copy stream (`cudaStreamSynchronize`) before it copies a 5.5 MB grain into one of two pinned buffers on the composer thread, so CPU copies and PCIe transfers never overlap. With about 1.5 ms of copy and 0.9 ms of x4 transfer per tile, that is the 2.5 ms per input. The full-frame RGBA overlay (8.3 MB at 1080p) is uploaded the same way on every frame.

Even with full overlap, 16 full-raster uploads (88 MB per output frame) take about 13 ms on an x4 link, which is above the 10 ms budget. Meeting §13 on an A16 needs the upload out of the compose path: upload each input grain once when it arrives (reader threads into pinned staging, per-input device buffers) and compose from frames already on the device. On a x16 Gen4 GPU (A4000, L4) the same transfer is about 3.5 ms, so the current design may pass there; that run is still open.
