# mxl-multiviewer

Broadcast multiviewer for the MXL proof-of-concept platform. It composites up to 32 NMOS inputs into one uncompressed `video/v210` MXL output (and an optional `audio/float32` follow), timed from TAI rather than a media-framework clock.

The previous Rust/GStreamer 2×2 prototype is gone. `docs/audit.md` says why. Behaviour is `SPECIFICATION.md`. How this tree differs from that text is `IMPLEMENTATION_PLAN.md`.

The output is an NMOS sender. Route it to `mxl-decklink` for an SDI wall or to `mxl-webrtc-monitor` for a browser. The outputs are uncompressed; browsers only get a low-rate preview: a JPEG per head, or with `MV_PREVIEW_MODE=webrtc` one H.264 WebRTC stream of all heads (see Preview below).

## Build

Linux, CMake ≥ 3.24, GCC ≥ 12 or Clang ≥ 16, Node.js ≥ 20 (admin UI), libcurl and libwebp (`libcurl4-openssl-dev libwebp-dev` on Ubuntu) for image tiles, and FFmpeg (`libavcodec-dev libavformat-dev libavutil-dev`) for the WebRTC preview. MXL is `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, built with `-DMXL_ENABLE_FABRICS_OFI=OFF`.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/mxl \
  -DMV_WITH_NMOS=ON -DNMOS_CPP_DIR=/path/to/nmos-cpp/Development
cmake --build build -j
```

`MV_WITH_NMOS=ON` needs the same nmos-cpp commit as the siblings (`fe303849527394b03bdedc8f161f377fe458bb62`). Without it, `NMOS_ENABLE=true` is rejected.

```bash
docker build -f docker/Dockerfile .
```

Image `ghcr.io/leeo86/mxl-multiviewer`. A `vX.Y.Z` tag publishes `X.Y.Z`, `X.Y`, and `X` and those tags are not moved. `main` publishes `nightly-dev` and `git-<sha>`. There is no `latest` tag. Labels include `org.opencontainers.image.source`, `org.opencontainers.image.revision`, `org.opencontainers.image.licenses`, and `io.dmf.mxl.revision` (the MXL pin). The process runs as uid 1000.

## Run

On the platform the pod network is enough. Mount the MXL root read-write (this process creates its own output domain and reads every other domain, including fabrics mirrors) and a writable `/config`. uid/gid 1000, `supplementalGroups: [1000]`, no `hostIPC`.

```bash
MXL_DOMAIN_SCAN_PATH=/Volumes/mxl \
MXL_OUTPUT_DOMAIN_DIR=/Volumes/mxl/multiviewer \
MV_STATE_DIR=/config \
NMOS_REGISTRY_ADDRESS=10.0.0.10 \
NMOS_HOST_ADDRESS=10.0.0.20 \
NMOS_SEED=sport-sa-mv1 \
NMOS_LABEL=mv1 \
NMOS_ENABLE=true \
./build/mxl-multiviewer
```

`MXL_OUTPUT_DOMAIN_DIR` and `MXL_OUTPUT_DOMAIN_ID` are aliases of `MV_OUTPUT_DOMAIN_DIR` and `MV_OUTPUT_DOMAIN_ID`. A deployed config that still sets the `MV_` names keeps working. `NMOS_HOST_ADDRESS` is the address in the node `href`, `api.endpoints[].host`, and IS-05 control hrefs. The platform sets it to the pod IP. When it is unset the process uses the first non-loopback IPv4. `HOST_ID` is only a label and seed.

### Ports

| Port | Setting | Default |
| --- | --- | --- |
| Web, REST, health, metrics | `WEB_PORT` | 8110 |
| NMOS Node and Connection | `NMOS_PORT` | 3262 |
| NMOS WebSocket | `NMOS_PORT` + 1 | 3263 |
| TSL UMD 5.0 UDP | `TSL_UDP_PORT` | 8910 |
| TSL UMD 5.0 TCP | `TSL_TCP_PORT` | 8911 |
| Built-in MediaMTX RTSP ingest (127.0.0.1) | `MEDIAMTX_RTSP_PORT` | 8754 |
| Built-in MediaMTX WHEP (WebRTC) | `MEDIAMTX_WHEP_PORT` | 8789 |
| Built-in MediaMTX HLS | `MEDIAMTX_HLS_PORT` | 8788 |
| Built-in MediaMTX ICE, UDP and TCP | `MEDIAMTX_ICE_UDP_PORT` | 8389 |

The MediaMTX ports are only open with `MV_PREVIEW_MODE=webrtc` and no `PREVIEW_PUBLISH_URL`; they miss mxl-webrtc-monitor (8554, 8889, 8888, 8189, 9997, 9998) and the FlowXer engine (8654, 8989, 8988, 8289, 9897). Two instances on one host need distinct values. A port that cannot be bound exits 75. These defaults miss the ports already used by mxl-decklink, mxl-st2110-gateway, mxl-fabrics-agent, mxl-webrtc-monitor, and FlowXer.

### API

| Method | Path |
| --- | --- |
| GET | `/api/v1/info` |
| GET | `/api/v1/inputs` |
| GET | `/api/v1/outputs` |
| GET | `/api/v1/layouts` |
| GET | `/api/v1/presets` |
| PUT | `/api/v1/layouts/{name}` |
| DELETE | `/api/v1/layouts/{name}` |
| POST | `/api/v1/layouts/{name}/activate` |
| PUT | `/api/v1/outputs/{h}` |
| GET | `/api/v1/alarms` |
| GET | `/api/v1/images` |
| GET, PUT, DELETE | `/api/v1/images/{name}` |
| GET | `/api/v1/events` (WebSocket) |
| GET | `/preview.jpg` (`?head=<h>` for heads 2 to 4; JPEG mode only) |
| GET | `/api/v1/preview/map` |
| GET | `/widgets` |
| GET | `/widget/head`, `/widget/tile-editor` |
| GET | `/statusz` |
| GET, PUT | `/api/v1/config` |
| GET | `/api/v1/config/export` |
| POST | `/api/v1/config/import` |
| GET | `/api/v1/config/env` |
| GET | `/livez` |
| GET | `/readyz` |
| GET | `/metrics` |

`GET /api/v1/config/export` returns one JSON document (`version`, `secrets`, `settings`, `layouts`, `routes`). This process has no secrets, so `secrets` is false and nothing is left out. `POST /api/v1/config/import` restores that document. Settings set by the environment are skipped (listed in `skipped`). Routes in the file apply on the next start.

`GET /api/v1/alarms` lists the active alarms with `severity` (`red` or `amber`) and `since` (Unix milliseconds). `GET /api/v1/inputs` (and the WebSocket and `/statusz`) carry per channel `ppm_dbfs`, `hold_dbfs`, `rms_dbfs`, and `clip`, and per input the TSL state: `tsl_lh`, `tsl_rh`, `tsl_text_tally` (0 off, 1 red, 2 green, 3 amber), `tally` (the border colour), and `tsl_text` (the label). Each output in `GET /api/v1/outputs` has `start_layout` and `start_layout_env` (a start layout the environment sets); `PUT /api/v1/outputs/{h}` takes `start_layout` (a layout name, or `null` to clear it). `GET /api/v1/info` also has `label`, `grid`, `preview_fps`, `preview_mode`, `preview` (WHEP and HLS URLs of the WebRTC preview), `hold_ms`, `timezone`, and `utc_offset_s`. Layout names in paths are percent-encoded (`2+8` is `2%2B8`).

`/readyz` is 200 when the composer heartbeat is fresh and, if a registry address is set, the Query API currently lists the node. `/metrics` is Prometheus text with the prefix `mxl_multiviewer_`.

### Settings

Precedence is environment, then `MV_CONFIG_FILE` (one flat JSON object of strings), then the defaults. Unknown environment variables are ignored. Unknown file keys and invalid values exit 78. State this process writes for itself lives under `MV_STATE_DIR` (default `/config`).

| Key | Default | Restart |
| --- | --- | --- |
| `HOST_ID` | hostname | yes |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | yes |
| `MV_OUTPUT_DOMAIN_DIR` (`MXL_OUTPUT_DOMAIN_DIR`) | `/Volumes/mxl/multiviewer` | yes |
| `MV_OUTPUT_DOMAIN_ID` (`MXL_OUTPUT_DOMAIN_ID`) | empty, UUIDv5 from `NMOS_SEED` | yes |
| `MV_STATE_DIR` | `/config` (also `images/`) | yes |
| `MXL_CLEANUP_ON_EXIT` | `false` | yes |
| `MV_BACKEND` | `auto` | yes |
| `MV_MAX_INPUTS` | `16` | yes |
| `MV_OUTPUTS` | `1` (at most 4) | yes |
| `MV_OUTPUT_FORMAT` | `1920x1080p50` | no |
| `MV_INPUT_OFFSET_GRAINS` | `2` | no |
| `MV_HOLD_MS` | `1000` | no |
| `MV_HISTORY_DURATION_NS` | `200000000` | yes |
| `MV_LAYOUTS_FILE` | empty (`<MV_STATE_DIR>/layouts.json`) | no |
| `MV_ACTIVE_LAYOUT` | `2x2` | no |
| `MV_AUDIO_CHANNELS` | `2` | no |
| `MV_AUDIO_FOLLOW` | `1` | no |
| `MV_OVERLAY_HZ` | `25` | no |
| `MV_PREVIEW_FPS` | `5` | no |
| `MV_PREVIEW_WIDTH` | `480` | no |
| `MV_PREVIEW_MODE` | `jpeg` (`webrtc`) | yes |
| `PREVIEW_PUBLISH_URL` | empty (built-in MediaMTX) | yes |
| `PREVIEW_PATH_PREFIX` | `mxl-multiviewer` | yes |
| `PREVIEW_WHEP_URL` | empty (built-in MediaMTX) | yes |
| `PREVIEW_HLS_URL` | empty (built-in MediaMTX) | yes |
| `WIDGET_FRAME_ANCESTORS` | `'self'` | yes |
| `MEDIAMTX_RTSP_PORT` | `8754` | yes |
| `MEDIAMTX_WHEP_PORT` | `8789` | yes |
| `MEDIAMTX_HLS_PORT` | `8788` | yes |
| `MEDIAMTX_ICE_UDP_PORT` | `8389` | yes |
| `MV_GRID` | `24` | no |
| `MV_BLACK_Y` | `32` | no |
| `MV_SILENCE_DBFS` | `-60` | no |
| `MV_CLIP_LINEAR` | `0.999` | no |
| `MV_ALARM_DEBOUNCE_MS` | `500` | no |
| `MV_ALARM_CLEAR_MS` | `500` | no |
| `MV_FREEZE_MS` | `2000` | no |
| `MV_BACKGROUND_FILE` | empty | no |
| `MV_TIMEZONE` | empty (`TZ`) | yes |
| `MV_CONFIG_FILE` | empty (`<MV_STATE_DIR>/config.json`) | yes |
| `NMOS_ENABLE` | `true` | yes |
| `NMOS_REGISTRY_ADDRESS` | empty | yes |
| `NMOS_REGISTRY_PORT` | `3210` | yes |
| `NMOS_QUERY_ADDRESS` | registry address | yes |
| `NMOS_QUERY_PORT` | registry port + 1 | yes |
| `NMOS_DNS_SD` | `false` | yes |
| `NMOS_PORT` | `3262` | yes |
| `NMOS_SEED` | `<HOST_ID>-multiviewer` | yes |
| `NMOS_LABEL` | empty (`HOST_ID`) | yes |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | yes |
| `NMOS_TAGS` | `{}` | yes |
| `WEB_ENABLE` | `true` | yes |
| `WEB_PORT` | `8110` | yes |
| `TSL_ENABLE` | `true` | yes |
| `TSL_UDP_PORT` | `8910` | yes |
| `TSL_TCP_PORT` | `8911` | yes |
| `TSL_V31` | `false` | yes |
| `TSL_SCREEN` | `-1` | no |
| `TSL_MAP` | empty | no |
| `LOG_LEVEL` | `info` | no |
| `LOG_FORMAT` | `json` | yes |
| `SHUTDOWN_TIMEOUT_S` | `10` | yes |

Per head `h` from 2 to 4, `MV_OUT<h>_FORMAT`, `MV_OUT<h>_LAYOUT`, `MV_OUT<h>_AUDIO_FOLLOW`, and `MV_OUT<h>_AUDIO_CHANNELS` override the unscoped keys. `MV_OUT1_*` is accepted for head 1. Meanings are in `SPECIFICATION.md` §9.

Routing is IS-05 only. The UI has no source picker. A layout names input numbers; changing the layout does not change the route.

```bash
# after the node is up, receiver id is in GET /api/v1/info
curl -X PATCH -H 'Content-Type: application/json' \
  -d '{"master_enable":true,"activation":{"mode":"activate_immediate"},"transport_params":[{"mxl_domain_id":"<domain>","mxl_flow_id":"<flow>"}]}' \
  http://127.0.0.1:3262/x-nmos/connection/v1.2/single/receivers/<id>/staged
```

The web UI is on `http://<host>:8110/` (see Web UI below).

`MV_BACKEND=auto` uses the CUDA compositor when the binary was built with nvcc and a device is visible, otherwise CPU. The image from `docker/Dockerfile` is built with CUDA 12.8 (`sm_75`, `sm_86`, `sm_89`) and links the CUDA runtime statically, so it still starts on a machine with no GPU. A GPU host also needs the NVIDIA container toolkit to inject the driver:

```bash
docker run --gpus all --network host \
  -e MV_BACKEND=auto -e MXL_DOMAIN_SCAN_PATH=/Volumes/mxl \
  -e MXL_OUTPUT_DOMAIN_DIR=/Volumes/mxl/multiviewer \
  -v /Volumes/mxl:/Volumes/mxl -v mv-config:/config \
  ghcr.io/leeo86/mxl-multiviewer:1.4.1
```

The image sets `NVIDIA_DRIVER_CAPABILITIES=compute,video,utility` (video: NVENC for the WebRTC preview); a deployment that overrides it must keep `video`. `--network host` is the single-machine form. The platform Deployment uses the pod network and sets `NMOS_HOST_ADDRESS` from the pod IP.

`docker/docker-compose.gpu.yaml` is the Compose form of that (`--gpus` via a device reservation). `deploy/mxl-multiviewer-gpu.yaml` is the Kubernetes form (`runtimeClassName: nvidia`, `nvidia.com/gpu: 1`). `deploy/mxl-multiviewer.yaml` does not request a GPU; `auto` stays on CPU. `MV_BACKEND=cuda` with no device exits 75. `MV_BACKEND=cuda` on a binary built without nvcc (the CI job, not the image) exits 78.

### Web UI

Open `http://<host>:8110/`. The tabs keep their place in the address (`#layout`), so a reload stays on the tab.

- **Preview**: the output picture at `MV_PREVIEW_FPS` (its JPEG, or its region of the WebRTC stream), a head selector with more than one head, the layout on air with Activate, and the output counters.
- **Layout**: the layout editor. Tiles snap to the `MV_GRID` grid; drag to move, drag the corner to resize, arrow keys move by one cell, Shift + arrows resize, Delete removes, Ctrl+D duplicates. Add input, clock, label, image, and empty tiles; the inspector sets what a tile shows (input and scale, analogue or digital clock with time zone and timecode, label text), its alarm display (border, labels and where they stack), its caption (UMD: the name strip under the picture, from the NMOS sender label, fixed text, or TSL, aligned left, centre, or right; at the top or bottom, over the video or in its own strip), audio bars (left, centre or right, over the video or in their own strip), tally, and overlays, and the layout's background colour and text tally default. Save, Save as, Discard, New, Delete (not the built-in presets), Preset defaults (reset a built-in preset), Activate, Use as start layout (per output), and Import / export of one layout or all of them. "Apply to all input tiles" copies one tile's audio bar settings to the others. Unsaved edits survive tab switches and lost connections.
- **Inputs**: video and audio state, source, format, live PPM levels, alarms, and the receiver ids to route to (IS-05 only).
- **Alarms**: active alarms with severity and since when.
- **Settings**: every setting with its origin (ENV, FILE, DEFAULT); environment values are read-only, saved values go into the configuration file and apply at the next start. Export as JSON or `KEY=value` (download or copy), import an exported document.

A head starts on (first match wins): `MV_OUT<h>_LAYOUT` or `MV_ACTIVE_LAYOUT` set in the environment; its start layout ("Use as start layout" in the layout editor, `PUT /api/v1/outputs/{h}` with `start_layout`, kept in `layouts.json`); the layout last chosen for it (Activate, or `PUT /api/v1/outputs/{h}` with `layout`); `MV_OUT<h>_LAYOUT` from the config file; the active layout of `layouts.json`. Otherwise `MV_ACTIVE_LAYOUT` only applies until that file exists. The platform sets neither, so the start layout chosen in the editor is what a restarted container shows. Built-in presets that a 1.1.x release saved unedited get the current preset defaults at the first start (the old file stays as `layouts.json.bak`).

The overlay draws audio bars on input tiles (built-in presets have them on): a PPM scale from 0 to −60 dBFS (`audio_bar_scale: false` hides it), over the picture at its left or right edge or in the centre, or (`audio_bar_overlay: false`) in their own strip at the left or right of the tile, the picture narrower, a 2 s peak hold, and a clip light. Dim, crossed-out bars mean no audio is routed to that input. The freeze alarm rises when the picture has not changed for `MV_FREEZE_MS` (default 2 s); a source that repeats frames (25p in 50p, a browser source) is not frozen. Nothing of a tile is drawn outside its rectangle: the caption bar is at the top or bottom inside it (`umd_position`), over the picture or, with `umd_overlay: false`, in its own strip with the picture placed in the rest. With both strips the picture gets the tile less both, and fit or fill work on that. Layouts with the 1.3.0 values (`bottom-inside`, `top-outside`, `right-beside`, …) are still accepted and migrated; `top-outside` and `bottom-outside` now are strips inside the tile. After `MV_HOLD_MS` without a frame the tile shows `NO SIGNAL` (or `WAITING` while the flow is missing); an input without a video route shows `NOT ROUTED`.

### Preview

`MV_PREVIEW_MODE=jpeg` (default) encodes a JPEG of each head at `MV_PREVIEW_FPS` and `MV_PREVIEW_WIDTH` (`/preview.jpg?head=<h>`) and publishes nothing. `MV_PREVIEW_MODE=webrtc` encodes no JPEG; instead every head is scaled into one 1920×1080 picture (one head fills it; two to four heads get a 960×540 quarter each, 2×2 in reading order) and that picture is encoded once as H.264, with NVENC on a GPU host (x264 only when NVENC cannot be opened, which is logged), 4 Mbit/s at `MV_PREVIEW_FPS`. On the CUDA backend the heads are scaled on the GPU in the compose path. `GET /api/v1/preview/map` gives each head's region; the UI plays the stream once (WHEP) and shows each head in a box of its region's aspect ratio, the `<video>` scaled and moved with a CSS transform so only that region shows (every browser).

The stream is published over RTSP to MediaMTX, which serves WHEP and HLS (the preview contract shared with mxl-webrtc-monitor):

| Setting | Meaning |
| --- | --- |
| `PREVIEW_PUBLISH_URL` | `rtsp://host:port` of a shared MediaMTX: the stream goes there and no MediaMTX is started. Empty: the image's MediaMTX runs as a child process (restarted when it exits, stopped with the multiviewer) on the `MEDIAMTX_*` ports |
| `PREVIEW_PATH_PREFIX` | path prefix; the stream is `<prefix>/heads` (default `mxl-multiviewer/heads`) |
| `PREVIEW_WHEP_URL`, `PREVIEW_HLS_URL` | public bases the page plays from (`<base>/<prefix>/heads/whep`, `.../index.m3u8`); empty: the built-in MediaMTX on the page's host name |

`/statusz` shows `preview`: `mode`, and with WebRTC `publish` (`own`, `shared`), `publish_url`, `path`, `state` (`connecting`, `publishing`, `error`), `error`, `encoder` (`nvenc`, `x264`), `frames`, and `mediamtx` (`running`, `restarts`) in own mode. The metrics are `mxl_multiviewer_preview_mode{mode}`, `preview_publish_mode{mode}`, `preview_publish_state{state}`, `preview_encoder{encoder}`, `preview_frames_total`, `preview_encode_seconds`, `preview_seconds{head}` and `compose_gpu_seconds{stage="preview"}`. Readiness does not depend on the preview. WebRTC needs UDP (or TCP) from the browser to the ICE port of the MediaMTX that serves it.

### Widgets

Operator screens embed parts of the multiviewer. `GET /widgets` lists them with their parameters (a JSON schema) and minimum size:

| Widget | Parameters | Minimum | Shows |
| --- | --- | --- | --- |
| `head` | `head` (1..`MV_OUTPUTS`) | 480×270 | one head's preview: its WebRTC region, or its JPEG |
| `tile-editor` | `head` | 600×400 | the tiles of the layout the head shows and the tile inspector, with Save and Discard |

`/widget/<id>?head=<h>[&theme=dark|light|transparent]` is the page without the app around it; it uses this multiviewer's API on its own origin and posts `widget-ready` and `widget-size` to the framing page. `WIDGET_FRAME_ANCESTORS` (default `'self'`) is the CSP `frame-ancestors` of the `/widget` routes (they send no `X-Frame-Options`); origins listed there exactly (or `*`) also get `Access-Control-Allow-Origin` on `GET /widgets`.

### Tally (TSL)

TSL UMD 5.0 arrives on `TSL_UDP_PORT` and `TSL_TCP_PORT` (TSL 3.1 on UDP with `TSL_V31=true`). With an empty `TSL_MAP`, display index `i` is input `i+1`; `TSL_SCREEN` (−1: every screen) picks one screen. One index drives every tile on every head that shows that input. UTF-16 labels are read as UTF-16 (umlauts included).

| TSL field | On the wall |
| --- | --- |
| LH tally | left lamp of the caption |
| RH tally | right lamp of the caption |
| text tally | the border (RH, then LH, while it is off); the caption background where `tally_text` is on |
| text | the caption of tiles with `umd_source: tsl` |

| Option | Where | Default | Meaning |
| --- | --- | --- | --- |
| `tally_border` | tile | `true` | border in the tally colour |
| `tally_lamp` | tile | `true` | LH and RH lamps at the ends of the caption; an off lamp is not drawn |
| `tally_text` | layout | `false` | the text tally colours the caption background (black text on it) |
| `tally_text` | tile | `null` | `true` or `false` overrides the layout, `null` follows it |

The heads share one TSL input, so per head the choice comes from the layout the head shows: give a head its own layout (Save as) to turn text tally backgrounds on for it alone.

On the platform the tally calculator sends TSL 5.0 to the Service on 8910/udp: screen 0, every change and a refresh each second, UTF-16 labels, index n−1 for input n.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | `--help` |
| 75 | a port could not be bound, or another startup failure |
| 78 | invalid configuration |
| 143 | SIGTERM or SIGINT, including a shutdown that ran past `SHUTDOWN_TIMEOUT_S` |

### Platform

Image tiles show a picture from an http(s) URL or one uploaded in the layout editor (`PUT /api/v1/images/{name}`, kept in `<MV_STATE_DIR>/images/`): PNG, JPEG, GIF (animated GIFs play), or WebP, at most 8 MiB, 4096 × 4096 pixels, and 32 megapixels over all frames. The multiviewer fetches a URL itself (3 s to connect, 10 s in all, through `https_proxy`/`http_proxy` from its environment, certificates checked) on a worker thread, so a slow URL never holds up the output; a failed picture shows `NO IMAGE` with the reason and is tried again after 30 s. Image tiles are drawn over the video tiles.

For the WebRTC preview the platform sets `MV_PREVIEW_MODE=webrtc`, `PREVIEW_PUBLISH_URL` (its shared MediaMTX), `PREVIEW_PATH_PREFIX` (unique per function, e.g. `<production>/<function>`), `PREVIEW_WHEP_URL` and `PREVIEW_HLS_URL` (the public bases of that MediaMTX), and `WIDGET_FRAME_ANCESTORS` (the designer and operator-screen origins). A GPU (CUDA backend) gets NVENC when the container toolkit injects the video capability, which the image asks for; without a GPU the CPU backend encodes with x264.

Clock tiles with local time use `MV_TIMEZONE` (an IANA name such as `Europe/Zurich`), else the container's `TZ`, else UTC; the image has the zone database (`tzdata`). The platform sets `TZ=Europe/Zurich`. `GET /api/v1/info` reports the zone (`timezone`, `utc_offset_s`), and the layout editor draws local clocks in it.

`deploy/mxl-multiviewer.yaml` is the pod-network Deployment: MXL root hostPath `/Volumes/mxl`, writable `/config`, `NMOS_HOST_ADDRESS` from `status.podIP`, probes on `/livez` and `/readyz`, and `terminationGracePeriodSeconds` above `SHUTDOWN_TIMEOUT_S`. Set `MXL_CLEANUP_ON_EXIT=true` so production-down sees the output domain disappear. Replace the example emptyDir with a persistent volume when `/config` must survive a reschedule. The GPU file adds `runtimeClassName: nvidia` and one GPU; the process still starts on CPU when the device is missing and `MV_BACKEND=auto`.

## Tests

```bash
./build/unit-tests
MXL_LIB_DIR=/opt/mxl/lib tests/integration/mosaic.sh \
  build/mxl-multiviewer build/mxl-mv-writer build/mxl-mv-sample
MXL_LIB_DIR=/opt/mxl/lib MEDIAMTX_BIN=/path/to/mediamtx tests/integration/preview.sh build/mxl-multiviewer
tests/nmos/amwa.sh   # IS-04-01, IS-05-01, IS-05-02; needs Docker, not default CI
```

Hardware targets are in `docs/performance.md`. They have not been measured on an A4000, an L4, or a Precision 3930-class CPU yet. A lab run on an NVIDIA A16 (2026-10-03) misses them; the numbers and the profile are in that file. Without a GPU, 16 inputs → 1080p50 keep real time on about 6 cores of a 2× Xeon Gold 6136 (1.1.2, same file).

## Deploy

`docker/docker-compose.demo.yaml` is a registry stand-in, a pattern writer, and the multiviewer on one machine. `docker/docker-compose.host.yaml` is that machine with a host MXL root. `docker/docker-compose.gpu.yaml` adds one NVIDIA GPU on top of the host file. The platform shape is `deploy/mxl-multiviewer.yaml` (pod network). `deploy/mxl-multiviewer-gpu.yaml` adds `runtimeClassName: nvidia` and a `nvidia.com/gpu` limit.

## License

Apache-2.0. Vendored `third_party/doctest`, `picojson`, `stb`, and `font8x8_basic.h` keep their own notices. DejaVu Sans in `third_party/dejavu/` is under its own license. Blend2D 0.21.2 and the asmjit it ships are fetched at configure time and linked statically. Image tiles link the system libraries libcurl (curl licence, MIT-style) and libwebp with libwebpdemux (BSD-3-Clause). The WebRTC preview links the system FFmpeg libraries (LGPL; Ubuntu builds them with x264, so the image's libraries are GPL-2.0-or-later). The image carries the MediaMTX binary (MIT, licence in `/usr/share/doc/mediamtx/LICENSE`).
