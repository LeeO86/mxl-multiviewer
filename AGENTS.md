# Agent notes

- Read `SPECIFICATION.md` and `IMPLEMENTATION_PLAN.md` before changing behaviour. Record every deviation in the plan.
- C++20, CMake, no GStreamer in the media path.
- MXL pin is `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` with `-DMXL_ENABLE_FABRICS_OFI=OFF`. One variable in `docker/Dockerfile` and `.github/workflows/ci.yaml`.
- nmos-cpp pin matches the siblings: `fe303849527394b03bdedc8f161f377fe458bb62`.
- Unit tests do not need libmxl. `tests/integration/mosaic.sh` does, plus the nmos-enabled binary.
- Do not write into a mirror domain (`x-mxl-fabrics-agent.mirror`).
- Config precedence is environment > `MV_CONFIG_FILE` > defaults. Invalid config exits 78.
