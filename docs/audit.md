# Audit of the existing mxl-multiviewer

Date: 2026-10-01. Scope: the tree on `main` at `bcc16f1` before this rewrite.
Sibling references read for alignment: `LeeO86/mxl-decklink`, `LeeO86/mxl-fabrics-agent`, `LeeO86/mxl-webrtc-monitor` (README, SPECIFICATION, IMPLEMENTATION_PLAN, and the NMOS / domain / config code paths).

## What the current repository does

`mxl-multiviewer` is a small Rust workspace (edition 2024, version 0.1.0) with two crates:

| Crate | Role |
| --- | --- |
| `multiviewer-mf` | CLI. Loads `config/multiviewer.json`, lets flags override fields, initialises GStreamer, runs one pipeline until EOS, error, or Ctrl-C. |
| `multiviewer-pipeline` | Builds a fixed GStreamer graph. |

The graph is exactly four `mxlsrc` branches into `compositor`, then `videoconvert` + `capsfilter` + `mxlsink`:

```
mxlsrc → queue (leaky, 2 buffers) → videoconvert → videoscale → caps(I420, half resolution)
      → compositor (fixed 2×2) → videoconvert → caps(v210) → mxlsink
```

Inputs and the output are addressed by flow UUID plus a single domain directory (`mxl_domain`, default `/dev/shm/mxl`). There is no domain scan, no `domain_def.json` identity, no mirror-domain handling, no NMOS node, no tally, no UMD, no audio, no alarms, no timing against TAI grain indices, and no admin UI. The output rate default in `config/multiviewer.json` is `30000/1001`. Scaling goes through I420, so the v210 output is a conversion, not a 10-bit-preserving compose. A missing flow fails the pipeline. The manifest (`manifests/multiviewer-mediafunction.json`) describes the same 4-in / 1-out 2×2 function.

Shutdown is a GStreamer state change to NULL. There are no exit codes 75 / 78 / 143, no `/livez`, no metrics.

## MXL API version it targets

The MXL SDK is a git submodule (`.gitmodules` → `https://github.com/dmf-mxl/mxl`) with **no pinned commit** in this repo. The application never calls the MXL C API. It links the Rust `gst-mxl-rs` plugin (`mxlsrc` / `mxlsink`) built from `mxl/rust`. The Docker image and CI install GStreamer, vcpkg, and libfabric, then `cargo build -p gst-mxl-rs` and `cargo build -p multiviewer-mf`. That is the early gst-mxl integration, not the pinned `release/v1.1` revision `218ddaa` used by `mxl-fabrics-agent` and `mxl-webrtc-monitor`, and not the BCP-007-03 NMOS transport.

The submodule directory in the checkout is empty (not initialised). No MXL headers are vendored.

## What can be kept

- The Apache-2.0 `LICENSE`.
- The product name and the intent “read `video/v210` MXL flows, write one `video/v210` mosaic”.
- CI’s habit of building a container and smoke-testing `--help` — the workflow itself is replaced.
- Nothing from the Rust sources. There is no v210 pack/unpack, no scaler, no flow-descriptor builder, and no config precedence model worth carrying over. `PipelineConfig::tile_dimensions` only checks that the raster is even and divides by two.

## What must be replaced

| Area | Why |
| --- | --- |
| Language and build | Target is C++20 / CMake, same as the siblings. The Rust workspace, Cargo lockfile, and gst-mxl build do not fit. |
| Media path | GStreamer clocked compositor. The target path is TAI-indexed: one output grain per output index, latest input grain not newer than `output_time − input_offset`, CPU and CUDA backends, no GStreamer. |
| Topology | Fixed 4 inputs and a fixed 2×2. Target is up to 32 NMOS receivers, layouts independent of routing, live layout switches. |
| Control | Flow IDs in a JSON file. Target is IS-04 v1.3 / IS-05 v1.2 / BCP-007-03, static registry, real receiver `subscription` updates. |
| Domain model | One directory path. Target scans an MXL root, identity from `domain_def.json` `id`, mirror domains included, output domain created by this process and never a mirror. |
| Ops | None. Target is `/livez` `/readyz` `/statusz` `/metrics`, exit codes, uid 1000, Grafana, Compose, Kubernetes. |
| Overlay, audio, TSL, UI | Absent. |

GStreamer is removed from the media path. It is not used in tests or tools either.

## Decision: rewrite

**Rewrite.** Evolving the Rust pipeline cannot get to the target without replacing every module:

- The media clock has to move from the GStreamer pipeline clock to MXL/TAI grain indices. The compositor element cannot express “for output index N, sample each input at `T − offset`”.
- NMOS receivers are a process-lifetime control plane (nmos-cpp, IS-05 activation of a flow that does not exist yet, per-input rebuild). Bolting that beside a static `mxlsrc` graph duplicates the data path.
- Dynamic layouts, a GPU compose, and an RGBA overlay updated at a capped rate do not map onto `compositor` sink pads created once at startup.
- The siblings’ config, domain scan, metrics, exit codes, and image tagging are C++ / CMake conventions. Keeping a Rust crate would mean two builds in CI for no shared code.

The repository keeps its name, license, and Git history. `crates/`, Cargo files, the gst-mxl Dockerfile, the unpinned submodule, and the 2×2 manifest are deleted by the implementation that follows this audit.

## Mapping from the old config / API to the new one

The old surface was a JSON file plus CLI flags. There is no HTTP API to preserve.

| Old | New | Notes |
| --- | --- | --- |
| `mxl_domain` | `MXL_DOMAIN_SCAN_PATH` plus per-receiver `mxl_domain_id` | One path no longer selects every flow. The scan root is where domains (including `mirror-<id>`) live. |
| `input_flow_ids[4]` | IS-05 `transport_params[0].mxl_flow_id` on receiver `MV In <n> Video` | Routing is not in the multiviewer config. A layout refers to an input number, not a flow id. |
| `output_flow_id` | Derived sender flow id (`NMOS_SEED` + head + format). A raster change mints a new id. | Operators do not pick the output UUID. The crosspoint follows the sender. |
| `output_width`, `output_height` | `MV_OUTPUT_FORMAT` (default `1920x1080p50`) | One token so rate and raster stay consistent. |
| `framerate.numerator` / `denominator` | Inside `MV_OUTPUT_FORMAT` | `5994` means `60000/1001`. |
| `interlace_mode` | Output is progressive only. | An interlaced output string is a config error (exit 78). |
| `colorimetry` | BT.709 in the flow descriptor. | Not configurable. v210 is 10-bit Y′CbCr 4:2:2. |
| CLI `--config` | `MV_CONFIG_FILE` | Flat `KEY=value` JSON, env > file > default, same model as mxl-decklink. |
| GStreamer `compositor` 2×2 | Built-in layout preset `2x2` | The preset is the migration equivalent of the old fixed mosaic. |
| `manifests/multiviewer-mediafunction.json` | Kubernetes manifests under `deploy/` | The old manifest was descriptive only. |

No external caller of the Rust CLI is known. The old flags are not accepted by the new binary.
