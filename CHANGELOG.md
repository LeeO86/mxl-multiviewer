# Changelog

## 1.4.0

WebRTC previews of the heads as one stream, the platform's preview contract (platform spec §11.5, D-185; as mxl-webrtc-monitor 1.3.0), and operator-screen widgets. Without new settings the multiviewer behaves as 1.3.0 (JPEG previews).

### Preview

- `MV_PREVIEW_MODE` (new, `jpeg` or `webrtc`, default `jpeg`). Never both: `jpeg` is the 1.3.0 JPEG per head and publishes nothing; `webrtc` encodes no JPEG (`/preview.jpg` answers 404) and puts every head into one 1920×1080 mosaic: one head fills it, two to four heads get a 960×540 quarter each (2×2, reading order; other aspect ratios keep theirs, centred). Each head is scaled into its place at `MV_PREVIEW_FPS` on the GPU in the compose path (CPU backend: on the CPU), and one thread encodes the mosaic once: H.264 with NVENC, or x264 when NVENC cannot be opened (logged as `preview_nvenc_unavailable`), 4 Mbit/s CBR, one-second GOP.
- `GET /api/v1/preview/map`: each head's `x`, `y`, `w`, `h` in the mosaic. The Preview tab plays the one WebRTC stream and shows the selected head's region (CSS `object-view-box`, Chromium-based browsers); in `jpeg` mode it shows the JPEG as before.
- Preview contract: `PREVIEW_PUBLISH_URL` (RTSP base of a shared MediaMTX; set, the stream goes there and no MediaMTX is started), `PREVIEW_PATH_PREFIX` (default `mxl-multiviewer`; the stream is `<prefix>/heads`), `PREVIEW_WHEP_URL` and `PREVIEW_HLS_URL` (public bases; empty is the own MediaMTX). Without `PREVIEW_PUBLISH_URL` the image's MediaMTX 1.20.1 (`/usr/local/bin/mediamtx`, MIT licence in `/usr/share/doc/mediamtx/`) runs as a supervised child process (restart with backoff, SIGTERM and SIGKILL on shutdown) on its own ports: `MEDIAMTX_RTSP_PORT` 8754 (127.0.0.1), `MEDIAMTX_WHEP_PORT` 8789, `MEDIAMTX_HLS_PORT` 8788, `MEDIAMTX_ICE_UDP_PORT` 8389 (UDP and TCP). They miss mxl-webrtc-monitor's and the FlowXer engine's.
- The publish state (`connecting`, `publishing`, `error` with the error), the encoder, the mode and the built-in MediaMTX are in `/statusz` (`preview`) and the metrics `mxl_multiviewer_preview_mode{mode}`, `preview_publish_mode{mode}`, `preview_publish_state{state}`, `preview_encoder{encoder}`, `preview_frames_total`, `preview_encode_seconds`, `preview_seconds{head}` and `compose_gpu_seconds{stage="preview"}`. `GET /api/v1/info` has `preview_mode` and the playback URLs (`preview`). Readiness does not depend on the preview.
- `MV_OUTPUTS` goes up to 4 (`MV_OUT4_*`), so four heads fill the 2×2 mosaic.

### Tile rectangle (correction of the layout options)

- A tile's rectangle is its hard boundary: nothing of the tile is drawn outside it (picture, caption bar, audio bars, lamps, borders, alarm labels). The overlay clips every tile to its rectangle.
- Caption (UMD): `umd_position` is `top` or `bottom` (default `bottom`), and the new `umd_overlay` (default true) says whether the bar lies over the picture or has its own strip of the tile, the picture scaled into the rest. The 1.3.0 values are accepted and migrated: `top-inside`/`bottom-inside` are that edge over the picture; `top-outside`/`bottom-outside`, which drew the bar outside the tile, are now that edge in a strip inside the tile. There is no position outside the tile any more.
- Audio bars: `audio_bar_position` is `left`, `right` (default) or `centre`, and the new `audio_bar_overlay` (default true) says whether they lie over the picture or have their own strip (left and right only; centred bars are always over the picture). The 1.3.0 values are accepted and migrated: `left-beside`/`right-beside` are that side in a strip, `overlay` is `centre`.
- With a caption strip and a bar strip the picture's area is the tile less both; fit and fill work on that area as before. CPU and CUDA place the picture the same way.
- A layout file with 1.3.0 values is read as before; each value is logged (`layout_repaired`) and the file uses the new names from the next save, which 1.3.0 cannot read.
- Layout editor: "Position" (Top, Bottom; Left, Centre, Right) and "Over the video" replace the long lists; the canvas shows a strip with a dashed edge. Imported layouts with 1.3.0 values open with the new options.

### Widgets

- `GET /widgets` lists the operator-screen widgets: `head` (`{head}`, one head's preview, WebRTC region or JPEG, at least 480×270) and `tile-editor` (`{head}`, the tiles of the layout the head shows and the layout editor's tile inspector with Save, at least 600×400). Origins listed in `WIDGET_FRAME_ANCESTORS` get `Access-Control-Allow-Origin` on it (GET).
- `GET /widget/<id>?head=<h>[&theme=dark|light|transparent]`: the widget page without app chrome, on this multiviewer's own API. The `/widget` routes send `Content-Security-Policy: frame-ancestors <WIDGET_FRAME_ANCESTORS>` (new, default `'self'`) and no `X-Frame-Options`. The page posts `widget-ready` and `widget-size` to its parent.

### Build

- The build needs `libavcodec-dev`, `libavformat-dev` and `libavutil-dev`; the image installs `libavcodec60`, `libavformat60` and `libavutil58` (Ubuntu's FFmpeg 6.1, a GPL build because of x264) and copies `/mediamtx` from `bluenviron/mediamtx:1.20.1`. It sets `NVIDIA_DRIVER_CAPABILITIES=compute,video,utility`, so the container toolkit injects the NVENC library.
- CI runs `tests/integration/preview.sh` against the MediaMTX 1.20.1 release binary.

## 1.3.0

TSL 5.0 tally per field, and display options from the platform rollout of 1.2.1.

### Overlay

- The left lamp of a caption shows the TSL LH tally and the right lamp the RH tally. Before, both lamps showed one combined colour. An off lamp is still not drawn.
- The border keeps the combined colour: text tally, else RH, else LH.
- Alarm display per tile: `alarm_border` and `alarm_labels` (both on by default) and `alarm_label_position` (`top-left`, `top`, `top-right`, `bottom-left`, `bottom`, `bottom-right`; default `top`). Every active alarm now has its own label in its own colour, stacked from that position; before, a tile showed only the most severe one. With one alarm the default looks as before.
- Image tiles (`content: image`): a picture from an http(s) URL (`image_url`) or one stored in the configuration volume (`image_file`), PNG, JPEG, GIF, or WebP, fit or fill. Animated GIFs play. At most 8 MiB, 4096 × 4096 pixels, 32 megapixels over all frames; type, magic bytes, and size are checked. URLs are fetched on a worker thread (3 s to connect, 10 s in all, through the environment's proxy), never on the render path; a failure shows `NO IMAGE` with the reason and is retried after 30 s. Image tiles are drawn over the video tiles.
- Audio bars beside the picture: `audio_bar_position` `left-beside` and `right-beside` give the bars a strip of the tile and scale the picture into the rest (CPU and CUDA alike). `left`, `right`, and `overlay` stay over the picture.
- New tile option `audio_bar_scale` (default on): off hides the level scale (ticks and dBFS labels) beside the audio bars.
- New tile option `umd_align`: the caption text sits left (default, as before), centre, or right. Text wider than the bar is cut and ends with `…`.
- New option `tally_text`: the text tally colours the caption background (the text turns black on it) while the text tally is not off. The layout sets the default (`tally_text`, false); a tile can set `true` or `false`, or `null` to follow the layout. A head gets it by showing a layout that has it.

### Build

- The image installs `tzdata`, `libcurl4t64`, `libwebp7`, and `libwebpdemux2`; building needs `libcurl4-openssl-dev` and `libwebp-dev`.

### Behaviour

- UTF-16 TSL labels are decoded as UTF-16; non-ASCII characters (`ü`) were broken on the wall and in the API.
- A TSL 3.1 tally sets both lamps and the text tally, so 3.1 looks as before.
- Freeze: the alarm rises when the picture has not changed for `MV_FREEZE_MS` (new setting, default 2000, at least 1000) and clears after `MV_ALARM_CLEAR_MS` of motion. Before, it compared two grains in a row: a source that repeats grains (25p in 50p, the browser source) kept the alarm up for good once a still moment had raised it. The hash now sums the luma of every second line per block of a 32×18 grid, so a clock or a ticker counts as motion; it sampled about 4096 single pixels before.

- Start layout per output: "Use as start layout" in the layout editor stores the layout an output shows after a restart (`start_layouts` in `layouts.json`). Activating another layout does not change it. At start, `MV_OUT<h>_LAYOUT` or `MV_ACTIVE_LAYOUT` set in the environment still win; after them come the start layout, then the layout shown last (as in 1.2.1).
- Clocks with local time drew UTC: the image had no time zone database. The image now has `tzdata`, so `TZ` works; the new setting `MV_TIMEZONE` (an IANA name such as `Europe/Zurich`, checked at start) overrides it.

### Web UI

- Layout editor: "Text tally as caption background" per tile (as the layout, on, off) and per layout.
- The layout editor draws local clocks in the multiviewer's time zone, not the browser's.
- Layout editor: "Use as start layout" per output, with a note when the environment sets the start layout.
- Layout editor: "+ Image" tiles with a web address or a stored picture (upload, choose, delete) and a preview on the canvas.
- Layout editor: an Alarms panel per input tile (border, labels, where the labels stack).
- Layout editor: audio bar positions grouped as over or beside the picture; the tile preview shows the strip.
- Layout editor: audio bar level scale on or off ("Apply to all input tiles" copies it too).
- Layout editor: caption text alignment. The caption positions are grouped and explained: "over the picture" (in the tile; the picture keeps its size) or outside the tile.

### API

- `GET /api/v1/inputs`, `/statusz`, and the WebSocket: `tsl_lh`, `tsl_rh`, and `tsl_text_tally` per input, next to `tally` and `tsl_text`.
- Layouts carry `tally_text` on the layout and on each tile. A value that is not `true` or `false` (or `null` on a tile) is rejected with 400.
- `GET /api/v1/info`: `timezone` (the zone of local clocks) and `utc_offset_s`.
- `GET /api/v1/images`, and `GET`, `PUT`, `DELETE /api/v1/images/{name}`: stored pictures of image tiles. Request bodies over 16 MiB are refused with 413.
- Layouts: tile options `alarm_border`, `alarm_labels`, `alarm_label_position`, `umd_align`, `audio_bar_scale`, `image_url`, `image_file`; `audio_bar_position` `left-beside` and `right-beside`; `content` `image`. Bad values are rejected with 400. Saved layouts without them load as before and look the same.
- `PUT /api/v1/outputs/{h}` takes `start_layout` (a layout name, or `null`); each output in `GET /api/v1/outputs` and the WebSocket has `start_layout` and `start_layout_env`. The layout book has `start_layouts`.

## 1.2.1

Fixes from the platform rollout of 1.2.0.

### Behaviour

- A head starts on the layout last chosen for it (Activate, or `PUT /api/v1/outputs/{h}`), else on `MV_OUT<h>_LAYOUT` when set, else on the layout book's active layout. Before, every head started on `MV_ACTIVE_LAYOUT` (default `2x2`) although `layouts.json` named another active layout, and a layout set with `PUT /api/v1/outputs/{h}` was lost at the next start. The book file keeps the choices in `heads`.
- Layouts that are unedited 1.1.x built-in presets get the 1.2 preset defaults (audio bars on) at the first start; edited layouts stay as they are. The original file is kept once as `layouts.json.bak`, `layouts_migrated` lists the layouts, and the book records `preset_revision: 2`, so later starts change nothing. An imported 1.1.x book is migrated the same way.

### Web UI

- Layout editor: "Preset defaults…" resets a built-in preset to today's definition (after a confirmation, unsaved until Save); "Apply to all input tiles" copies the audio bar settings of the selected tile to every input tile.

### API

- `GET /api/v1/presets`: today's built-in presets.
- `GET /api/v1/layouts` and the export carry `heads` and `preset_revision`.

## 1.2.0

### Web UI

- New look shared with the other LeeO86 MXL UIs (mxl-st2110-gateway, mxl-browser-source): header with node label, status pills and version, banners for errors and lost connections, tabs that keep their place in the address (`#layout`), panels, pills, light and dark theme.
- **Layout editor**: add input, clock, label, and empty tiles; duplicate, delete, forward/backward; drag, resize, and arrow keys snap to `MV_GRID` (from the API; it was fixed at 24); a preview of each tile with its source name, caption, live audio levels, or clock. The inspector sets the tile content (input and scale, analogue or digital clock with time zone and timecode, label text), the caption (UMD, explained on the page: NMOS sender label, fixed text, or TSL, with its text, position, size, and background), audio bars (channels, first channel, position, RMS, zones), tally, overlays, and the layout background. Save, Save as, Discard, New, Delete, Activate, Import and Export of one layout or the whole book. Unsaved changes are marked and survive tab switches; a lost WebSocket no longer reloads the layouts and wipes edits.
- **Preview**: picture at `MV_PREVIEW_FPS` (was polled once a second), head selector, layout on air with Activate, output counters.
- **Inputs**: video and audio state, source, format, live PPM bars with peak hold and clip per channel, alarms, receiver ids, how to route.
- **Alarms**: table with severity and since when; explanation of each alarm.
- **Settings**: every setting with its origin and a one-line description, editing of file values, JSON and `KEY=value` export (download, copy), import from a file or pasted text.

### Overlay

- Built-in presets show audio bars on every input tile (two channels on the right). Saved layouts keep their setting.
- Audio bars have a PPM scale (0 to −60 dBFS) with labels when there is room, zones by position on the bar, a 2 s peak-hold line, and a clip light. They are drawn on input tiles only, keep clear of the caption, and are dim and crossed out when the input has no audio routed.
- Slates: after `MV_HOLD_MS` without a frame the tile is black with `NO SIGNAL` (or `WAITING` while the flow is missing) and the input name; an input without a video route shows `NOT ROUTED`. Before, the last frame stayed forever.
- The layout `background` colour is drawn on the CPU and CUDA backends (it was always black); letterbox areas stay black.
- Alarm borders inside the tally border, red for no signal, black, freeze, and clip, amber for silence and format; amber badges for amber alarms.
- Digital clocks, timecode, labels, and slates are sized to the tile; `clock_style` accepts `analog`; unknown clock styles and zones are rejected instead of becoming digital UTC.

### Behaviour

- The `is04` caption is the routed sender's label from the registry Query API (looked up by sender id, or by flow id when the route has none), then the tile's text, then the MXL flow label, then `MV In <n>`.
- The audio leg is read on its own: from its own domain and sender, without a video route, re-opened on a re-route, metered on every sample, with the states `running`, `waiting`, and `no_signal`.
- The `no_signal` alarm rises (it never did); the video state goes `holding` and then `no_signal` when frames stop. `silence` also rises when routed audio does not arrive.
- An input recovers when its source writer restarts (the writer re-creates the flow): video and audio open the new flow without a re-route. Before, video stayed on the old flow with a frozen picture.
- Audio-follow copies only the samples of each output frame from a ring buffer.
- Activating a layout or changing `audio_follow` while a frame was being composed could be undone by that frame; on the CPU backend most activations were lost. Fixed.
- Layout names with `+`, spaces, or quotes work in the API paths (percent-decoded), and quotes and backslashes in names, captions, and labels no longer break `GET /api/v1/layouts` and `layouts.json`.
- `POST /api/v1/config/import` skips settings that come from the environment (listed in `skipped`) instead of failing.
- Every head keeps a layout that exists: an import that drops a head's layout switches that head to the book's active layout (`heads_moved`), a book without layouts is refused, a layout on a head cannot be deleted, and `PUT /api/v1/outputs/{h}` rejects an unknown layout (404) and `audio_follow` outside 0 to `MV_MAX_INPUTS` (400).
- Audio bars stay within the 16 metered channels (first channel + count ≤ 16).
- `layouts.json` from an older release with values 1.2.0 rejects (audio zones out of order, bars past channel 16, unknown clock values) is repaired at start and logged; a file that cannot be read is renamed to `layouts.json.bad` instead of being overwritten by the next save.

### API

- `GET /preview.jpg?head=<h>`: the preview of heads 2 and 3.
- `GET /api/v1/alarms`: `severity` and `since` per alarm.
- `GET /api/v1/inputs` and the WebSocket: `hold_dbfs` and `clip` per channel.
- `GET /api/v1/info`: `label`, `grid`, `preview_fps`, `hold_ms`.

## 1.1.3

- A new output `domain_def.json` carries `tags` (empty), as BCP-007-03 requires (`id`, `label`, `description`, `tags`). An existing file is still not rewritten. The integration test checks the four fields.

## 1.1.2

### Performance

- The CPU backend keeps real time. On the lab (2× Xeon Gold 6136, no GPU, 1080p50 output, 4x4 layout) 4 inputs went from 973 of 1000 frames (572 late) on 3.7 cores to every frame on 1.7 cores, and 16 inputs from 503 of 1000 frames on 11.7 cores to every frame (1 late) on 5.9 cores (`docs/performance.md`).
  - Readers keep a pooled copy of the packed grain instead of unpacking it into a new 8 MB planar frame; the scaler unpacks only the source lines a tile touches and uses integer taps computed once per placement (within 1 of the float scaler, exact on flat areas).
  - The black and freeze alarms take their samples while the grain is copied line by line; as a separate pass they read most of each frame from memory again.
  - Tiles run on a persistent worker pool into reused images instead of a new thread and image per tile per frame.
  - The overlay is converted to YCbCr once per drawing on the overlay thread; the compose thread blends only its visible spans, in integers.
- The CUDA backend uses less CPU (16 inputs: 2.9 → 1.8 cores): the alarms read the packed grain in one pass.

## 1.1.1

### Behaviour

- After a restart, a route restored from `routes.json` is also the receiver's IS-05 `active` and `staged` document (`master_enable`, `sender_id`, `mxl_domain_id`, `mxl_flow_id`) and its IS-04 subscription. The input was routed, but IS-05 said inactive, so a controller saw a disconnected receiver (platform guideline G9).

## 1.1.0

### Performance

The CUDA backend no longer moves frames through the CPU. Measured on an NVIDIA A16 (one GA107, PCIe Gen4 x4), 16×1080p50 into 1080p50 went from 581 of 1500 frames to all frames with no late frame, and 16×1080p50 into 2160p50 ran one hour without a late or missed frame (compose 8.6 ms, 3.5 cores); numbers in [docs/performance.md](docs/performance.md).

- Each input grain is uploaded once, by its input thread on its own CUDA stream, straight from the MXL grain (page-locked on first use) into a device frame. Compose reads the packed v210 in place. On the CUDA backend the CPU no longer unpacks, copies or allocates per grain; black and freeze read the same luma samples from the packed grain.
- Every head composes from the same device frames and writes the packed result by DMA into the open MXL output grain.
- The overlay is drawn on its own thread per head at `MV_OVERLAY_HZ`. Compose uploads only the areas that changed since the overlay on the device; the background is uploaded once.
- Inputs wait for their next grain instead of polling every 2 ms, and parse the flow definition once per opened flow.
- The preview reads the written grain at preview size instead of unpacking the whole output.

### Metrics

- `mxl_multiviewer_compose_gpu_seconds{head,stage}`: GPU time per compose stage (`background`, `tiles`, `overlay`, `pack`, `download`) from CUDA events.

### Behaviour

- On the CUDA backend an input whose upload fails is unpacked on the CPU as before. When compose fails for about half a second in a row, all inputs go back to the CPU (`cuda_disabled` in the log); before that, failed frames show those inputs black.

## 1.0.0

Stable settings, HTTP API, and process behaviour for the MXL platform. A later break is 2.0.0.

### Settings

New names, all optional. Existing names keep working.

| Name | Default | Notes |
| --- | --- | --- |
| `MV_STATE_DIR` | `/config` | `config.json`, `layouts.json`, and `routes.json` |
| `MXL_CLEANUP_ON_EXIT` | `false` | remove this process's output domain on shutdown |
| `NMOS_QUERY_ADDRESS` | registry address | Query API host |
| `NMOS_QUERY_PORT` | registry port + 1 | Query API port |
| `NMOS_LABEL` | empty (`HOST_ID`) | node label and device label prefix |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | the only announced address |
| `NMOS_TAGS` | `{}` | JSON object of string arrays on the node and device |
| `MXL_OUTPUT_DOMAIN_DIR` | | alias of `MV_OUTPUT_DOMAIN_DIR` |
| `MXL_OUTPUT_DOMAIN_ID` | | alias of `MV_OUTPUT_DOMAIN_ID` |

`HOST_ID` is still the label and the default seed. It is not the node href. `NMOS_DNS_SD=false` skips DNS-SD browse and mDNS advertisement. The image links the DNS-SD client library and does not run avahi-daemon.

Layouts used to be stored beside `MV_CONFIG_FILE` when that was set and `MV_LAYOUTS_FILE` was empty. They now default to `<MV_STATE_DIR>/layouts.json`. Set `MV_LAYOUTS_FILE` to keep the old path.

### Behaviour

- SIGTERM and SIGINT exit 143 after media is released, the NMOS node is deleted from the registry, and (when asked) the output domain directory is removed. Exit 0 remains `--help`.
- A TCP or UDP port that cannot be bound, including TSL, exits 75.
- An existing `domain_def.json` with a different id is logged and left as it is.
- `WEB_ENABLE=false` returns 404 for the UI, the preview, and mutating methods. Probes, metrics, and GET APIs stay.
- Active media routes are restored from `<MV_STATE_DIR>/routes.json`. The IS-05 active document is rebuilt inactive until the next PATCH.
- `/readyz` is 200 only while the composer is fresh and, when a registry is configured, the Query API lists the node.

### API

- `GET /api/v1/config/export` returns one JSON document: `version`, `secrets`, `settings`, `layouts`, `routes`.
- `POST /api/v1/config/import` restores that document. There are no secrets, so `secrets` is false and nothing is omitted. Routes apply on the next start.

### Image and deploy

- `ghcr.io/leeo86/mxl-multiviewer` on `vX.Y.Z` publishes `X.Y.Z`, `X.Y`, and `X`. Those tags are not moved. `main` publishes `nightly-dev` and `git-<sha>`. The `latest` tag is no longer published.
- Kubernetes examples use the pod network, `/config`, `/livez` and `/readyz`, and `ghcr.io/leeo86/mxl-multiviewer:1.0.0`.
