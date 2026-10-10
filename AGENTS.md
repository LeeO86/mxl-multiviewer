# Agent notes

- Read `SPECIFICATION.md` and `IMPLEMENTATION_PLAN.md` before changing behaviour. Record every deviation in the plan.
- C++20, CMake, no GStreamer in the media path.
- MXL pin is `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` with `-DMXL_ENABLE_FABRICS_OFI=OFF`. One variable in `docker/Dockerfile` and `.github/workflows/ci.yaml`.
- nmos-cpp pin matches the siblings: `fe303849527394b03bdedc8f161f377fe458bb62`.
- Unit tests do not need libmxl. `tests/integration/mosaic.sh` does, plus the nmos-enabled binary.
- The binary links the system FFmpeg (`libavcodec`, `libavformat`, `libavutil`) for the WebRTC preview. `tests/integration/preview.sh` needs a MediaMTX binary (`MEDIAMTX_BIN`, the 1.20.1 release in CI).
- The container image builds on `nvidia/cuda:12.8.2-devel-ubuntu24.04` and statically links the CUDA runtime. It still starts with no GPU. A GPU host needs the NVIDIA container toolkit (`--gpus all`, `docker/docker-compose.gpu.yaml`, or `deploy/mxl-multiviewer-gpu.yaml`) so `libcuda` is injected. `ci.yaml` does not install nvcc.
- Do not write into a mirror domain (`x-mxl-fabrics-agent.mirror`).
- Config precedence is environment > `MV_CONFIG_FILE` > defaults. Invalid config exits 78.
