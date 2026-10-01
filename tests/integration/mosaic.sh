#!/usr/bin/env bash
# CPU integration: IS-05 activation of a missing flow, then a real MXL writer,
# sampled output pixels, a layout switch, and a metrics scrape.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-multiviewer}"
WRITER="${2:-$ROOT/build/mxl-mv-writer}"
SAMPLE="${3:-$ROOT/build/mxl-mv-sample}"
LIB_DIR="${MXL_LIB_DIR:-/tmp/mxl-install/lib}"

if [[ ! -x "$BIN" || ! -x "$WRITER" || ! -x "$SAMPLE" ]]; then
  echo "missing binaries (pass mxl-multiviewer mxl-mv-writer mxl-mv-sample)" >&2
  exit 1
fi

WORK="$(mktemp -d /dev/shm/mv-int-XXXXXX)"
cleanup() {
  if [[ -n "${MV_PID:-}" ]]; then kill "$MV_PID" 2>/dev/null || true; wait "$MV_PID" 2>/dev/null || true; fi
  if [[ -n "${W1:-}" ]]; then kill "$W1" 2>/dev/null || true; fi
  if [[ -n "${REG_PID:-}" ]]; then kill "$REG_PID" 2>/dev/null || true; fi
  rm -rf "$WORK"
}
trap cleanup EXIT

DOMAIN_ID="aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
FLOW_ID="bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
REG_PORT=13210
WEB_PORT=18110
NMOS_PORT=13262

mkdir -p "$WORK/src"
cat > "$WORK/src/domain_def.json" <<EOF
{"id":"$DOMAIN_ID","label":"src","unused":true}
EOF
echo '{"urn:x-mxl:option:history_duration/v1.0":200000000}' > "$WORK/src/options.json"

python3 "$ROOT/tests/integration/fake_registry.py" "$REG_PORT" >"$WORK/registry.log" 2>&1 &
REG_PID=$!

export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
MXL_DOMAIN_SCAN_PATH="$WORK" \
MV_OUTPUT_DOMAIN_DIR="$WORK/mv" \
MV_BACKEND=cpu \
MV_MAX_INPUTS=4 \
MV_OUTPUT_FORMAT=192x108p50 \
MV_ACTIVE_LAYOUT=2x2 \
MV_AUDIO_CHANNELS=0 \
TSL_ENABLE=false \
NMOS_ENABLE=true \
NMOS_REGISTRY_ADDRESS=127.0.0.1 \
NMOS_REGISTRY_PORT="$REG_PORT" \
NMOS_PORT="$NMOS_PORT" \
NMOS_SEED=test-seed \
WEB_PORT="$WEB_PORT" \
LOG_LEVEL=info \
"$BIN" >"$WORK/mv.log" 2>&1 &
MV_PID=$!

for _ in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:${WEB_PORT}/livez" >/dev/null; then
    break
  fi
  if ! kill -0 "$MV_PID" 2>/dev/null; then
    echo "multiviewer exited early" >&2
    cat "$WORK/mv.log" >&2
    exit 1
  fi
  sleep 0.2
done
curl -sf "http://127.0.0.1:${WEB_PORT}/livez" >/dev/null

INFO="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/info")"
RX="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["receivers"][0]["video"])' "$INFO")"

patch_body="$(python3 - <<PY
import json
print(json.dumps({
  "sender_id": None,
  "master_enable": True,
  "activation": {"mode": "activate_immediate"},
  "transport_params": [{"mxl_domain_id": "$DOMAIN_ID", "mxl_flow_id": "$FLOW_ID"}]
}))
PY
)"

CODE="$(curl -s -o "$WORK/patch.txt" -w '%{http_code}' -X PATCH \
  -H 'Content-Type: application/json' \
  -d "$patch_body" \
  "http://127.0.0.1:${NMOS_PORT}/x-nmos/connection/v1.2/single/receivers/${RX}/staged")"
if [[ "$CODE" != "200" && "$CODE" != "202" ]]; then
  echo "IS-05 PATCH failed: $CODE" >&2
  cat "$WORK/patch.txt" >&2
  exit 1
fi

waiting=0
for _ in $(seq 1 40); do
  STATE="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/inputs" | python3 -c 'import json,sys; print(json.load(sys.stdin)["inputs"][0]["video"]["state"])')"
  if [[ "$STATE" == "waiting" ]]; then
    waiting=1
    break
  fi
  sleep 0.25
done
if [[ "$waiting" != 1 ]]; then
  echo "expected waiting, got ${STATE:-none}" >&2
  curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/inputs" >&2 || true
  exit 1
fi

"$WRITER" --domain "$WORK/src" --flow "$FLOW_ID" --width 192 --height 108 --rate 50 --y 200 --cb 512 --cr 512 \
  >"$WORK/writer.log" 2>&1 &
W1=$!

running=0
for _ in $(seq 1 50); do
  STATE="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/inputs" | python3 -c 'import json,sys; print(json.load(sys.stdin)["inputs"][0]["video"]["state"])')"
  if [[ "$STATE" == "running" ]]; then
    running=1
    break
  fi
  sleep 0.2
done
if [[ "$running" != 1 ]]; then
  echo "expected running, got ${STATE:-none}" >&2
  tail -50 "$WORK/mv.log" >&2
  exit 1
fi

OUT="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/outputs")"
FLOW_OUT="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["outputs"][0]["video_flow_id"])' "$OUT")"
python3 - "$WORK/mv/$FLOW_OUT.mxl-flow/flow_def.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
assert doc["frame_width"] == 192
assert doc["frame_height"] == 108
assert doc["media_type"] == "video/v210"
assert doc["grain_rate"]["numerator"] == 50
PY

PIXEL="$("$SAMPLE" --domain "$WORK/mv" --flow "$FLOW_OUT" --x 24 --y 20 --width 192)"
echo "pixel=$PIXEL"
if [[ "$PIXEL" != "200" ]]; then
  echo "expected tile colour 200" >&2
  exit 1
fi

curl -sf -X POST "http://127.0.0.1:${WEB_PORT}/api/v1/layouts/1/activate" >/dev/null
sleep 0.4
OUT2="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/outputs")"
FLOW_OUT2="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["outputs"][0]["video_flow_id"])' "$OUT2")"
if [[ "$FLOW_OUT2" != "$FLOW_OUT" ]]; then
  echo "layout switch minted a new flow id" >&2
  exit 1
fi
PIXEL2="$("$SAMPLE" --domain "$WORK/mv" --flow "$FLOW_OUT" --x 96 --y 40 --width 192)"
echo "pixel_after_switch=$PIXEL2"
if [[ "$PIXEL2" != "200" ]]; then
  echo "full-frame layout did not show input 1" >&2
  exit 1
fi

curl -sf "http://127.0.0.1:${WEB_PORT}/metrics" | grep -q "mxl_multiviewer_output_frames_total"
echo "integration ok"
