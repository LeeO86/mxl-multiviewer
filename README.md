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

Image `ghcr.io/leeo86/mxl-multiviewer`. Tags: `vX.Y.Z` → `X.Y.Z`, `X.Y`, `X`, `latest`; `main` → `nightly-dev`; every build → `git-<sha>`. Label `io.dmf.mxl.revision` is the MXL pin.

## Run

Host networking. MXL root mounted read-write (the process creates its output domain and reads every other domain, including fabrics mirrors). uid/gid 1000, same as the domain owner.

```bash
MXL_DOMAIN_SCAN_PATH=/Volumes/mxl \
MV_OUTPUT_DOMAIN_DIR=/Volumes/mxl/multiviewer \
NMOS_REGISTRY_ADDRESS=127.0.0.1 \
NMOS_ENABLE=true \
./build/mxl-multiviewer
```

Web, REST, health, and metrics: `WEB_PORT` **8110**. NMOS Node/Connection: `NMOS_PORT` **3262**, WebSocket **3263**. TSL UMD 5.0: UDP **8910**, TCP **8911**. These miss the ports already used by mxl-decklink, mxl-st2110-gateway, mxl-fabrics-agent, mxl-webrtc-monitor, and FlowXer.

Routing is IS-05 only. The UI has no source picker. A layout names input numbers; changing the layout does not change the route.

```bash
# after the node is up, receiver id is in GET /api/v1/info
curl -X PATCH -H 'Content-Type: application/json' \
  -d '{"master_enable":true,"activation":{"mode":"activate_immediate"},"transport_params":[{"mxl_domain_id":"<domain>","mxl_flow_id":"<flow>"}]}' \
  http://127.0.0.1:3262/x-nmos/connection/v1.2/single/receivers/<id>/staged
```

Open `http://<host>:8110/` for the preview, the layout editor, inputs, alarms, and a `KEY=value` export.

`MV_BACKEND=auto` uses CUDA when the binary was built with the toolkit and a device is visible, otherwise CPU. `MV_BACKEND=cuda` on a binary without CUDA exits 78.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | clean shutdown |
| 75 | startup failed |
| 78 | invalid configuration |
| 143 | shutdown grace exceeded |

## Tests

```bash
./build/unit-tests
MXL_LIB_DIR=/opt/mxl/lib tests/integration/mosaic.sh \
  build/mxl-multiviewer build/mxl-mv-writer build/mxl-mv-sample
tests/nmos/amwa.sh   # IS-04-01, IS-05-01, IS-05-02; needs Docker, not default CI
```

Hardware targets are in `docs/performance.md`. They have not been measured on an A4000, an L4, or a Precision 3930-class CPU yet.

## Deploy

`docker/docker-compose.demo.yaml` is a registry stand-in, a pattern writer, and the multiviewer. `docker/docker-compose.host.yaml` is one platform host. `deploy/mxl-multiviewer.yaml` is the Kubernetes Deployment (`hostNetwork`, MXL root hostPath, probes, ServiceMonitor) for `mxl-poc-platform` to vendor. Add `runtimeClassName: nvidia` and `nvidia.com/gpu` on GPU nodes.

## License

Apache-2.0. Vendored `third_party/doctest`, `picojson`, `stb`, and `font8x8_basic.h` keep their own notices.
