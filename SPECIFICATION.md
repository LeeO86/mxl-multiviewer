# mxl-multiviewer — Specification

Status: v1.0 (implementation follows this document)
Repository: `LeeO86/mxl-multiviewer`
Sibling projects this spec aligns with: `LeeO86/mxl-decklink`, `LeeO86/mxl-fabrics-agent`, `LeeO86/mxl-webrtc-monitor`, and the platform meta repo `mxl-poc-platform`

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.

`docs/audit.md` records why this is a rewrite of the Rust/GStreamer 2×2 prototype. This document is the target behaviour.

---

## 1. Purpose and scope

`mxl-multiviewer` is a media function that composites up to 32 MXL inputs into one or more uncompressed MXL outputs. Each input is a real NMOS receiver, so `mxl-fabrics-agent` replicates a remote flow when the receiver subscribes to it. The output is an NMOS sender. A controller (the Qvest NMOS crosspoint) routes senders to the multiviewer with ordinary IS-05. The multiviewer output can be routed to `mxl-decklink` for an SDI monitor wall or to `mxl-webrtc-monitor` for a browser.

The multiviewer does no encoding and no WebRTC. The admin UI gets a low-rate JPEG preview only.

Design principles:

1. **Standards on the control surface.** IS-04 v1.3, IS-05 v1.2, BCP-007-03 (`transport: urn:x-nmos:transport:mxl`), BCP-004-01 receiver capabilities. Routing is IS-05 only. The UI has no source picker.
2. **House time, not a pipeline clock.** For every output grain index derived from TAI, the composer reads the latest grain of each input that is not newer than `output_time − input_offset`, composites, and writes one output grain.
3. **Readers never block the composer.** Each input has its own reader thread. A missing or late input shows its last frame for `MV_HOLD_MS`, then a "no signal" slate.
4. **Layouts do not change routing.** A layout maps tiles to input numbers or to special content (clock, label, empty).
5. **Conventions of the sibling repos:** env > JSON file > defaults, admin UI, `/metrics`, exit codes, CI, Compose and Kubernetes.

Out of scope: compressed outputs, recording, IS-07, IS-08, IS-12, authentication, ST 2110 I/O, GStreamer in the media path.

---

## 2. Architecture

```
 NMOS controller (crosspoint)
        │ IS-05 PATCH (mxl_domain_id, mxl_flow_id) on each receiver
┌───────▼──────────────────────────────────────────────────────────┐
│ mxl-multiviewer (C++)                                             │
│  nmos-cpp Node                                                    │
│    MV In <n> Video/Audio receivers (group hint)                   │
│    MV Out <h> Video/Audio senders                                  │
│  per input: reader thread                                         │
│    domain scan → mxlFlowReader → latest grain + audio window      │
│  per output head: composer thread (TAI paced)                     │
│    CPU or CUDA: v210 unpack → scale → composite → overlay blend   │
│                 → v210 pack → mxlFlowWriter                       │
│    audio-follow of one selected input → audio/float32 writer      │
│  overlay: RGBA bitmap, redrawn on change or at MV_OVERLAY_HZ     │
│  TSL 5.0 (UDP and TCP DLE/STX), optional TSL 3.1                  │
│  web: Vue 3 UI, REST, WebSocket, JPEG preview, /metrics           │
└───────────────────────────────────────────────────────────────────┘
        │ one video/v210 flow (+ optional audio/float32)
        ▼
 mxl-decklink  or  mxl-webrtc-monitor
```

One process. One NMOS node. Up to `MV_OUTPUTS` heads (default 1, maximum 3), each with its own raster, layout, and optional audio-follow flow. Heads share the input receivers.

---

## 3. Technology and build

- C++20, CMake ≥ 3.24, Ninja. GCC ≥ 12 or Clang ≥ 16.
- MXL: `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, one pin variable in the Dockerfile and CI, built with `-DMXL_ENABLE_FABRICS_OFI=OFF`. Public C API only (`mxl/mxl.h`, `mxl/flow.h`, `mxl/time.h`).
- nmos-cpp: `fe303849527394b03bdedc8f161f377fe458bb62` (same commit as the siblings).
- Overlay: Blend2D 0.21.2, statically linked, with DejaVu Sans 2.37 compiled into the binary. `MV_WITH_BLEND2D=OFF` keeps the 8×8 bitmap renderer. See `IMPLEMENTATION_PLAN.md`.
- CUDA backend: compiled when the CUDA toolkit is present. Each input thread uploads its grains on its own stream straight from page-locked MXL memory; frames stay packed v210 on the device and the scale kernel decodes the samples it needs. Kernels scale, blend the overlay and pack v210; the result goes by DMA into the MXL output grain.
- CPU backend: the same pipeline, planar 10-bit in 16-bit, tile thread pool, SSE2 clear/blend on x86_64. Sized for about 4–9 tiles at 1080p50.
- JPEG preview and background images: stb (public domain), bundled. No runtime download.
- Web UI: Vue 3 built to one HTML file and embedded. No CDN.
- Base image: Ubuntu 24.04 with `tzdata`. The image MUST start without a GPU (`MV_BACKEND=auto` selects CPU).
- Tests: doctest (vendored). Integration tests are shell scripts.
- Where the pinned MXL or nmos-cpp API differs from this text, follow the API and record the deviation.

Repository layout mirrors the siblings: `.github/workflows`, `cmake`, `deploy`, `docker`, `src`, `tests`, `third_party`, `web`, `assets`, `AGENTS.md`, `IMPLEMENTATION_PLAN.md`, `README.md`, `SPECIFICATION.md`, `docs/audit.md`, Apache-2.0 `LICENSE`.

---

## 4. NMOS

### 4.1 Node and resources

- One nmos-cpp Node, one Device. The node label is `NMOS_LABEL`, or `HOST_ID` when that is empty. The device label is `NMOS_LABEL` plus ` multiviewer`, or `MXL Multiviewer` when `NMOS_LABEL` is empty. `NMOS_TAGS` (a JSON object of string arrays) is copied onto the node and the device. Group hints stay.
- `MV_MAX_INPUTS` (default 16, max 32). Input `n` (1-based) has:
  - one video receiver, `urn:x-nmos:transport:mxl`, format video;
  - one audio receiver, `urn:x-nmos:transport:mxl`, format audio;
  - group hint `urn:x-nmos:tag:grouphint/v1.0` values `MV In <n>:Video` and `MV In <n>:Audio`;
  - labels `MV In <n> Video` and `MV In <n> Audio`.
- Each head `h` (1-based) has a video Source, Flow, and Sender, and, when audio output is enabled, an audio Source, Flow, and Sender. Group hint `MV Out <h>:Video` / `MV Out <h>:Audio`. Labels `MV Out <h> Video` / `MV Out <h> Audio`.
- Receiver capabilities (BCP-004-01):
  - video `video/v210` and `video/v210a`, progressive and interlaced, frame width 1–3840, height 1–2160, grain rates 24000/1001, 24/1, 25/1, 30000/1001, 30/1, 50/1, 60000/1001, 60/1. Colour sampling YCbCr-4:2:2, component depth 10.
  - audio `audio/float32`, sample rate 48000/1, channel count 1–64, sample depth 32.
- `video/v210a`: the key plane is straight alpha for that tile. It is not ignored.
- Registration is unicast (`NMOS_REGISTRY_ADDRESS` / `NMOS_REGISTRY_PORT`). The Query API is `NMOS_QUERY_ADDRESS` (default: the registry address) and `NMOS_QUERY_PORT` (default: registration port + 1). `NMOS_DNS_SD` defaults to false. False sets nmos-cpp `pri` and `highest_pri` to the maximum integer, which skips both mDNS advertisement and DNS-SD browse. The image still links nmos-cpp's DNS-SD client library; it does not run avahi-daemon and does not need a D-Bus socket while DNS-SD is off.
- Stable ids are UUIDv5 (RFC 4122 URL namespace `6ba7b811-9dad-11d1-80b4-00c04fd430c8`) from `NMOS_SEED`:

| Resource | Name |
| --- | --- |
| Node | `mxl-multiviewer/<seed>/node` |
| Device | `mxl-multiviewer/<seed>/device` |
| Output domain id (when `MV_OUTPUT_DOMAIN_ID` is empty) | `mxl-multiviewer/<seed>/domain` |
| Video receiver n | `mxl-multiviewer/<seed>/in/<n>/video` |
| Audio receiver n | `mxl-multiviewer/<seed>/in/<n>/audio` |
| Video source / sender head h | `.../out/<h>/video/source` and `.../sender` |
| Video flow head h | `.../out/<h>/video/flow/<format-token>` |
| Audio source / sender / flow | `.../out/<h>/audio/...` with channel count in the flow token |

A raster or rate change changes the flow token, which mints a new flow id. The sender's flow id and active `mxl_flow_id` are updated. The crosspoint follows the sender.

### 4.2 IS-05 behaviour

- BCP-007-03 `transport_params[0]` carries `mxl_domain_id` and `mxl_flow_id`. No transport file.
- An activation is accepted when the ids are UUIDs even if the domain or flow is not on disk yet. Non-UUID values are rejected with the IS-05 error response.
- `master_enable: false` stops that leg. State `not_routed`. The other leg of the same input is independent. Video and audio MAY come from different senders.
- Each activation is written to `<MV_STATE_DIR>/routes.json`. The next start restores those routes into the readers before the first frame. The restored route is also written into the receiver's IS-05 active and staged documents (`master_enable`, `sender_id`, `mxl_domain_id`, `mxl_flow_id`) and its IS-04 subscription, so a controller sees the connection the reader follows.
- On every activation the IS-04 receiver `subscription` (`sender_id`, `active`) is updated.
- Senders' active transport params carry this process's output domain id and the current flow id. `master_enable` is true while the head is writing.
- Output receivers are not exposed. Inputs are not senders.

### 4.3 Sender label lookup

UMD source `is04` reads the routed sender's `label` from the registry Query API at `NMOS_QUERY_ADDRESS`:`NMOS_QUERY_PORT`. A route without a sender id is looked up by its flow id (`senders?flow_id=`). A failed lookup leaves the previous label; a re-route clears it. Without a registry label the caption falls back to the tile's manual text, then the routed MXL flow's `label`, then `MV In <n>`.

---

## 5. Domains and MXL I/O

### 5.1 Scan

- `MXL_DOMAIN_SCAN_PATH` (default `/Volumes/mxl`) is the MXL root. Direct subdirectories that contain `domain_def.json` are domains. The identity is the `id` field, never the directory name. Unknown JSON fields are ignored.
- A domain whose `domain_def.json` contains an `x-mxl-fabrics-agent` object with `mirror: true` is a mirror domain. Mirror domains are valid sources. They are not valid output domains.
- The scan runs on every resolve attempt. There is no negative cache.

### 5.2 Output domain

- `MV_OUTPUT_DOMAIN_DIR` is created if missing. `MXL_OUTPUT_DOMAIN_DIR` and `MXL_OUTPUT_DOMAIN_ID` are aliases of `MV_OUTPUT_DOMAIN_DIR` and `MV_OUTPUT_DOMAIN_ID`. Environment beats the alias, then the file, then the alias in the file. `domain_def.json` is written with `MV_OUTPUT_DOMAIN_ID` or the UUIDv5 domain id from §4.1 only when the file is missing. If the file already exists with a different id, the process logs `domain_id_mismatch` and keeps the id from the file. It does not overwrite `domain_def.json` or `options.json` on a later start. `options.json` sets `urn:x-mxl:option:history_duration/v1.0` from `MV_HISTORY_DURATION_NS` (default 200 ms) only when this process creates that file. `MXL_CLEANUP_ON_EXIT=true` removes this output directory on shutdown. A mirror directory, and a directory that is the scan root itself, are left in place.
- If the directory is a mirror domain, startup fails with exit 78.
- The process never writes flows into a mirror domain.

### 5.3 Reader lifecycle

- On activation: resolve domain → `mxlCreateInstance` on that directory → `mxlCreateFlowReader`. Each missing step retries with backoff 250 ms → 5 s while `master_enable` is true. State `waiting`, reason `domain_not_found` or `flow_not_found`.
- The reader thread takes the newest complete grain (and a matching audio window) into a slot the composer can take without blocking. When caught up it waits for the next grain (`mxlFlowReaderGetGrain` with a timeout). On the CUDA backend the grain is uploaded to a device frame before the reader continues; on the CPU backend (or when that upload fails) it is unpacked into host memory.
- If `mxlFlowReaderGetGrain` returns too-late, the reader jumps to the current head and increments `resyncs`.
- A writer that restarts re-creates its flow; MXL then reports `MXL_ERR_FLOW_INVALID` on a read past the head. The reader releases the flow and opens it again (`waiting` while it is missing); the last frame holds meanwhile. The audio leg, which only reads while its head moves, probes past the head once a second while it is `no_signal`.
- A format change (`flow_def.json` width, height, rate, media type, or channel count) rebuilds that input only. Other inputs and the output keep running.
- State `running` when a grain newer than the hold deadline is in hand. State `holding` when the last good grain is still inside `MV_HOLD_MS`. State `no_signal` after that, or when the flow exists but no grain has arrived.

### 5.4 Composer

For each output head, for each output index `N` at the head's grain rate:

1. `output_time = indexToTimestamp(rate, N)` using the MXL 128-bit rounding (see the implementation plan).
2. Sleep until `output_time` unless the clock is already past it.
3. If the clock has moved more than one index ahead, count the skipped indexes as missed, count the frame late, and continue at the current index.
4. Take the layout pointer once (frame boundary).
5. For each input tile, select the newest copied grain whose origin timestamp is `≤ output_time − MV_INPUT_OFFSET_GRAINS × output_frame_duration`. A grain newer than that is not used. Frame-rate conversion (for example 60000/1001 into 50) is this comparison.
6. Compose, blend the overlay, pack v210, `mxlFlowWriterOpenGrain` / `CommitGrain` with `validSlices = totalSlices`.
7. If audio-follow is on, write the selected input's float32 samples covering that output frame (2 or 16 channels). Missing channels are silence. Extra channels are dropped from the front of the tile's channel window.
8. A frame whose commit time is later than the next index's timestamp is late.

`MV_INPUT_OFFSET_GRAINS` defaults to 2. Added latency from input grain origin to the output grain that first displays it MUST be ≤ 2 output frames plus this offset, when the input grain is available at the reader.

Interlaced inputs are bobbed: the composer scales field 0 (even lines) to the tile. The output flow is progressive.

### 5.5 Pixel path

- Unpack v210 to planar 10-bit samples stored in `uint16_t` (Y full width, Cb/Cr half width). Row stride of v210 is `ceil(width/48)×128`, matching MXL and DeckLink.
- v210a key plane follows the MXL layout: 3×10-bit samples per little-endian 32-bit word, 4-byte line alignment, immediately after the fill plane. Alpha is straight. 0 is transparent, 1023 is opaque.
- Scale is bilinear. `fit` letterboxes or pillarboxes (limited-range black Y=64, Cb=Cr=512). `fill` crops the source equally on the overflowing axis.
- Composite in ascending z-order. Overlapping tiles are allowed; the higher z wins.
- Pack back to v210. A pack/unpack of active pixels is bit-exact.
- Overlay is an RGBA layer the size of the canvas, blended every output frame. A thread per head redraws it at `MV_OVERLAY_HZ` (default 25); compose uses the latest finished drawing. Meter ballistics are part of that redraw. On the CUDA backend the overlay thread uploads only the areas that changed.

### 5.6 Backends

`MV_BACKEND=auto|cuda|cpu`.

- `auto`: CUDA when the binary contains the CUDA backend and a device is present, otherwise CPU. The container image contains the backend. A device is present when the NVIDIA container toolkit or a Kubernetes `nvidia` runtime has injected the host driver (`libcuda`). The image starts without that driver and stays on CPU.
- `cuda`: required. If the binary has no CUDA backend, exit 78. If no device is visible, exit 75.
- `cpu`: CPU backend. 2160p output is legal but not the sizing target.

On the CUDA backend, when compose fails for 25 frames in a row, the process switches to the CPU path for the rest of its life (`cuda_disabled` logged once).

GPU memory is reported from `cudaMemGetInfo` when CUDA is active, otherwise 0.

### 5.7 Audio metering

Per input channel, peak programme meter:

- IEC 60268-10 type IIa attack: a step reaches `1 − exp(−t/τ)` with `τ = 10 ms`.
- Decay: 24 dB in 2.8 s (linear-amplitude exponential), the type IIa return time.
- Scale is dBFS. 0 dBFS is full scale. The overlay draws PPM marks at 0, −6, −12, −18, −24, −36, −48, −60 beside the bars, with dBFS labels when the bar is tall enough. Colour zones default to green below −18 dBFS, amber below −9, red at and above −9, by position on the bar. Zones are configurable per tile (`zone_green`, `zone_amber`).
- Peak hold default 2 s, then the same decay. The overlay draws it as a line on the bar.
- Optional RMS (250 ms) is computed and exported on the WebSocket. It is not on the bar unless the tile asks for it.
- EBU R 128 momentary loudness is not in this version.

Bars: 1–16 channels within the first 16 channels of the input (the metered ones: first channel + count ≤ 16), first channel selectable, position left, right, or overlay, clip indicator above each bar when `|sample| ≥ MV_CLIP_LINEAR` (default 0.999). Bars are drawn on input tiles only. When the input's audio leg is not routed the bars are dim and crossed out; routed audio without samples shows empty bars.

The audio leg is read from its own flow and domain, independent of the video leg (§4.2). Every new sample is metered from the flow's head index. A head that does not move for `MV_HOLD_MS` is `no_signal`.

---

## 6. Layouts and tiles

### 6.1 Document

A layout is JSON, `version: 1`:

```json
{
  "version": 1,
  "name": "2x2",
  "background": "#101010",
  "tiles": [
    {
      "id": "t1",
      "content": "input",
      "input": 1,
      "rect": {"x": 0, "y": 0, "w": 0.5, "h": 0.5},
      "z": 0,
      "scale": "fit"
    }
  ]
}
```

`rect` is normalised, origin top-left, `x,y,w,h` in `[0,1]`, `x+w` and `y+h` ≤ 1 within 1e-6. `content` is `input`, `clock`, `label`, or `empty`. `input` is 1-based and ≤ `MV_MAX_INPUTS`. Tile ids are unique inside the layout. Names are unique across the book.

The book is stored at `MV_LAYOUTS_FILE` when that variable is set (atomic write). At start, values an older release accepted (audio zones out of order or range, bars past channel 16, an unknown clock style or zone, a `tally_text` that is not a boolean) are corrected and logged (`layout_repaired`); a file that still cannot be read is renamed to `<file>.bad` and logged (`layouts_file_invalid`), and the presets run. The API rejects those values. Import and export are the same document with a `layouts` array and an `active` name. An imported book needs at least one layout; an `active` name it does not contain becomes its first layout, and a head whose layout is not in the book switches to the book's active layout. Built-in presets are recreated if missing: `1`, `2x2`, `3x3`, `4x4`, `2+8`, `1+5`, `1+7`, `2+6`, `5x5`.

The book also keeps the layout last chosen for each head (`"heads": {"1": "3x3"}`), the start layout of each head (`"start_layouts": {"1": "2x2"}`, "use as start layout" in the editor), and the revision of the preset defaults it follows (`preset_revision`, 2 since 1.2). A head starts on the first of these that names a layout of the book:

1. `MV_OUT<h>_LAYOUT`, else `MV_ACTIVE_LAYOUT`, when it is set in the environment (not in the config file);
2. the head's start layout;
3. the head's saved layout (the one it showed last);
4. `MV_OUT<h>_LAYOUT` from the config file;
5. the book's `active` layout (`MV_ACTIVE_LAYOUT` is the active layout of a book that has no file yet).

`POST /api/v1/layouts/{name}/activate` makes a layout active and the saved layout of every head; `PUT /api/v1/outputs/{h}` saves it for that head (`layout`) and sets or, with `null`, clears its start layout (`start_layout`). Neither moves a start layout. Deleting a layout removes it from `heads` and `start_layouts`.

A book below `preset_revision` 2 (written by 1.1.x, or by 1.2.0 from such a file) is migrated at start: every layout whose fields all equal the 1.1.x built-in preset of the same name (for any `MV_MAX_INPUTS`) becomes today's preset, with audio bars; edited layouts stay. Before the file is rewritten it is copied once to `<file>.bak` (an existing `.bak` is never replaced), and `layouts_migrated` names the layouts. The book is then at revision 2, so a later start changes nothing. An imported book is migrated the same way, without a `.bak`.

Preset geometry:

| Name | Tiles |
| --- | --- |
| `1` | one full-frame input 1 |
| `2x2`, `3x3`, `4x4`, `5x5` | equal grid, inputs in reading order |
| `2+8` | two stacked tiles on the left half (inputs 1–2), eight tiles in a 2×4 grid on the right (inputs 3–10) |
| `1+5` | input 1 on the left two-thirds, inputs 2–6 stacked in the right third |
| `1+7` | input 1 in the top-left 3/4 by 3/4, seven tiles along the right column and the bottom row |
| `2+6` | inputs 1–2 side by side on the top half (PVW/PGM), inputs 3–8 in a row along the bottom half |

Activating a layout swaps the pointer the composer reads at the next frame boundary. The output grain stream does not stop and the flow id does not change.

### 6.2 Per-tile display

| Option | Values | Default on input tiles |
| --- | --- | --- |
| `umd` | on/off | on |
| `umd_source` | `is04`, `manual`, `tsl` | `is04` |
| `umd_text` | string | empty |
| `umd_position` | `top-inside`, `top-outside`, `bottom-inside`, `bottom-outside` | `bottom-inside` |
| `umd_align` | `left`, `centre`, `right`: the text in the bar (between the lamps). Text wider than the bar is cut and ends with `…` | `left` |
| `umd_font` | px at a 1080-tall canvas, scaled with the canvas | 28 |
| `umd_bg` | `#RRGGBB` or `#RRGGBBAA` | `#000000c0` |
| `tally_border`, `tally_lamp` | bool | true |
| `tally_text` | `true`, `false`, or `null` (follow the layout's `tally_text`) | `null`; the layout's `tally_text` is false |
| `audio_bars` | bool | true in the built-in presets; false when the key is missing |
| `audio_bar_rms` | bool, draw the 250 ms RMS tick on each bar | false |
| `audio_bar_channels` | 1–16 | 2 |
| `audio_bar_first` | 0-based channel; `audio_bar_first` + `audio_bar_channels` ≤ 16 | 0 |
| `audio_bar_position` | `left`, `right`, `overlay` | `right` |
| `zone_green`, `zone_amber` | dBFS where amber and where red start; −60 ≤ `zone_green` ≤ `zone_amber` ≤ 0 | −18 / −9 |
| `format_label` | bool | true |
| `latency` | bool, grain origin versus now | false |
| `safe_area` | 90% and 80% rectangles | false |
| `centre` | centre cross | false |
| `aspect_markers` | any of `16:9`, `4:3`, `1:1`, `9:16` | none |
| `scale` | `fit`, `fill` | `fit` |

Clock tiles: `clock_style` `analogue` (also accepted as `analog`) or `digital`, `clock_zone` `tai`, `utc`, or `local` (the zone of `MV_TIMEZONE`, else of `TZ`, else UTC), optional `timecode_rate` (`25`, `50`, `30000/1001`, …) drawn as `HH:MM:SS:FF` from the TAI index at that rate. Other values are rejected. The clock and its timecode are sized to the tile.

Label tiles: `label_text`, centred and sized to the tile.

Background: `background` colour where no tile covers the canvas. Optional JPEG or PNG at `MV_BACKGROUND_FILE`, decoded and scaled to cover the canvas under the tiles. Letterbox and pillarbox areas of `fit` tiles stay limited-range black.

Tally colours: red, green, amber, off (TSL, §7). Border width is 8 px at 1080 and scales. The border shows the text tally, else RH, else LH. Lamps sit at the two ends of the UMD: the left lamp shows LH, the right lamp RH; an off lamp is not drawn. The UMD text is drawn in the border's colour, white when all three are off.

`tally_text` colours the UMD background with the text tally while that is not off, and the text is then black; otherwise the background is `umd_bg`. The layout carries `tally_text` too (`"tally_text": true` next to `background`, default false): it applies to every tile that leaves its own `tally_text` null or out. The heads share one TSL input, so a head gets text tally backgrounds by showing a layout that has them; heads that show the same layout look the same. Other values are rejected by the API and repaired in a layout file (§6.1).

### 6.3 Alarms

Evaluated per input with debounce `MV_ALARM_DEBOUNCE_MS` (default 500) and clear `MV_ALARM_CLEAR_MS` (default 500); `freeze` uses `MV_FREEZE_MS` instead of the debounce:

| Alarm | Condition |
| --- | --- |
| `no_signal` | state `no_signal` or `waiting` while enabled |
| `black` | mean Y of the tile's source ≤ `MV_BLACK_Y` (default 32, 10-bit) |
| `freeze` | the 64-bit picture hash has not changed for `MV_FREEZE_MS` (default 2000, at least 1000), counted from its last change. The hash covers the luma of every second line, summed per block of a 32×18 grid with a weight for the place of each 6-pixel group in its block. A source that repeats grains (25p in 50p) is not frozen |
| `silence` | peak of the metered channels < `MV_SILENCE_DBFS` (default −60) |
| `clip` | clip latch on a metered channel |
| `format_mismatch` | routed video is outside the receiver caps (rate or raster the node did not advertise, or not v210/v210a) |

An alarm shows a badge and a coloured border (red for no-signal, black, freeze; amber for silence and format; red for clip) distinct from tally: the alarm border sits inside the tally border. A tile that shows a slate (§6.4) has no badge. Active alarms increment `mxl_multiviewer_alarms_total`. `silence` also rises when routed audio does not arrive.

### 6.4 Slate

After the hold time the tile is limited-range black with the text `NO SIGNAL` and the input label (`MV In <n>`). `waiting` uses `WAITING`. A video leg that is not routed shows `NOT ROUTED` at once.

---

## 7. TSL

- `TSL_ENABLE=true` listens on `TSL_UDP_PORT` (default 8910) and `TSL_TCP_PORT` (default 8911).
- TSL UMD 5.0. UDP packets are the little-endian body (`PBC`, `VER`, `FLAGS`, `SCREEN`, then display messages). A DLE/STX … DLE/ETX wrapper (DLE = 0xFE, STX = 0x02, ETX = 0x03, stuffed DLE DLE) is accepted on UDP and required on TCP. `PBC` is the number of bytes after the PBC field. `VER` 0. `FLAGS` bit 0 selects UTF-16LE, otherwise ASCII; UTF-16 text (surrogate pairs included) is converted to UTF-8, a lone surrogate becomes U+FFFD. Bit 1 (screen control) is ignored. Display message: `INDEX`, `CONTROL`, `LENGTH`, `TEXT`. Tally in `CONTROL` bits 0–1 (RH), 2–3 (text), 4–5 (LH): 0 off, 1 red, 2 green, 3 amber. Brightness bits 6–7 are stored and not required for the drawing. Bit 15 (control data) skips that display.
- Each input keeps the LH, RH, and text tally and the text of its last display message. The drawing (§6.2): left lamp LH, right lamp RH, border the text tally if it is not off, otherwise RH if not off, otherwise LH; the text tally colours the UMD background of tiles with `tally_text`. One display index applies to every tile on every head that shows that input.
- Display index maps to inputs through `TSL_MAP` (`0:1,1:2` means display 0 → input 1). Empty map means display `i` → input `i+1`. `TSL_SCREEN` default −1 accepts every screen; otherwise only that screen index is applied.
- `TSL_V31=true` also parses 18-byte TSL 3.1 datagrams on the UDP port: address in byte 0 bits 0–6, byte 1 bit 0 red, bit 1 green, bit 2 amber (both red and green without amber is shown as amber), bytes 2–17 ASCII text. That one tally sets LH, RH, and text tally alike. The same index map applies.
- On the platform the tally calculator sends TSL 5.0 over UDP to the multiviewer's Service (8910/udp): `SCREEN` 0, every change plus a refresh each second, UTF-16 labels, `INDEX` n−1 for input n (empty `TSL_MAP`).

---

## 8. Web UI and API

Vue 3, embedded, no CDN. Unauthenticated, same posture as the siblings: protected networks only. `WEB_ENABLE=false` returns 404 for `/`, `/index.html`, `/preview.jpg`, and every POST, PUT, PATCH, and DELETE. GET APIs, `/livez`, `/readyz`, and `/metrics` stay. The process has no secrets. Export always sets `"secrets": false` and includes every setting.

### 8.1 Pages

- **Preview.** JPEG of one head at `MV_PREVIEW_FPS` (default 5) and `MV_PREVIEW_WIDTH` (default 480), a head selector when `MV_OUTPUTS` > 1, the head's layout with Activate, and its counters from the WebSocket.
- **Layout.** Canvas editor. Drag and resize with snapping to a grid of `MV_GRID` (default 24), arrow keys move, Shift + arrows resize. Add input, clock, label, and empty tiles, duplicate, delete, and change the drawing order. Assign content and the options in §6.2. Save, save as a named layout, delete (not the built-in presets or the active layout), activate. Import and export one layout or the book. Unsaved edits stay in the page until they are saved or discarded.
- **Inputs.** For each input: video and audio state, source label, flow ids, format, audio channel count, live PPM bars, alarms, and the receiver ids. No source picker.
- **Alarms.** Active alarms with severity and the time they became active.
- **Settings.** Effective config with provenance. Env-set keys are read-only. Import and export of the configuration document. `KEY=value` export.

### 8.2 REST

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/v1/info` | version, MXL pin, backend, max inputs, heads |
| GET | `/api/v1/inputs` | per-input state |
| GET | `/api/v1/outputs` | per-head frames, late, missed, flow ids, layout |
| GET | `/api/v1/layouts` | the book |
| PUT | `/api/v1/layouts/{name}` | create or replace one layout |
| DELETE | `/api/v1/layouts/{name}` | delete a layout that is not active and not on an output head |
| POST | `/api/v1/layouts/{name}/activate` | arm the layout for the next frame of every head; it becomes the book's active layout and the saved layout of every head |
| GET | `/api/v1/presets` | today's built-in presets, as a book |
| PUT | `/api/v1/outputs/{h}` | `{"layout": "name", "audio_follow": n, "format": "1920x1080p50", "start_layout": "name"}` (`start_layout` may be null); an unknown layout is 404, `audio_follow` outside 0–`MV_MAX_INPUTS` is 400, a `start_layout` that is not a name or null is 400; the layout is saved for the next start, `start_layout` is the head's start layout (§6.1) |
| GET | `/api/v1/alarms` | active alarms: input, name, `severity` (`red`, `amber`), `since` (Unix ms) |
| GET | `/api/v1/events` | WebSocket: inputs, meters (at overlay rate), alarms, outputs |
| GET | `/preview.jpg` | latest JPEG of head 1; `?head=<h>` for another head |
| GET/PUT | `/api/v1/config` | flat key update; `restart_required` when a global key changes |
| GET | `/api/v1/config/export` | one JSON document: `version`, `secrets`, `settings`, `layouts`, `routes` |
| POST | `/api/v1/config/import` | restore that document. Settings and layouts apply immediately. Routes are written to `routes.json` and apply on the next start (`routes_restart`). Settings set by the environment are skipped and listed in `skipped`; heads moved to the active layout are listed in `heads_moved` |
| GET | `/api/v1/config/env` | `KEY=value` text |

`PUT /api/v1/config` body is `{ "KEY": "value" | null }`. Null removes the file layer. The merge is validated before the file is replaced.

Each head in `GET /api/v1/outputs` (and the WebSocket) carries `start_layout` (its start layout or null) and `start_layout_env` (the layout the environment starts it on, or null). `GET /api/v1/info` also carries `label` (node label), `grid` (`MV_GRID`), `preview_fps`, `hold_ms`, `timezone` (the IANA zone of `local` clocks: `MV_TIMEZONE`, else `TZ`, else the zone `/etc/localtime` names, else `UTC`; empty when unknown), and `utc_offset_s` (that zone's offset now). The web UI draws `local` clocks in that zone, not in the browser's. Each input in `GET /api/v1/inputs` carries `ppm_dbfs`, `hold_dbfs`, `rms_dbfs`, and `clip` per channel (16 each), and its TSL state (§7): `tsl_lh`, `tsl_rh`, `tsl_text_tally` (0 off, 1 red, 2 green, 3 amber), `tally` (the border colour: text, else RH, else LH), and `tsl_text` (the label). `/statusz` and the WebSocket carry the same input objects. Layout names in paths are percent-decoded.

### 8.3 Ops

On `WEB_PORT` (default 8110):

- `/livez` — 200 while the heartbeat is younger than 5 s.
- `/readyz` — 200 when the composer heartbeat is fresh and, if `NMOS_ENABLE=true` and a registry address is set, the Query API currently returns the node. Otherwise 503 with a JSON reason. Inputs in `waiting` do not by themselves fail readiness: the wall is producing slates and the output flow exists.
- `/statusz` — 200, JSON snapshot.
- `/metrics` — Prometheus text, prefix `mxl_multiviewer_`.

---

## 9. Configuration

Precedence: environment > `MV_CONFIG_FILE` JSON (flat object, keys are the variable names, values are strings) > built-in default. Invalid configuration exits 78. The file is written atomically (temporary file and rename).

| Key | Default | Restart | Meaning |
| --- | --- | --- | --- |
| `HOST_ID` | hostname | yes | node label when `NMOS_LABEL` is empty, and the default seed material. Not an announced address |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | yes | MXL root. Parent of domain directories, mirrors included |
| `MV_OUTPUT_DOMAIN_DIR` | `/Volumes/mxl/multiviewer` | yes | output domain directory. Alias: `MXL_OUTPUT_DOMAIN_DIR` |
| `MV_OUTPUT_DOMAIN_ID` | empty (UUIDv5) | yes | `domain_def.json` id when the file is created. Alias: `MXL_OUTPUT_DOMAIN_ID` |
| `MV_STATE_DIR` | `/config` | yes | only directory this process writes for its own state: `config.json`, `layouts.json`, `routes.json` |
| `MXL_CLEANUP_ON_EXIT` | false | yes | remove the output domain directory after SIGTERM |
| `MV_BACKEND` | `auto` | yes | `auto`, `cuda`, `cpu` |
| `MV_MAX_INPUTS` | 16 | yes | 1–32 |
| `MV_OUTPUTS` | 1 | yes | 1–3 |
| `MV_OUTPUT_FORMAT` | `1920x1080p50` | no | head 1 raster and rate; progressive |
| `MV_INPUT_OFFSET_GRAINS` | 2 | no | 0–30 |
| `MV_HOLD_MS` | 1000 | no | slate delay |
| `MV_HISTORY_DURATION_NS` | 200000000 | yes | new domain only |
| `MV_LAYOUTS_FILE` | empty | no | layout book path. Empty uses `<MV_STATE_DIR>/layouts.json` |
| `MV_ACTIVE_LAYOUT` | `2x2` | no | active layout of a layout book that has no file yet; heads start on the book's saved choices (§6.1). Set in the environment, it is the start layout of every head that has no `MV_OUT<h>_LAYOUT` in the environment |
| `MV_AUDIO_CHANNELS` | 2 | no | `0`, `2`, or `16`; 0 disables audio flows |
| `MV_AUDIO_FOLLOW` | 1 | no | input number whose audio is copied; 0 disables |
| `MV_OVERLAY_HZ` | 25 | no | cap |
| `MV_PREVIEW_FPS` | 5 | no | JPEG rate |
| `MV_PREVIEW_WIDTH` | 480 | no | JPEG width |
| `MV_GRID` | 24 | no | editor snap divisor |
| `MV_BLACK_Y` | 32 | no | 10-bit |
| `MV_SILENCE_DBFS` | −60 | no | |
| `MV_CLIP_LINEAR` | 0.999 | no | |
| `MV_ALARM_DEBOUNCE_MS` | 500 | no | |
| `MV_ALARM_CLEAR_MS` | 500 | no | |
| `MV_FREEZE_MS` | 2000 | no | 1000–600000; freeze alarm after the picture has not changed for this long (§6.3) |
| `MV_BACKGROUND_FILE` | empty | no | JPEG or PNG under the tiles |
| `MV_TIMEZONE` | empty | yes | IANA zone (`Europe/Zurich`) of clock tiles with `clock_zone: local`; overrides `TZ`. A name the zone database does not have is invalid |
| `MV_CONFIG_FILE` | empty | yes | flat JSON |
| `NMOS_ENABLE` | true | yes | |
| `NMOS_REGISTRY_ADDRESS` | empty | yes | registration dial address. A DNS name is allowed here; it is not announced |
| `NMOS_REGISTRY_PORT` | 3210 | yes | |
| `NMOS_QUERY_ADDRESS` | empty (registry address) | yes | Query API dial address |
| `NMOS_QUERY_PORT` | empty (registry port + 1) | yes | |
| `NMOS_DNS_SD` | false | yes | false disables browse and mDNS advertisement |
| `NMOS_PORT` | 3262 | yes | Node and Connection APIs. WebSocket on `NMOS_PORT+1` |
| `NMOS_SEED` | `<HOST_ID>-multiviewer` | yes | UUIDv5 material for every id, including the default output domain id |
| `NMOS_LABEL` | empty (`HOST_ID`) | yes | node label and device label prefix |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | yes | the only address announced (node `href`, `api.endpoints[].host`, IS-05 control hrefs). Rejects hostnames, `0.0.0.0`, and `127.0.0.0/8` when NMOS is on |
| `NMOS_TAGS` | `{}` | yes | JSON object of string arrays on the node and device |
| `WEB_ENABLE` | true | yes | false hides the UI, the preview, and mutating routes |
| `WEB_PORT` | 8110 | yes | |
| `TSL_ENABLE` | true | yes | |
| `TSL_UDP_PORT` | 8910 | yes | |
| `TSL_TCP_PORT` | 8911 | yes | |
| `TSL_V31` | false | yes | also accept TSL 3.1 on UDP |
| `TSL_SCREEN` | −1 | no | |
| `TSL_MAP` | empty | no | `display:input` pairs |
| `LOG_LEVEL` | `info` | no | `trace` `debug` `info` `warn` `error` |
| `LOG_FORMAT` | `json` | yes | `json` or `text` |
| `SHUTDOWN_TIMEOUT_S` | 10 | yes | SIGTERM budget. The clean path exits 143 inside this budget; past it the process `_exit`s 143 |

Per head `h` ≥ 2 (head 1 uses the unscoped keys):

| Key | Meaning |
| --- | --- |
| `MV_OUT<h>_FORMAT` | defaults to `MV_OUTPUT_FORMAT` |
| `MV_OUT<h>_LAYOUT` | layout of head `h` when none was saved for it (§6.1); otherwise the book's active layout. Set in the environment, it is the start layout of head `h` |
| `MV_OUT<h>_AUDIO_FOLLOW` | defaults to `MV_AUDIO_FOLLOW` |
| `MV_OUT<h>_AUDIO_CHANNELS` | defaults to `MV_AUDIO_CHANNELS` |

`MV_OUT1_*` is accepted as an alias of the unscoped keys.

Format token: `<width>x<height>p<rate>` with rate `24`, `25`, `30`, `50`, `60`, `2398`, `2997`, `5994`, or `N/D`. `2398` → 24000/1001, `2997` → 30000/1001, `5994` → 60000/1001. Interlaced output tokens are rejected. Width and height even, width ≤ 3840, height ≤ 2160, width multiple of 2. 3840×2160 is supported; the CUDA backend is the one sized for 16×1080p50 into 2160p50.

Ports MUST NOT collide with each other. Defaults are chosen to miss 8080, 3212/3213, 8090, 8095, 3232/3233, 23500–23599, 8100, 3242/3243, 8554, 8888, 8889, 8189, 9997, 9998, 9610, 9620, 3252/3253, and 9100.

Runtime changes of restart-flagged keys are persisted and reported as `restart_required`. They do not apply until the next process start.

---

## 10. Metrics

Prefix `mxl_multiviewer_`.

| Metric | Type | Labels |
| --- | --- | --- |
| `info` | gauge 1 | `version`, `mxl_revision`, `backend` |
| `output_frames_total` | counter | `head` |
| `output_frames_late_total` | counter | `head` |
| `output_frames_missed_total` | counter | `head` |
| `compose_seconds` | histogram | `head`, `backend` |
| `compose_gpu_seconds` | histogram (CUDA only) | `head`, `stage` (`background`, `tiles`, `overlay`, `pack`, `download`) |
| `gpu_memory_bytes` | gauge | |
| `input_state` | gauge 1 for the current state | `input`, `kind` (`video`/`audio`), `state` |
| `input_late_grains_total` | counter | `input` |
| `input_resyncs_total` | counter | `input` |
| `alarms` | gauge 0/1 | `input`, `name` |
| `alarms_total` | counter | `input`, `name` |
| `tsl_messages_total` | counter | `transport` (`udp`/`tcp`) |
| `nmos_registry_up` | gauge 0/1 | |
| `nmos_activations_total` | counter | `input`, `kind` |

Histogram buckets for compose time: 1, 2, 5, 10, 20, 40, 80 ms.

`deploy/grafana/mxl-multiviewer.json` graphs frames, late, missed, compose time, input states, resyncs, and alarms.

---

## 11. Process lifecycle

Startup: validate config (else 78) → create the state directory → create the output domain (else 78 if the path is a mirror or cannot be created) → bind web, NMOS, and TSL (else 75) → restore routes → start readers and composers → register the node. A TCP or UDP port that cannot be bound exits 75. Card-level hardware does not apply.

The addresses this process announces are IP literals taken from `NMOS_HOST_ADDRESS`. The HTTP and NMOS sockets listen on the wildcard address; the announced host is separate. This process does not write SDP, ICE candidates, or SRT addresses.

SIGTERM and SIGINT, within `SHUTDOWN_TIMEOUT_S`: stop HTTP, stop composers and release MXL readers and writers, tombstone the node's IS-04 resources so nmos-cpp sends DELETEs, then if `MXL_CLEANUP_ON_EXIT=true` remove this process's output domain directory, then exit 143. Child work is stopped with the threads. If the budget expires first, the process exits 143 without waiting.

| Code | Meaning |
| --- | --- |
| 0 | `--help` |
| 75 | a port could not be bound, or another startup failure (`EX_TEMPFAIL`) |
| 78 | invalid configuration (`EX_CONFIG`) |
| 143 | SIGTERM or SIGINT, including a shutdown that exceeded `SHUTDOWN_TIMEOUT_S` |

The container runs as uid/gid 1000.

---

## 12. Deployment and CI

- Image `ghcr.io/leeo86/mxl-multiviewer`, public. Tags on `vX.Y.Z`: `X.Y.Z`, `X.Y`, `X`. Those version tags are not moved. Branch `main`: `nightly-dev` and `git-<sha>`. There is no `latest` tag. OCI labels include `org.opencontainers.image.source`, `org.opencontainers.image.revision` (the git commit), `org.opencontainers.image.licenses`, and `io.dmf.mxl.revision` (the MXL pin `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`). The runtime user is uid 1000. Root is not used.
- CI: build MXL and nmos-cpp, build the project, unit tests, CPU integration test, container build. The `ci.yaml` job does not install nvcc, so that binary is CPU-only. The container build compiles the CUDA compositor. A GPU is not required to build or to start the image.
- `docker/docker-compose.demo.yaml`: registry stand-in, pattern writers, the multiviewer, and a note for attaching `mxl-webrtc-monitor` to the output flow. `docker/docker-compose.host.yaml`: host network, MXL root bind, ports 8110 and 3262/3263. `docker/docker-compose.gpu.yaml`: overlay that requests one NVIDIA GPU so the container toolkit injects the driver.
- `deploy/mxl-multiviewer.yaml`: pod network, no `hostNetwork` and no `hostIPC`, uid 1000 with `supplementalGroups: [1000]`, MXL root `hostPath` at `/Volumes/mxl`, a writable `/config` volume, probes on `/livez` and `/readyz`, `terminationGracePeriodSeconds` greater than `SHUTDOWN_TIMEOUT_S`, and the standard environment names. `deploy/mxl-multiviewer-gpu.yaml` is that Deployment plus `runtimeClassName: nvidia` and `nvidia.com/gpu: 1`. The example `/config` volume is an emptyDir; a platform that must keep state across reschedule replaces it with a persistent volume. The image tag in those files is `1.0.0`.
- `tests/nmos/amwa.sh`: runs the AMWA NMOS Testing tool suites IS-04-01, IS-05-01, and IS-05-02 against `NMOS_PORT`. Not part of the default CI job (the harness image is large and the suite is long). It is the supported way to run those tests.

---

## 13. Performance targets

Measured on hardware, not in CI. Results are recorded in `docs/performance.md` when a run exists. Until then that file states that the run has not been taken.

| Case | Target |
| --- | --- |
| CUDA, RTX A4000 or L4, 16×1080p50 → 1080p50 | compose time < 50% of the frame period (10 ms at 50p), zero late frames over 1 hour |
| CUDA, same GPUs, 16×1080p50 → 2160p50 | same |
| CPU, Precision 3930 class, 4×1080p50 → 1080p50 | same |
| Latency | input grain origin → output grain ≤ 2 frames + `MV_INPUT_OFFSET_GRAINS` |

---

## 14. Testing

- Unit: layout validation and presets, tile geometry (fit, fill, even snap), v210 pack/unpack bit-exact including a short row and the v210a key plane, scaler against a bilinear reference (tolerance), PPM attack and 24 dB / 2.8 s decay, alarm debounce, freeze timing (a repeated-grain cadence and small motion are not frozen, a still picture is after `MV_FREEZE_MS`), TSL 5.0 including DLE stuffing, the three tally fields and UTF-16 labels, and a TSL 3.1 datagram, tally lamps, border, and `tally_text` in the overlay, config precedence and exit-78 validation, UUIDv5 ids, domain scan with a mirror domain and unknown JSON fields, TAI index rounding against the MXL test vectors.
- Integration (CI, CPU, real MXL in a temp root): pattern writers; registry stand-in; a persisted route is restored and is the receiver's IS-05 active state; `/readyz` becomes 200; IS-05 activation of a missing flow → `waiting` → writer starts → `running`; output `flow_def.json` matches the raster; sampled pixels carry the tile colours; a layout switch does not reset the flow id and applies on a later frame; a TSL 5.0 datagram puts the LH and RH lamps and the text tally background on the output and its fields into `GET /api/v1/inputs`; `/metrics` exposes `mxl_multiviewer_output_frames_total`; `GET /api/v1/config/export` returns the document; SIGTERM exits 143, the Query API no longer has the node, and `MXL_CLEANUP_ON_EXIT=true` removes the output domain.
- NMOS: `tests/nmos/amwa.sh`.
- Hardware: §13, not in CI.

---

## 15. References

- MXL `218ddaa` — https://github.com/dmf-mxl/mxl
- AMWA IS-04 v1.3, IS-05 v1.2, BCP-007-03, BCP-004-01
- TSL UMD protocol 5.0 — https://tslproducts.com/wp-content/uploads/TSL-UMD-protocol.pdf
- IEC 60268-10 type IIa, EBU Tech 3205
- Sibling specifications in `LeeO86/mxl-decklink`, `LeeO86/mxl-fabrics-agent`, `LeeO86/mxl-webrtc-monitor`
