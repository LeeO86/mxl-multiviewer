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
4. **Query API defaults to the registry address and `NMOS_REGISTRY_PORT + 1`.** `NMOS_QUERY_ADDRESS` and `NMOS_QUERY_PORT` override that. Same default as the nmos-cpp registry.
5. **Ring depth is the domain `history_duration` option**, not a per-flow setting. This process writes `options.json` only when it creates the output domain. It does not rewrite a domain it did not create.
6. **Layouts are a versioned JSON document** (`MV_LAYOUTS_FILE`), not a flat env blob. Flat `KEY=value` remains the config model for everything in the configuration table. The settings view exports `KEY=value` and a separate layout export.
6a. **Blend2D is the default overlay.** See §2. `MV_WITH_BLEND2D=OFF` still builds the 8×8 bitmap renderer. The first cut left Blend2D unlinked because asmjit is a second C++ build; that cut is reversed. The published image and CI use Blend2D.
7. **`MV_OUTPUTS` is implemented** up to 3. The prompt allowed deferring it. The composer loop is per head, so the extra heads are the same code path.
8. **TSL 3.1 is implemented** behind `TSL_V31` (default false). TSL 5.0 is always the primary parser.
9. **CPU inner loops are scalar plus SSE2 clear/blend** on x86_64, with one thread per tile. A third-party scaler is not linked. The planar `uint16_t` layout is the SIMD-friendly form. Hand-written AVX2 v210 unpack is not in this round; `docs/performance.md` is where a miss against the CPU target would be recorded.
10. **CUDA is in the published image, and optional on a from-source build.** `docker/Dockerfile` builds on `nvidia/cuda:12.8.2-devel-ubuntu24.04` and statically links `libcudart`. Architectures are `sm_75`, `sm_86` (RTX A4000), and `sm_89` (L4), with PTX for the last so a newer GPU can JIT. The runtime image is still Ubuntu 24.04 and does not contain the NVIDIA driver. `MV_BACKEND=auto` calls `cudaGetDeviceCount`; that succeeds only when the NVIDIA container toolkit (Docker `--gpus all` / `docker/docker-compose.gpu.yaml`) or a Kubernetes `nvidia` runtime (`deploy/mxl-multiviewer-gpu.yaml`) has injected `libcuda`. Otherwise the same binary uses the CPU path and still starts. `MV_BACKEND=cuda` with no device exits 75. `MV_BACKEND=cuda` on a binary built without nvcc (the `ci.yaml` job) exits 78. Kernels unpack v210 and the v210a key, bilinear-scale, blend the RGBA overlay, and pack v210. Two streams and pinned host buffers overlap upload, compute, and download. Readers still unpack on the CPU so alarms, the luma hash, and the CPU fallback share one frame. A CUDA failure on a single output frame falls back to that CPU path.
11. **AMWA NMOS Testing** is `tests/nmos/amwa.sh`, not a default CI job. The harness image is large and the suites are long. CI does start the node and PATCH an IS-05 receiver (the integration script), which is the activation behaviour the platform depends on.
12. **The demo registry is the Python stand-in** in `tests/integration/fake_registry.py`, same approach as mxl-webrtc-monitor. It implements the registration and query calls this process makes. A facility sets `NMOS_REGISTRY_ADDRESS` to nmos-cpp. The Compose file documents that.
13. **Performance targets are not met on the A16 lab host.** `docs/performance.md` has the 2026-10-03 run: CUDA keeps two 1080p50 inputs, 16 inputs reach 48 ms per frame because grains are copied and uploaded one by one on the composer thread. The A4000/L4 run is still open. The numbers in the prompt stay the acceptance bar.
14. **GStreamer is not used in tests.** The prompt allowed it. The tests talk to libmxl and to HTTP instead.
15. **Output audio is a copy, not a mix.** Audio-follow copies one input. Channels beyond the output width are dropped. Fewer channels are padded with silence.
16. **Sender subscription updates.** nmos-cpp updates the IS-04 receiver `subscription` as part of connection activation at this pin. The node also writes `sender_id` and `active` explicitly after activation so a library change cannot leave the fabrics agent blind.
17. **`/readyz` does not require every input to be `running`.** A multiviewer with unrouted inputs is a working wall of slates. Readiness is the output domain, a fresh composer heartbeat, and registry registration when a registry is configured.
18. **Default TSL ports are 8910 and 8911.** The prompt did not assign numbers. These miss the host ports listed in the platform notes.
19. **Background images are JPEG or PNG** via stb. Other formats are rejected. The image is scaled to cover the canvas (`fill`) once per output raster, then copied under the tiles.
20. **DNS-SD off** sets nmos-cpp `pri` and `highest_pri` to the maximum integer, which skips advertisement and discovery. nmos-cpp still links its DNS-SD client library, so the image ships `libavahi-compat-libdnssd1`. avahi-daemon is not installed and is not required while `NMOS_DNS_SD=false`.
21. **Unknown environment variables are ignored.** The config file still rejects unknown keys. CI exports `NMOS_CPP_REF` (and `MXL_REF`) on every step, including the process under test; treating every `NMOS_` name as configuration made that step exit 78.

Release 1.2.0 (web UI rework, audio bars, captions, clocks, slates):

22. **Presets show audio bars** (user decision, 2026-10-06). Every input tile of the built-in presets has `audio_bars: true`, two channels from the first, on the right. A tile without the key still parses as `false`, so saved layouts keep their value. Specification §6.2 changed.
23. **Audio zones keep the implemented keys.** The specification named `audio_zones` (three thresholds); the code always read `zone_green` and `zone_amber`, and the third threshold (0 dBFS) is just the top of the scale. Specification §6.2 now names `zone_green` / `zone_amber`; a layout needs −60 ≤ `zone_green` ≤ `zone_amber` ≤ 0.
24. **The `is04` caption uses the registry.** One thread (`labelMain`) asks the Query API for the label of each input's routed sender every 10 s (500 ms timeout), by sender id, or by flow id (`senders?flow_id=`) when the IS-05 activation carried no sender id. A failed lookup keeps the previous label, a re-route clears it. After the registry label and the tile's text, the MXL flow label is used before `MV In <n>`, so a lab without a registry still shows the source name. Specification §4.3 updated.
25. **The audio leg has its own reader thread.** Each input's audio is read from its own flow and domain (its own MXL instance), independent of the video leg, and every new sample since the last read is metered from the flow's head index (at most 100 ms after a stall). The leg is `running` while the head moves, `no_signal` when it stands still for `MV_HOLD_MS`, `waiting` while the domain or flow is missing; a re-route opens the new flow. For audio-follow the leg appends its new samples (at most 16 channels) to a ring of 0.5 s per channel while a head follows that input, and the composer copies the samples of one output frame from it under a short lock. A first cut copied the whole 0.5 s window on every read (about 300 MB/s at 16 channels). A writer that restarts re-creates its flow, which MXL reports only as `MXL_ERR_FLOW_INVALID` on a read past the head; a leg whose head stands still probes past it once a second and re-opens the flow on that error, as the video reader does on any read.
26. **Slates follow the hold time.** The composer stops using a grain once it is older than `MV_HOLD_MS` at its read point; the tile is then limited-range black and the overlay draws `NO SIGNAL` or `WAITING` with the input name, by the same rule. `NOT ROUTED` shows at once, and a re-route drops the previous flow's frames at once, so a tile never holds a frame of the wrong source. The video leg reports `holding` once its last grain is three of its frames old and `no_signal` after the hold time, and the `no_signal` alarm now rises (it was never set before). A 50 ms wait without a grain does not touch the black, freeze, and format alarms; only `no_signal` clears them. A first cut cleared them on every wait, so sources below 20 fps (and jittery 24/25p sources) never raised them.
27. **Alarm colours.** The alarm border is drawn inside the tally border so both stay visible; red for no signal, black, freeze, and clip, amber for silence and format. A tile with a slate shows no badge. `silence` also rises when routed audio does not arrive.
28. **Background colour on both backends.** The layout `background` fills the canvas where no tile is (CPU fill, CUDA background kernel); tiles without a picture and the letterbox areas of `fit` tiles stay limited-range black (`CudaComposeDesc::solid*`). The RGB to YCbCr conversion is the overlay's.
29. **Text sized to the tile.** Digital clocks, timecode, labels, and slates take the largest size that fits the tile, measured with Blend2D font metrics (the 8×8 path estimates 8 px per character per size step). Captions that do not fit are cut with an ellipsis.
30. **A preview per head.** `/preview.jpg?head=<h>`; every head encodes its JPEG on its output thread, as head 1 did. On the CPU backend a large `MV_PREVIEW_WIDTH` (1920 at 5 fps on the lab) makes that thread late; the default 480 does not.
31. **Settings saved through the API apply at the next start.** The process reads its configuration once at start, also for the keys §9 marks "Restart: no"; this was already so before 1.2.0 and is not changed here. The Settings page says so and lists the saved keys in the restart banner.
32. **Import skips settings set by the environment.** An export carries every setting; importing it on a host where some keys come from the environment failed the whole import. Those keys are now skipped and listed in `skipped`.
33. **Head layout and audio-follow belong to the API.** The composer wrote its whole output status back every frame, which could undo an activation (or an `audio_follow` change) that arrived during that frame; on the CPU backend it lost most activations. `RuntimeModel::setOutput` now keeps those fields.
34. **Web UI.** Vue 3 + Vite single file as before, split into one component per tab; the look (colours, header, banners, tabs, panels, pills, buttons, tables) is the one of mxl-st2110-gateway and mxl-browser-source. The layout editor keeps its drafts outside the tab component, so tab switches and WebSocket reconnects never drop an unsaved edit; while the WebSocket is down only inputs, outputs, and alarms are polled.
35. **Older layout files are repaired, not dropped.** 1.2.0 checks audio zones, bar channels (first + count ≤ 16, the metered channels), and clock values. The layout file is read leniently: such values are corrected and logged (`layout_repaired`), while the API rejects them. A file that cannot be read at all is renamed to `layouts.json.bad`; before, the presets ran and the next save overwrote the user's file.
36. **Every head always names a layout of the book.** An import that drops a layout a head shows switches that head to the imported book's active layout (`heads_moved`); a book without layouts is refused; deleting a layout a head shows is refused; `PUT /api/v1/outputs/{h}` rejects unknown layouts (`LayoutBookStore::layout` falls back to the first layout, which hid the error) and `audio_follow` outside 0–`MV_MAX_INPUTS`.

Release 1.2.1 (platform rollout of 1.2.0):

37. **Heads start on the saved layout.** 1.2.0 started every head on `MV_OUT<h>_LAYOUT`, defaulting to `MV_ACTIVE_LAYOUT` (`2x2`), whatever the layout file said; the 1.1.3 code did the same, so a wall that came up on its saved layout before had it applied again after the start. A layout set with `PUT /api/v1/outputs/{h}` was not saved at all. The book now keeps the choice of each head (`heads`), and a head starts on: its saved layout, else `MV_OUT<h>_LAYOUT` when set (a per-head setting beats the book-wide `active`), else the book's `active` layout. `MV_ACTIVE_LAYOUT` stays what it always was for the book: the active layout until a file exists. Activate saves every head, so the latest choice wins over `MV_OUT<h>_LAYOUT` at the next start too.
38. **Unedited 1.1.x presets get the 1.2 defaults.** Agreed with the platform: a layout is migrated only when all its fields equal the 1.1.x preset of the same name, compared with `legacyPresets()` (`src/layout/migrate.cpp`), a frozen copy of the 1.1.3 presets with every tile field spelled out, so later preset or default changes do not move the comparison. Rects compare within 1e-5 (files keep six significant digits); every input count from 1 to 32 is tried, because the file may come from another `MV_MAX_INPUTS`. The book records `preset_revision: 2` instead of comparing on every start, so a preset whose bars are later switched off by hand stays that way. `layouts.json.bak` is written once and never replaced. An import runs the same migration (no backup: the imported document is the user's own copy).
39. **Editor.** "Preset defaults…" replaces a built-in preset's tiles with today's definition (`GET /api/v1/presets`) after a confirmation, unsaved until Save; "Apply to all input tiles" copies the audio bar settings of the selected tile to every input tile of the layout.

Release 1.3.0 (TSL 5.0 tally per field, platform request T2):

40. **Lamps and border show the TSL fields.** Up to 1.2.1 an input kept one colour (text, else RH, else LH) and both lamps showed it. The runtime now keeps LH, RH, and text tally as received; the left lamp shows LH, the right lamp RH, the border keeps the combined colour, and the caption text keeps it too (white when all are off), as before. The overlay is rasterised on the CPU for both backends; the CUDA path only uploads and blends that RGBA layer, so no kernel changed. Unit tests check the drawn pixels; `tests/integration/mosaic.sh` sends a TSL 5.0 datagram (UDP, screen 0, UTF-16) and samples the lamps and the caption in the CPU output flow.
41. **`tally_text` is a layout default with a tile override.** The request was "per tile and per head, defaulting from the head", fitted into the layout and tile options. A head's look is the layout it shows, so the default is a layout field (`tally_text`, false) and a tile sets `true`, `false`, or `null` (follow the layout; written as `null`). Two heads that show the same layout share it; Save as gives one head its own layout. A per-head setting (`MV_OUT<h>_...`) was not added: it would be a second place for the same choice. On the tally colour the caption text is black (white text on amber and green is hard to read). The API rejects other values with 400; a layout file is repaired and logged, as clock values are.
42. **UTF-16 labels are decoded.** The parser kept the low byte of each UTF-16 code unit, so `ü` became an invalid UTF-8 byte on the wall and in the API's `tsl_text`. UTF-16LE (with surrogate pairs) is now converted to UTF-8; a lone surrogate is U+FFFD. The platform's tally calculator sends UTF-16.
43. **TSL 3.1 lights both lamps.** A 3.1 datagram has one tally; it now sets LH as well as RH and text, so 3.1 looks as it did before 40.
44. **API field names.** `tsl_lh`, `tsl_rh`, and `tsl_text_tally` sit next to `tally` and `tsl_text` in each input (flat, like the other input fields). The `tsl_` prefix keeps them apart from the layout option `tally_text`.

Release 1.3.0, display options (platform requests after the 1.2.1 rollout):

45. **Freeze is time without a picture change.** Up to here freeze was `hash == lastHash` for two grains in a row, debounced (500 ms up, 500 ms of continuous non-freeze to clear). A source that repeats grains (the browser source paints slower than 50 fps, any 25p in 50p) flips that every grain, so once a still moment raised the alarm it never cleared (tile 6 on the platform). Freeze is now "the hash has not changed for `MV_FREEZE_MS`", measured from the last change (`FreezeDetector`, `src/media/alarm.cpp`); it is raised at once after that time and cleared after `MV_ALARM_CLEAR_MS`. The default is 2000 ms, the default duration of ffmpeg's `freezedetect`; broadcast monitors use 2–10 s (AWS MediaConnect allows 10–60 s). Values below 1000 ms are refused. The hash sampled about 4096 single pixels (`w*h/4096` apart), so a clock or a ticker could change no sample at all. It is now the luma of every second line summed per block of a 32×18 grid, each 6-pixel group weighted by its place in the block (so a small object moving inside a block on a flat background changes the sum), then FNV-1a over the 576 sums. The three scan paths (unpacked `Frame422` for alpha inputs, packed v210 on the CUDA backend, `copyV210Scan` on the CPU backend) give the same hash. The packed paths read every second line of each grain on the reader thread instead of 4096 samples; on the CUDA backend that is about half of each grain read from host memory.
46. **Local clocks use the zone database.** The image had no `tzdata` (no `/usr/share/zoneinfo`, no `/etc/localtime`), so `localtime_r` drew UTC even with `TZ=Europe/Zurich`, while the web UI drew the browser's time. The build and runtime images install `tzdata`. `MV_TIMEZONE` (an IANA name, checked against the zone database: a TZif file under `TZDIR` or `/usr/share/zoneinfo`; anything else exits 78) is written into `TZ` before any thread starts, so it overrides the container's `TZ`. C++20 `zoned_time` was not used: GCC 13 of the build image has no time zone database support. `GET /api/v1/info` reports `timezone` and `utc_offset_s`; the editor draws `local` clocks with `Intl.DateTimeFormat` in that zone, or with the offset when the browser does not know the name (a POSIX `TZ` string).
47. **Start layout per head.** Since 1.2.1 a head came back to the layout it showed last. The editor's "Use as start layout" now stores a start layout per head in the book (`start_layouts`, next to `heads`, in `layouts.json` on the config volume; export and import carry it); Activate and `PUT /api/v1/outputs/{h}` with `layout` do not move it. The platform sets neither `MV_ACTIVE_LAYOUT` nor `MV_OUT<h>_LAYOUT`, so that file decides. Precedence at start: a layout set in the environment (`MV_OUT<h>_LAYOUT`, else `MV_ACTIVE_LAYOUT`), the start layout, the last layout, `MV_OUT<h>_LAYOUT` from the config file, the book's active layout. The environment wins, as asked; that changes 1.2.1 for a deployment that sets `MV_ACTIVE_LAYOUT` or `MV_OUT<h>_LAYOUT` in the environment: such a head now always starts there, where 1.2.1 preferred the layout it showed last. A value in the config file keeps its 1.2.1 place, so a setting saved in the Settings page does not override the editor. The API reports `start_layout` and `start_layout_env` per head, and the editor disables the choice while the environment sets the start.
48. **Caption alignment.** `umd_align` (`left`, `centre`, `right`, default `left` as before) places the text in the room between the lamps. `fitText` already cut a caption wider than that room at a code point and ended it with `…` (`..` on the 8×8 path, which has no `…`); alignment works on that cut text, so a cut caption fills the room in every alignment. `centre` is spelled as the `centre` overlay option is.
49. **Caption inside the tile.** Asked for captions over the video because the outside positions are hard to arrange. `top-inside` and `bottom-inside` (the default) already were that: the composer places the picture in the whole tile whatever the caption position, and the overlay draws the band over its top or bottom lines; only slates and audio bars keep clear of the band. Nothing new was added; the editor now groups the positions as "in the tile, over the picture" and "outside the tile" and says what each does. The outside positions stay for saved layouts.
50. **Alarm display per tile.** Three tile options: `alarm_border`, `alarm_labels` (both default true), and `alarm_label_position` (six positions, default `top`), so a tile shows labels, the border, both, or neither. Up to here a tile showed one badge, the most severe alarm, in the colour of the most severe alarm. Labels now stack: one per active alarm, each in its own alarm's colour, in the old priority order. With the default options and one active alarm a tile looks exactly as before; with several it shows all of them, which is what the stacked labels were asked for. The mapping from the input's alarms to border and labels is `showAlarms()` in `src/media/overlay.cpp` (unit-tested), called by the overlay thread. Labels at a side move clear of the bars on that side and, top left, of the format and latency captions; centred labels keep the old geometry.
51. **Level scale on or off.** `audio_bar_scale` (default true, as before) hides the ticks and dBFS labels beside the bars; the bar panel gets narrower by their width. The faint marks across the bars stay, so the levels can still be read.
52. **Bars beside the picture.** `audio_bar_position` gains `left-beside` and `right-beside`. The composer places the picture in the tile less a strip at that side (`pictureRect()`, `src/layout/geometry.cpp`) before `placeTile`, so `fit` and `fill` work in the narrower area, and the strip is filled like a letterbox area (limited-range black). The CPU and CUDA paths take the same `Placement` and the same letterbox rectangle from `headMain`, so the geometry cannot differ between them; no kernel changed. The strip width comes from `barMetrics()`, shared with the overlay's `drawBars`: margins, bars, gap, and with the scale on its tick and a fixed 28 px (at 1080) for the dBFS labels, which are left out when they do not fit that room. It does not depend on font metrics or on whether audio is routed, so the picture never moves while the wall runs. A tile whose strip would take more than half its width keeps the whole picture and draws no bars. The left and right positions over the picture stay as they were.

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

`io.dmf.mxl.revision` is `MXL_REF`. `org.opencontainers.image.revision` is the git commit. Version tags `X.Y.Z`, `X.Y`, and `X` are not moved. `main` also publishes `nightly-dev` and `git-<sha>`. There is no `latest` tag.

## 8. Platform guideline G1–G14

Audit against the MXL PoC platform guideline. Status is met or N/A. Line numbers are the implementation that closes the item.

| Item | Requirement | Status | Evidence | Change |
| --- | --- | --- | --- | --- |
| G1 | Env, then one JSON file, then defaults. Unknown env ignored. Invalid values exit 78. One settings table. State only under one directory, default `/config`. Secrets never logged. | met | `src/config/config.cpp:494`, `src/config/config.cpp:290`, `src/main.cpp:185`, `SPECIFICATION.md` §9 | `MV_STATE_DIR` default `/config` holds `config.json`, `layouts.json`, and `routes.json`. This process has no secrets. |
| G2 | Scan `/Volumes/mxl`. Own output domain from `MXL_OUTPUT_DOMAIN_DIR` / `MXL_OUTPUT_DOMAIN_ID`, created if missing. A different id in `domain_def.json` is logged and not overwritten. No writes into other domains. No rewrite every start. `history_duration` configurable. | met | `src/mxlio/engine.cpp:337`, `src/mxlio/engine.cpp:360`, `src/config/config.cpp:352` | `MXL_OUTPUT_DOMAIN_*` are aliases of the existing `MV_OUTPUT_DOMAIN_*` keys. Mismatch logs `domain_id_mismatch` and keeps the file id. |
| G3 | `NMOS_SEED` UUIDv5 for node, device, sources, flows, senders, receivers, and the default domain id. `NMOS_LABEL`. `NMOS_TAGS` on node and device. Group hints stay. | met | `src/nmos/ids.cpp`, `src/nmos/node.cpp:195`, `src/nmos/node.cpp:321` | Added `NMOS_LABEL` and `NMOS_TAGS`. Seed behaviour unchanged. |
| G4 | Registry and query addresses. Query defaults to the registry and registration port + 1. `NMOS_DNS_SD` defaults false and disables browse and mDNS advertisement (`pri` and `highest_pri` = max int). No Avahi or D-Bus requirement while that is false. | met | `src/config/config.cpp:608`, `src/nmos/node.cpp:208` | Query host and port are settings. DNS-SD off does not call browse or register. The client library stays linked because nmos-cpp references it; the daemon is not required. |
| G5 | Announced addresses are IP literals from `NMOS_HOST_ADDRESS`. Default is the first non-loopback IPv4. Never a hostname, `0.0.0.0`, or `127.0.0.1`. | met | `src/config/config.cpp:413`, `src/nmos/node.cpp:205` | `HOST_ID` remains the label and seed, not the href. SDP, ICE, and SRT are N/A: this process does not announce them. The UI has no address to copy. |
| G6 | Every listen port is an env setting, including the NMOS WebSocket at `NMOS_PORT+1`. Bind failure exits 75. | met | `src/config/config.cpp:175`, `src/mxlio/engine.cpp:1220`, `src/main.cpp:142` | TSL `bind` failures throw before the threads start and the process exits 75. `WEB_ENABLE=false` no longer falls through. |
| G7 | `/livez`, `/readyz` (serving, and registered when a registry is set), `/metrics` with prefix `mxl_multiviewer_`. | met | `src/ops/api.cpp:478`, `src/ops/api.cpp:487`, `src/ops/metrics.cpp:74` | Readiness uses the Query API at `NMOS_QUERY_ADDRESS`:`NMOS_QUERY_PORT`. |
| G8 | SIGTERM within `SHUTDOWN_TIMEOUT_S`: stop media, DELETE the node, optionally remove only the output domain, exit 143. | met | `src/main.cpp:174`, `src/nmos/node.cpp:531`, `src/mxlio/engine.cpp:1581` | Tombstones are IS-04 resources only, so nmos-cpp sends the DELETEs. `MXL_CLEANUP_ON_EXIT` defaults false. |
| G9 | Senders report `mxl_domain_id` and `mxl_flow_id`. Receivers accept the staged PATCH. `master_enable: false` stops the reader. SHOULD: the route survives a restart. | met | `src/nmos/node.cpp:417`, `src/mxlio/engine.cpp:1253` | Routes persist in `<MV_STATE_DIR>/routes.json` and the readers resume. Since 1.1.1 the restored route is also the IS-05 active and staged document and the IS-04 subscription (`src/nmos/node.cpp` `restoreRoute`). |
| G10 | `GET /api/v1/config/export` and `POST /api/v1/config/import`. Secrets omitted unless requested. | met | `src/ops/api.cpp:358`, `src/ops/api.cpp:407` | The document is settings, layouts, and routes. There are no secrets, so `secrets` is false and nothing is omitted. Routes apply on the next start. |
| G11 | Actions builds and pushes `ghcr.io/leeo86/mxl-multiviewer`. `main`: `git-<sha>` and `nightly-dev`. Tag `vX.Y.Z`: `X.Y.Z`, `X.Y`, `X`. uid 1000. OCI labels. Version tags are not moved. | met | `.github/workflows/container.yaml:38`, `docker/Dockerfile:85` | `latest` is not published. Example manifests reference `1.0.0`. |
| G12 | Pod network, standard env, probes, grace period, MXL hostPath, writable `/config`, no `hostIPC`. | met | `deploy/mxl-multiviewer.yaml:42`, `deploy/mxl-multiviewer-gpu.yaml` | Host network removed. The GPU manifest adds the nvidia runtime and one GPU. |
| G13 | README settings, ports, exit codes, API, platform run. CHANGELOG 1.0.0. SPEC matches the code. | met | `README.md`, `CHANGELOG.md`, `SPECIFICATION.md` | Written with this release. |
| G14 | Unit tests for parsing and the new behaviour. Integration covers start, ready, SIGTERM, deregister, and domain removal. CI green. | met | `tests/unit/test_config.cpp:31`, `tests/integration/mosaic.sh:171` | The mosaic sets `MXL_CLEANUP_ON_EXIT=true` and requires exit 143, a removed domain, and query 404. |

On a GPU host with the NVIDIA container toolkit the process needs the driver injected at start (`--gpus all`, `docker/docker-compose.gpu.yaml`, or `deploy/mxl-multiviewer-gpu.yaml`). The toolkit is what provides `libcuda`. The image already contains the compositor.
