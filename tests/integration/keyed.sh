#!/usr/bin/env bash
# CPU integration (1.4.5): a v210a input (fill + key) is laid over what is under its tile. Up to
# 1.4.4 the CPU backend blended the key into the tile's own image and then copied that image
# opaque onto the canvas, so the picture under a keyed tile was hidden. Input 1 is an opaque
# picture (luma 800) on the left three quarters; inputs 2-4 are luma 200 with key 0, 1023 and 512
# in tiles over it; input 4 is also shown over the layout background (#000000, luma 64).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-multiviewer}"
WRITER="${2:-$ROOT/build/mxl-mv-writer}"
SAMPLE="${3:-$ROOT/build/mxl-mv-sample}"
LIB_DIR="${MXL_LIB_DIR:-/tmp/mxl-install/lib}"
WEB_PORT="${MV_TEST_WEB_PORT:-18130}"

if [[ ! -x "$BIN" || ! -x "$WRITER" || ! -x "$SAMPLE" ]]; then
  echo "missing binaries (pass mxl-multiviewer mxl-mv-writer mxl-mv-sample)" >&2
  exit 1
fi

WORK="$(mktemp -d /dev/shm/mv-keyed-XXXXXX)"
PIDS=()
cleanup() {
  if [[ -n "${MV_PID:-}" ]]; then kill "$MV_PID" 2>/dev/null || true; wait "$MV_PID" 2>/dev/null || true; fi
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  rm -rf "$WORK"
}
trap cleanup EXIT

DOMAIN_ID="dddddddd-dddd-4ddd-8ddd-dddddddddddd"
BASE="d0000001-0000-4000-8000-000000000001"
KEY0="d0000002-0000-4000-8000-000000000002"
KEYFULL="d0000003-0000-4000-8000-000000000003"
KEYMID="d0000004-0000-4000-8000-000000000004"

mkdir -p "$WORK/src" "$WORK/config"
echo "{\"id\":\"$DOMAIN_ID\",\"label\":\"src\"}" > "$WORK/src/domain_def.json"
echo '{"urn:x-mxl:option:history_duration/v1.0":200000000}' > "$WORK/src/options.json"
python3 - "$WORK/config/routes.json" "$DOMAIN_ID" "$BASE" "$KEY0" "$KEYFULL" "$KEYMID" <<'PY'
import json, sys
path, domain, *flows = sys.argv[1:]
routes = [{"input": i + 1, "video": True, "enable": True, "domain_id": domain, "flow_id": f, "sender_id": ""} for i, f in enumerate(flows)]
json.dump({"routes": routes}, open(path, "w"))
PY

export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$WRITER" --domain "$WORK/src" --flow "$BASE" --width 192 --height 108 --rate 50 --y 800 >"$WORK/w1.log" 2>&1 &
PIDS+=($!)
"$WRITER" --domain "$WORK/src" --flow "$KEY0" --width 192 --height 108 --rate 50 --y 200 --key 0 >"$WORK/w2.log" 2>&1 &
PIDS+=($!)
"$WRITER" --domain "$WORK/src" --flow "$KEYFULL" --width 192 --height 108 --rate 50 --y 200 --key 1023 >"$WORK/w3.log" 2>&1 &
PIDS+=($!)
"$WRITER" --domain "$WORK/src" --flow "$KEYMID" --width 192 --height 108 --rate 50 --y 200 --key 512 >"$WORK/w4.log" 2>&1 &
PIDS+=($!)

MXL_DOMAIN_SCAN_PATH="$WORK" \
MV_OUTPUT_DOMAIN_DIR="$WORK/mv" \
MV_STATE_DIR="$WORK/config" \
MXL_CLEANUP_ON_EXIT=true \
MV_BACKEND="${MV_TEST_BACKEND:-cpu}" \
MV_MAX_INPUTS=4 \
MV_OUTPUT_FORMAT=192x108p50 \
MV_AUDIO_CHANNELS=0 \
NMOS_ENABLE=false \
WEB_PORT="$WEB_PORT" \
LOG_LEVEL=info \
"$BIN" >"$WORK/mv.log" 2>&1 &
MV_PID=$!

api() { curl -sf "http://127.0.0.1:${WEB_PORT}$1"; }
alive() {
  if ! kill -0 "$MV_PID" 2>/dev/null; then
    wait "$MV_PID" && code=0 || code=$?
    echo "multiviewer exited ($code)" >&2
    tail -40 "$WORK/mv.log" >&2
    exit 1
  fi
}
for _ in $(seq 1 50); do
  alive
  api /livez >/dev/null 2>&1 && break
  sleep 0.2
done

# Plain tiles (no caption, bars, format label or alarm display) with fill, so the picture covers
# each tile. The base tile is 144×108; the keyed tiles are 48×54 along the top, z above it.
tile() { echo "{\"id\":\"$1\",\"input\":$2,\"z\":$3,\"scale\":\"fill\",\"umd\":false,\"format_label\":false,\"audio_bars\":false,\"alarm_border\":false,\"alarm_labels\":false,\"rect\":{\"x\":$4,\"y\":0,\"w\":$5,\"h\":$6}}"; }
LAYOUT="{\"version\":1,\"name\":\"keyed\",\"background\":\"#000000\",\"tiles\":[$(tile base 1 0 0 0.75 1),$(tile k0 2 1 0 0.25 0.5),$(tile kfull 3 1 0.25 0.25 0.5),$(tile kmid 4 1 0.5 0.25 0.5),$(tile kbg 4 1 0.75 0.25 0.5)]}"
curl -sf -X PUT -H 'Content-Type: application/json' -d "$LAYOUT" "http://127.0.0.1:${WEB_PORT}/api/v1/layouts/keyed" >/dev/null
curl -sf -X POST "http://127.0.0.1:${WEB_PORT}/api/v1/layouts/keyed/activate" >/dev/null

FLOW="$(api /api/v1/outputs | python3 -c 'import json,sys; print(json.load(sys.stdin)["outputs"][0]["video_flow_id"])')"
sample() { "$SAMPLE" --domain "$WORK/mv" --flow "$FLOW" --x "$1" --y "$2" --width 192; }
near() { (( $1 >= $2 - 2 && $1 <= $2 + 2 )); }

# key 0: the base picture; key 1023: the fill; key 512: (200×512 + 800×511) / 1023 = 500 over
# the picture, (200×512 + 64×511) / 1023 = 132 over the background; below the keyed tiles: base.
ok=0
for _ in $(seq 1 50); do
  alive
  sleep 0.2
  K0="$(sample 24 27)"; KFULL="$(sample 72 27)"; KMID="$(sample 120 27)"; KBG="$(sample 168 27)"; UNDER="$(sample 72 81)"
  if near "$K0" 800 && near "$KFULL" 200 && near "$KMID" 500 && near "$KBG" 132 && near "$UNDER" 800; then
    ok=1
    break
  fi
done
echo "key0=$K0 key1023=$KFULL key512=$KMID key512-on-background=$KBG base=$UNDER"
if [[ "$ok" != 1 ]]; then
  echo "keyed tiles are not laid over what is under them" >&2
  api /api/v1/inputs >&2 || true
  exit 1
fi

alive
kill -TERM "$MV_PID"
wait "$MV_PID" && CODE=0 || CODE=$?
MV_PID=""
[[ "$CODE" == "143" ]] || { echo "expected SIGTERM exit 143, got $CODE" >&2; exit 1; }
echo "keyed integration OK"
