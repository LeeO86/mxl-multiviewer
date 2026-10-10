# Performance

The targets in `SPECIFICATION.md` §13 have not been measured on an RTX A4000, an L4, or a Precision 3930-class CPU in this tree. CI runs the CPU backend on a small raster (`192x108p50`) and checks pixels, not the 16×1080p50 budget. One lab run on other hardware is recorded below. It misses the targets.

The CUDA compositor is compiled into the container image. Each input grain is uploaded once by its input thread (page-locked MXL memory, own stream) and stays packed on the device; heads scale and blend from it, and the result is written by DMA into the MXL output grain. The overlay thread keeps the overlay on the device and uploads only what changed. `mxl_multiviewer_compose_seconds` is labelled `backend=cuda` or `backend=cpu`; `mxl_multiviewer_compose_gpu_seconds{stage=background|tiles|overlay|pack|download}` is the GPU time of each stage (CUDA events); `mxl_multiviewer_gpu_memory_bytes` is `cudaMemGetInfo` total minus free while CUDA is in use.

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

## Lab run 2026-10-04: NVIDIA A16, GPU input upload

Same host, GPU, inputs and method as the run above; image built from this tree (1.1.0). `mxl-multiviewer:1.0.0` was measured again the same day as the reference. Compose buckets are the histogram's (1, 2, 5, 10, 20, 40, 80 ms). GPU stages are the means of `mxl_multiviewer_compose_gpu_seconds`.

| Image | Inputs → output | Layout | Output frames / 1500 | Late | Missed | Compose mean | Compose p50 / p95 bucket | GPU stages (mean) | Process CPU | GPU util |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1.0.0 | 16 → 1080p50 | 4x4 | 474 | 1503 | 1029 | 60.7 ms | ≤ 80 / ≤ 80 ms | – | 8.0 cores | 27 % |
| 1.0.0 | 4 → 1080p50 | 2x2 | 1382 | 815 | 123 | 18.7 ms | ≤ 20 / ≤ 40 ms | – | 2.7 cores | 32 % |
| this tree | 1 → 1080p50 | 1 | 1506 | 0 | 0 | 3.6 ms | ≤ 5 / ≤ 10 ms | – | 0.6 cores | 18 % |
| this tree | 2 → 1080p50 | 2x2 | 1508 | 0 | 0 | 3.2 ms | ≤ 5 / ≤ 10 ms | – | 0.7 cores | 14 % |
| this tree | 9 → 1080p50 | 3x3 | 1505 | 0 | 0 | 5.1 ms | ≤ 10 / ≤ 10 ms | – | 1.9 cores | 48 % |
| this tree | 16 → 1080p50 | 4x4 | 1505 | 0 | 0 | 2.3 ms | ≤ 5 / ≤ 5 ms | tiles 0.80, overlay 0.16, pack 0.14, download 1.01 ms | 2.2 cores | 77 % |
| this tree | 16 → 2160p50 | 4x4 | 1505 | 0 | 0 | 8.6 ms | ≤ 10 / ≤ 10 ms | tiles 2.94, overlay 0.57, pack 0.54, download 4.01 ms | 3.0 cores | 80 % |

The 1-, 2- and 9-input rows were taken before the overlay moved to the device and include the old overlay upload in compose. GPU utilisation now follows the input count, not the compose work: it is the copy engine taking the input grains (16 × 1080p50 is 4.4 GB/s on this x4 link).

Result: on this host both §13 CUDA cases now meet the compose criterion (under 10 ms at p95) with no late or missed frame in 30 s. The one-hour soak is below. The remaining cost at 2160p is the 22 MB download of each output grain (4 ms on x4); a x16 GPU takes about a quarter of that.

What changed to get there, in the order it was measured:

1. Upload each input grain once in its reader thread, straight from page-locked MXL memory, and compose from device frames: 474 → 1483 frames, compose 60.7 → 8.0 ms, CPU 8.0 → 2.7 cores.
2. Keep the grain packed on the device and decode v210 in the scaler: GPU memory 825 → 563 MiB.
3. Draw the overlay on its own thread and take the preview from the packed output at preview size: the remaining late frames came from overlay drawing and a full CPU unpack for the preview on the output thread (0 late in 60 s).
4. Keep the overlay on the device, uploaded by the overlay thread, only the areas that changed: the overlay stage in compose went from 4.0 ms (waiting behind the input copies) to 0.2 ms at 1080p, and 2160p from 41 % late frames to none.

### One-hour soak, 16 → 2160p50

Same host and inputs, image built from `bd977ad` (this release before the version and documentation changes), 4x4 layout, CUDA backend, 15 s warm-up, then one hour measured with nothing else running on the host:

| Output frames | Late | Missed | Compose mean | p50 / p95 bucket | Process CPU | GPU util | GPU memory |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 180005 (180000 expected) | 0 | 0 | 8.6 ms | ≤ 10 / ≤ 10 ms | 3.5 cores | 80 % | 667 MiB |

This meets the §13 target (zero late frames over one hour) for 2160p on a GPU that is not one of the target GPUs. An earlier soak of the same code overlapped with two container image builds on the host for 25 of its 60 minutes and had 8 late and 6 missed frames.

## Lab run 2026-10-05: CPU backend without a GPU (1.1.2)

Same host, no GPU device in the container (`MV_BACKEND=cpu`), inputs 1–4 from mxl-test-player and 5–16 from the lab writers, 1080p50 output, 4x4 layout, 20 s measured after 15 s warm-up:

| Image | Inputs | Output frames / 1000 | Late | Missed | Compose mean | Process CPU |
| --- | --- | --- | --- | --- | --- | --- |
| 1.1.1 | 4 | 973 | 572 | 32 | 19.0 ms | 3.7 cores |
| 1.1.1 | 16 | 503 | 1006 | 503 | 34.3 ms | 11.7 cores |
| 1.1.2 | 4 | 1006 | 0 | 0 | 8.6 ms | 1.7 cores |
| 1.1.2 | 16 | 1006 | 1 | 0 | 13.3 ms | 5.9 cores |

What the 1.1.1 profile showed and what changed:

1. Every input was unpacked completely into a new 8 MB planar frame per grain (39 % at 16 inputs). Readers now keep a copy of the packed grain from a small buffer pool, and the scaler unpacks only the source lines a tile touches (`scaleV210Into`).
2. The bilinear scaler ran in float with `floor`, clamps and `lroundf` per sample (27–36 %). It now takes the same sample positions with taps computed once per placement and weights in 1/1024 steps (within 1 of the float result, exact on flat areas; unit test against `scaleInto`).
3. The black and freeze alarms read every 32nd pixel of the packed grain with one call per sample (22 % once the unpack was gone, because each sample is a new cache line). They are now taken while the reader copies the grain line by line (`copyV210Scan`, the same samples in the same order).
4. Each tile ran on a new `std::async` thread per frame and allocated its image; a persistent pool (up to 16 workers) renders into reused images.
5. The overlay was converted from RGBA in `double` over the whole canvas on every frame. The overlay thread now converts it once per drawing (`prepareOverlay`), and the compose thread blends only the visible spans with integers (the same samples as `blendStraightRgba`, unit test).

With the CUDA backend (16 → 1080p50) frames and compose time are unchanged (1005 / 1000, 0 late, 2.4 ms) and process CPU went from 2.9 to 1.8 cores, from item 3.

## Lab run 2026-10-10: WebRTC preview (1.4.0)

Same host, one A16 GPU (shared with an mxl-replay container at about 45 % GPU), image built from this branch. Four heads 1080p50 (`MV_OUTPUTS=4`, layouts 2x2, 1, 2x2, 1), inputs 1–4 from mxl-test-player (1080p50), `MV_PREVIEW_FPS=5`, 20 s warm-up, 60 s measured. "Preview step" is `preview_seconds` (the head thread's preview work per picture), "GPU" is `compose_gpu_seconds{stage="preview"}` (tile kernel and its 0.8 MB download, CUDA events, queueing on the shared GPU included), "encode" is `preview_encode_seconds` (copy, encode and send of one 1920×1080 picture on the publisher thread).

| Backend | Mode | Frames per head (3000 expected) | Late per head | Preview step per head | GPU per head | Encode per picture | Publisher thread CPU | Process CPU |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| CUDA | jpeg | 3001–3002 | 0 | 11.4–12.2 ms | – | – | – | 3.64 cores |
| CUDA | webrtc (NVENC) | 3011 | 0 | 1.8–4.4 ms | 1.1–3.7 ms | 11.1 ms (wall) | 19 ms/s (3.9 ms per picture) | 3.88 cores |
| CUDA | webrtc, 25 fps | 1502 / 30 s | 0 | 2.3–4.7 ms | 1.8–4.1 ms | 6.5 ms | 64 ms/s (2.5 ms per picture) | 4.02 cores |
| CPU | jpeg | 2140–2991 | 433–3001 | 8.0–8.4 ms | – | – | – | 8.49 cores |
| CPU | webrtc (x264) | 2137–2985 | 442–3001 | 5.4–5.8 ms | – | 4.1 ms | 15 ms/s (3.1 ms per picture) | 8.78 cores |

- On the CUDA backend the WebRTC preview takes the JPEG encode (about 12 ms of CPU per head and picture, on the head thread) off the heads: what is left there is the tile kernel and the download. NVENC ran one session at 4–5 pictures per second and reported 0–1 % encoder use; the mosaic is 4 Mbit/s CBR.
- The CPU backend is over its budget with four 1080p50 heads in both modes (the `1` layouts compose in 24 ms); WebRTC changes nothing there. A first cut scaled the mosaic tile on the head thread alone (29 ms per picture); it now runs in bands on the head's tile workers.
- With 16 inputs (four from the player, twelve lab writers) and these four heads, neither mode keeps real time on this shared A16 (CUDA compose 30–470 ms per frame in repeated runs; JPEG and WebRTC alike), so the comparison above uses four inputs.
- Headless Edge (from the office network over the VPN) played the stream from the built-in MediaMTX: 1920×1080, `object-view-box` of each `/widget/head?head=<h>` page equal to the tile map, the centre of each region in that head's colour (each head showed a layout of one background colour), and the `<video>` box in the region's aspect, so no other head shows. A page on an origin listed in `WIDGET_FRAME_ANCESTORS` framed the widget and received `widget-ready` and `widget-size`; another origin was refused by the CSP. The tile-editor widget showed the four tiles of a 2x2 head and saved a caption change.
- The built-in MediaMTX came back after `kill -9` (restarts 1) and the stream published again within 2 s.
- The caption strip and the audio bar strip (§6.2, a full-canvas tile of a lab writer, caption at the bottom without overlay, two bars at the right without overlay) sampled the same on the CUDA and the CPU path: picture x 0–1843, y 3–1039; letterbox y 0–2 and 1040–1043; caption strip from y 1044; bar strip from x 1844.

## Lab run 2026-10-10: crop in Edge and Firefox, reconnect (1.4.1)

Same host, GPU 2, image built from this branch, CUDA backend. Four heads, no inputs, each showing a layout of one background colour (red, green, blue, yellow); heads 1–3 1080p50, head 4 640×480p25, so its region is 4:3 (720×540 at 1080, 540). Headless Edge 155 (DevTools protocol) and headless Firefox 140.9 ESR (WebDriver BiDi; it fetched OpenH264 2.6.0 at start, as any installation does) on a Windows client over the VPN opened each head's `/widget/head` page (960×540) and the Preview tab (1600×900) with each head's button.

- The stream played (1920×1080) in all 16 views; no `object-view-box` (`none` in Edge, unknown to Firefox).
- The part of the mosaic each box shows, from the box and the transformed `<video>` rectangles, equals the tile map to 0.1 px in all 16 views.
- Screenshots: inside each box every sampled pixel is nearest to that head's colour; around head 4's 4:3 box in the 16:9 frame there is only black. The outermost 1–2 screen pixels at some box edges are a blend with the neighbouring mosaic pixels (filtering and 4:2:0 chroma at the boundary); 1.4.0's `object-view-box` gave the same pixel values in Edge.
- Own mode: after `kill -9` of the built-in MediaMTX the state was `error` (`rtsp write: Broken pipe`) within 0.5 s, the supervisor started it after 1 s, and the stream was `publishing` again at 1.6 s (`restarts` 1). An open widget page in Edge connected again by itself and played on.
- Shared mode: a separate MediaMTX 1.20.1 stopped for 10 s: `error` (connection refused, retried every 2 s), `publishing` 2 s after it was started again. The widget page (WHEP from that MediaMTX, `PREVIEW_WHEP_URL`) lost the picture and played again.
- `tests/integration/preview.sh` passed in the image (plus python3 and procps, loopback only). With one expected value changed (the own-mode encoder, `restarts`, `mediamtx` in shared mode) it failed at that check each time.
