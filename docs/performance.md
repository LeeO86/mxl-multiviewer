# Performance

The targets in `SPECIFICATION.md` §13 have not been measured on an RTX A4000, an L4, or a Precision 3930-class CPU in this tree. CI runs the CPU backend on a small raster (`192x108p50`) and checks pixels, not the 16×1080p50 budget.

When a hardware run is taken, record:

- GPU, driver, and `MV_BACKEND`
- input count, raster, and rate, and the output raster
- `mxl_multiviewer_compose_seconds` p50/p95
- `mxl_multiviewer_output_frames_late_total` over the run
- input grain origin timestamp versus the output grain index that first showed it

Pass condition: compose time under half a frame (10 ms at 50p) and zero late output frames over one hour, with added latency at most two output frames plus `MV_INPUT_OFFSET_GRAINS`.
