# mxl-multiviewer

Broadcast multiviewer for the MXL proof-of-concept platform. It composites up to 32 NMOS inputs into one uncompressed `video/v210` MXL output (and an optional `audio/float32` follow), timed from TAI rather than a media-framework clock.

The previous Rust/GStreamer 2×2 prototype is gone. `docs/audit.md` says why. Behaviour is `SPECIFICATION.md`. How this tree differs from that text is `IMPLEMENTATION_PLAN.md`.

The output is an NMOS sender. Route it to `mxl-decklink` for an SDI wall or to `mxl-webrtc-monitor` for a browser. This process does not encode and does not speak WebRTC. The admin UI only gets a low-rate JPEG.

## Build

Linux, CMake ≥ 3.24, GCC ≥ 12 or Clang ≥ 16, Node.js ≥ 20 (admin UI). MXL is `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, built with `-DMXL_ENABLE_FABRICS_OFI=OFF`.

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

Two instances on one host need distinct values. A port that cannot be bound exits 75. These defaults miss the ports already used by mxl-decklink, mxl-st2110-gateway, mxl-fabrics-agent, mxl-webrtc-monitor, and FlowXer.

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
| GET | `/api/v1/events` (WebSocket) |
| GET | `/preview.jpg` (`?head=<h>` for heads 2 and 3) |
| GET, PUT | `/api/v1/config` |
| GET | `/api/v1/config/export` |
| POST | `/api/v1/config/import` |
| GET | `/api/v1/config/env` |
| GET | `/livez` |
| GET | `/readyz` |
| GET | `/metrics` |

`GET /api/v1/config/export` returns one JSON document (`version`, `secrets`, `settings`, `layouts`, `routes`). This process has no secrets, so `secrets` is false and nothing is left out. `POST /api/v1/config/import` restores that document. Settings set by the environment are skipped (listed in `skipped`). Routes in the file apply on the next start.

`GET /api/v1/alarms` lists the active alarms with `severity` (`red` or `amber`) and `since` (Unix milliseconds). `GET /api/v1/inputs` (and the WebSocket and `/statusz`) carry per channel `ppm_dbfs`, `hold_dbfs`, `rms_dbfs`, and `clip`, and per input the TSL state: `tsl_lh`, `tsl_rh`, `tsl_text_tally` (0 off, 1 red, 2 green, 3 amber), `tally` (the border colour), and `tsl_text` (the label). `GET /api/v1/info` also has `label`, `grid`, `preview_fps`, and `hold_ms`. Layout names in paths are percent-encoded (`2+8` is `2%2B8`).

`/readyz` is 200 when the composer heartbeat is fresh and, if a registry address is set, the Query API currently lists the node. `/metrics` is Prometheus text with the prefix `mxl_multiviewer_`.

### Settings

Precedence is environment, then `MV_CONFIG_FILE` (one flat JSON object of strings), then the defaults. Unknown environment variables are ignored. Unknown file keys and invalid values exit 78. State this process writes for itself lives under `MV_STATE_DIR` (default `/config`).

| Key | Default | Restart |
| --- | --- | --- |
| `HOST_ID` | hostname | yes |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | yes |
| `MV_OUTPUT_DOMAIN_DIR` (`MXL_OUTPUT_DOMAIN_DIR`) | `/Volumes/mxl/multiviewer` | yes |
| `MV_OUTPUT_DOMAIN_ID` (`MXL_OUTPUT_DOMAIN_ID`) | empty, UUIDv5 from `NMOS_SEED` | yes |
| `MV_STATE_DIR` | `/config` | yes |
| `MXL_CLEANUP_ON_EXIT` | `false` | yes |
| `MV_BACKEND` | `auto` | yes |
| `MV_MAX_INPUTS` | `16` | yes |
| `MV_OUTPUTS` | `1` | yes |
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
| `MV_GRID` | `24` | no |
| `MV_BLACK_Y` | `32` | no |
| `MV_SILENCE_DBFS` | `-60` | no |
| `MV_CLIP_LINEAR` | `0.999` | no |
| `MV_ALARM_DEBOUNCE_MS` | `500` | no |
| `MV_ALARM_CLEAR_MS` | `500` | no |
| `MV_FREEZE_MS` | `2000` | no |
| `MV_BACKGROUND_FILE` | empty | no |
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

Per head `h` ≥ 2, `MV_OUT<h>_FORMAT`, `MV_OUT<h>_LAYOUT`, `MV_OUT<h>_AUDIO_FOLLOW`, and `MV_OUT<h>_AUDIO_CHANNELS` override the unscoped keys. `MV_OUT1_*` is accepted for head 1. Meanings are in `SPECIFICATION.md` §9.

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
docker run --gpus all --network host -e NVIDIA_DRIVER_CAPABILITIES=compute,utility \
  -e MV_BACKEND=auto -e MXL_DOMAIN_SCAN_PATH=/Volumes/mxl \
  -e MXL_OUTPUT_DOMAIN_DIR=/Volumes/mxl/multiviewer \
  -v /Volumes/mxl:/Volumes/mxl -v mv-config:/config \
  ghcr.io/leeo86/mxl-multiviewer:1.3.0
```

`--network host` is the single-machine form. The platform Deployment uses the pod network and sets `NMOS_HOST_ADDRESS` from the pod IP.

`docker/docker-compose.gpu.yaml` is the Compose form of that (`--gpus` via a device reservation). `deploy/mxl-multiviewer-gpu.yaml` is the Kubernetes form (`runtimeClassName: nvidia`, `nvidia.com/gpu: 1`). `deploy/mxl-multiviewer.yaml` does not request a GPU; `auto` stays on CPU. `MV_BACKEND=cuda` with no device exits 75. `MV_BACKEND=cuda` on a binary built without nvcc (the CI job, not the image) exits 78.

### Web UI

Open `http://<host>:8110/`. The tabs keep their place in the address (`#layout`), so a reload stays on the tab.

- **Preview**: the output picture at `MV_PREVIEW_FPS`, a head selector with more than one head, the layout on air with Activate, and the output counters.
- **Layout**: the layout editor. Tiles snap to the `MV_GRID` grid; drag to move, drag the corner to resize, arrow keys move by one cell, Shift + arrows resize, Delete removes, Ctrl+D duplicates. Add input, clock, label, and empty tiles; the inspector sets what a tile shows (input and scale, analogue or digital clock with time zone and timecode, label text), its caption (UMD: the name strip under the picture, from the NMOS sender label, fixed text, or TSL), audio bars, tally, and overlays, and the layout's background colour and text tally default. Save, Save as, Discard, New, Delete (not the built-in presets), Preset defaults (reset a built-in preset), Activate, and Import / export of one layout or all of them. "Apply to all input tiles" copies one tile's audio bar settings to the others. Unsaved edits survive tab switches and lost connections.
- **Inputs**: video and audio state, source, format, live PPM levels, alarms, and the receiver ids to route to (IS-05 only).
- **Alarms**: active alarms with severity and since when.
- **Settings**: every setting with its origin (ENV, FILE, DEFAULT); environment values are read-only, saved values go into the configuration file and apply at the next start. Export as JSON or `KEY=value` (download or copy), import an exported document.

A head starts on the layout last chosen for it (Activate, or `PUT /api/v1/outputs/{h}`), else on `MV_OUT<h>_LAYOUT`, else on the active layout of `layouts.json`; `MV_ACTIVE_LAYOUT` only applies until that file exists. Built-in presets that a 1.1.x release saved unedited get the current preset defaults at the first start (the old file stays as `layouts.json.bak`).

The overlay draws audio bars on input tiles (built-in presets have them on): a PPM scale from 0 to −60 dBFS, a 2 s peak hold, and a clip light. Dim, crossed-out bars mean no audio is routed to that input. The freeze alarm rises when the picture has not changed for `MV_FREEZE_MS` (default 2 s); a source that repeats frames (25p in 50p, a browser source) is not frozen. After `MV_HOLD_MS` without a frame the tile shows `NO SIGNAL` (or `WAITING` while the flow is missing); an input without a video route shows `NOT ROUTED`.

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

`deploy/mxl-multiviewer.yaml` is the pod-network Deployment: MXL root hostPath `/Volumes/mxl`, writable `/config`, `NMOS_HOST_ADDRESS` from `status.podIP`, probes on `/livez` and `/readyz`, and `terminationGracePeriodSeconds` above `SHUTDOWN_TIMEOUT_S`. Set `MXL_CLEANUP_ON_EXIT=true` so production-down sees the output domain disappear. Replace the example emptyDir with a persistent volume when `/config` must survive a reschedule. The GPU file adds `runtimeClassName: nvidia` and one GPU; the process still starts on CPU when the device is missing and `MV_BACKEND=auto`.

## Tests

```bash
./build/unit-tests
MXL_LIB_DIR=/opt/mxl/lib tests/integration/mosaic.sh \
  build/mxl-multiviewer build/mxl-mv-writer build/mxl-mv-sample
tests/nmos/amwa.sh   # IS-04-01, IS-05-01, IS-05-02; needs Docker, not default CI
```

Hardware targets are in `docs/performance.md`. They have not been measured on an A4000, an L4, or a Precision 3930-class CPU yet. A lab run on an NVIDIA A16 (2026-10-03) misses them; the numbers and the profile are in that file. Without a GPU, 16 inputs → 1080p50 keep real time on about 6 cores of a 2× Xeon Gold 6136 (1.1.2, same file).

## Deploy

`docker/docker-compose.demo.yaml` is a registry stand-in, a pattern writer, and the multiviewer on one machine. `docker/docker-compose.host.yaml` is that machine with a host MXL root. `docker/docker-compose.gpu.yaml` adds one NVIDIA GPU on top of the host file. The platform shape is `deploy/mxl-multiviewer.yaml` (pod network). `deploy/mxl-multiviewer-gpu.yaml` adds `runtimeClassName: nvidia` and a `nvidia.com/gpu` limit.

## License

Apache-2.0. Vendored `third_party/doctest`, `picojson`, `stb`, and `font8x8_basic.h` keep their own notices. DejaVu Sans in `third_party/dejavu/` is under its own license. Blend2D 0.21.2 and the asmjit it ships are fetched at configure time and linked statically.
