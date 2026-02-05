#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Building gst-mxl-rs (and MXL native library)..."
pushd "${ROOT_DIR}/mxl/rust" >/dev/null
cargo build -p gst-mxl-rs --release
popd >/dev/null

echo "Building multiviewer MediaFunction..."
pushd "${ROOT_DIR}" >/dev/null
cargo build -p multiviewer-mf --release
popd >/dev/null

echo "Done."
