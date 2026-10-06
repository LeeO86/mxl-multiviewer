# Changelog

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
