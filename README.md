# mxl-multiviewer

DMF multiviewer MediaFunction implemented in Rust + GStreamer, using the
official MXL SDK and gst-mxl-rs for DMF I/O. The MXL SDK is included as a git
submodule and built alongside the multiviewer. Upstream reference:
[dmf-mxl/mxl](https://github.com/dmf-mxl/mxl).

## Repository layout

```
.
├── mxl/                           # MXL SDK submodule (upstream)
├── crates/
│   ├── multiviewer-mf/            # MediaFunction runtime (CLI + lifecycle)
│   └── multiviewer-pipeline/      # GStreamer pipeline builder
├── config/
│   └── multiviewer.json           # Default runtime configuration
├── manifests/
│   └── multiviewer-mediafunction.json
├── docker/
├── scripts/
│   └── build.sh
├── Dockerfile
└── .github/workflows/ci.yml
```

## Build prerequisites (Linux)

This repository builds:
- MXL native library (via `mxl-sys` build script in the submodule).
- gst-mxl-rs GStreamer plugin.
- Multiviewer MediaFunction binary.

Dependencies are aligned with MXL's devcontainer setup:
- build tools: `clang`, `cmake`, `ninja-build`, `pkg-config`, `git`
- GStreamer dev/runtime: `libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`,
  `gstreamer1.0-plugins-{good,bad,ugly}`
- GTK/GStreamer headers for plugin builds: `libglib2.0-dev`, `libgirepository1.0-dev`,
  `libgdk-pixbuf2.0-dev`, `libcairo2-dev`, `libpango1.0-dev`, `libgraphene-1.0-dev`,
  `libgtk-4-dev`, `libatk1.0-dev`
- `librdmacm-dev`
- `vcpkg` (required by MXL CMake presets)
- `libfabric` (install via `mxl/scripts/common/libfabric/install.sh`)

## Devcontainer

A devcontainer is provided to match the MXL Ubuntu 24.04 toolchain and build
requirements. It includes clang, CMake/Ninja, GStreamer dev/runtime packages,
vcpkg, and libfabric (installed during image build). The required devcontainer
scripts are vendored under `.devcontainer/scripts` so the build does not depend
on submodule initialization.

1. Open the repo in VS Code/Cursor.
2. Reopen in container when prompted (or use the command palette).
3. The container runs `git submodule update --init --recursive` automatically.
4. Build with `./scripts/build.sh`.

## Build (local)

1. Initialize the submodule:
   ```bash
   git submodule update --init --recursive
   ```
2. Ensure `VCPKG_ROOT` is set (example):
   ```bash
   export VCPKG_ROOT="$HOME/vcpkg"
   ```
3. Build:
   ```bash
   ./scripts/build.sh
   ```

This builds:
- `mxl/rust/target/release/libgstmxl.so` (gst-mxl-rs plugin)
- `target/release/multiviewer-mf` (MediaFunction)

## Run (local)

The GStreamer plugin and `libmxl.so` must be discoverable at runtime.
If you built via `./scripts/build.sh`:

```bash
export GST_PLUGIN_PATH="$(pwd)/mxl/rust/target/release"
export LD_LIBRARY_PATH="$(pwd)/mxl/rust/target/release/build/mxl-sys-*/out/build/lib:$(pwd)/mxl/rust/target/release/build/mxl-sys-*/out/build/lib/internal"

./target/release/multiviewer-mf --config config/multiviewer.json
```

Update `config/multiviewer.json` to match your input/output flow IDs.

## MediaFunction definition

- Inputs: 4x DMF video flows (`video/v210`)
- Output: 1x DMF video flow (`video/v210`)
- Configuration: domain path, 4 input flow IDs, output flow ID, output resolution,
  framerate, interlace mode, colorimetry.

Example config file: `config/multiviewer.json`.

## GStreamer pipeline (conceptual)

```
mxlsrc (x4) -> queue -> videoconvert -> videoscale -> caps(I420 half res)
     -> compositor (2x2) -> videoconvert -> caps(v210 full res) -> mxlsink
```

The pipeline is built in `crates/multiviewer-pipeline`.

## Container build & run

```bash
docker build -t mxl-multiviewer .
docker run --rm \
  -v /dev/shm/mxl:/domain \
  -e GST_PLUGIN_PATH=/app \
  -e LD_LIBRARY_PATH=/app \
  mxl-multiviewer --config /app/config/multiviewer.json
```

## CI

`.github/workflows/ci.yml` builds:
- MXL + gst-mxl-rs (via Cargo in `mxl/rust`)
- multiviewer MediaFunction
- container image + basic `--help` smoke test

## Deployment notes

- Deploy alongside other DMF MediaFunctions with `/dev/shm/mxl` (or equivalent)
  mounted into the container as `/domain`.
- Wire 4 producer flows into the input flow IDs configured, and consume the
  output flow ID downstream.
- Scaling: one instance handles a single 2x2 layout. For more sources, run
  multiple instances or extend to NxN layouts.
- Limitations: CPU-bound compositor/scaler, fixed v210 formats, no dynamic flow
  discovery or hot-plug.
