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
TSL_PORT=18910

mkdir -p "$WORK/src" "$WORK/config"
cat > "$WORK/config/routes.json" <<EOF
{"routes":[{"input":1,"video":true,"enable":true,"domain_id":"$DOMAIN_ID","flow_id":"$FLOW_ID","sender_id":""}]}
EOF
cat > "$WORK/src/domain_def.json" <<EOF
{"id":"$DOMAIN_ID","label":"src","unused":true}
EOF
echo '{"urn:x-mxl:option:history_duration/v1.0":200000000}' > "$WORK/src/options.json"

python3 "$ROOT/tests/integration/fake_registry.py" "$REG_PORT" >"$WORK/registry.log" 2>&1 &
REG_PID=$!

export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
MXL_DOMAIN_SCAN_PATH="$WORK" \
MV_OUTPUT_DOMAIN_DIR="$WORK/mv" \
MV_STATE_DIR="$WORK/config" \
MXL_CLEANUP_ON_EXIT=true \
MV_BACKEND=cpu \
MV_MAX_INPUTS=4 \
MV_OUTPUT_FORMAT=192x108p50 \
MV_ACTIVE_LAYOUT=2x2 \
MV_AUDIO_CHANNELS=0 \
TSL_ENABLE=true \
TSL_UDP_PORT="$TSL_PORT" \
TSL_TCP_PORT="$((TSL_PORT + 1))" \
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

restored=0
for _ in $(seq 1 40); do
  STATE="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/inputs" | python3 -c 'import json,sys; print(json.load(sys.stdin)["inputs"][0]["video"]["state"])')"
  if [[ "$STATE" == "waiting" || "$STATE" == "running" ]]; then
    restored=1
    break
  fi
  sleep 0.25
done
if [[ "$restored" != 1 ]]; then
  echo "persisted route was not restored, got ${STATE:-none}" >&2
  exit 1
fi

ready=0
for _ in $(seq 1 50); do
  CODE="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${WEB_PORT}/readyz")"
  if [[ "$CODE" == "200" ]]; then
    ready=1
    break
  fi
  sleep 0.2
done
if [[ "$ready" != 1 ]]; then
  echo "readyz did not become 200" >&2
  curl -s "http://127.0.0.1:${WEB_PORT}/readyz" >&2 || true
  exit 1
fi

INFO="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/info")"
RX="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["receivers"][0]["video"])' "$INFO")"

# The route restored from routes.json is also the receiver's IS-05 active state.
ACTIVE="$(curl -sf "http://127.0.0.1:${NMOS_PORT}/x-nmos/connection/v1.2/single/receivers/${RX}/active")"
if ! python3 -c 'import json,sys; a=json.loads(sys.argv[1]); p=a["transport_params"][0]; sys.exit(0 if a["master_enable"] and p["mxl_flow_id"]==sys.argv[2] and p["mxl_domain_id"]==sys.argv[3] else 1)' "$ACTIVE" "$FLOW_ID" "$DOMAIN_ID"; then
  echo "restored route is not the IS-05 active state: $ACTIVE" >&2
  exit 1
fi

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

# BCP-007-03 schema: the output domain_def.json needs id, label, description and tags.
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if all(k in d for k in ("id","label","description")) and isinstance(d.get("tags"), dict) else 1)' "$WORK/mv/domain_def.json" ||
  { echo "output domain_def.json is not BCP-007-03: $(cat "$WORK/mv/domain_def.json")" >&2; exit 1; }

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

# Centre of the top-left 2×2 tile. The format caption sits on the top edge and the UMD on the bottom.
PIXEL="$("$SAMPLE" --domain "$WORK/mv" --flow "$FLOW_OUT" --x 48 --y 28 --width 192)"
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

# TSL 5.0 as the platform's tally calculator sends it: UDP, screen 0, display 0 = input 1,
# UTF-16LE label. LH red, RH green, text amber. Tile a follows the layout's tally_text (on),
# tile b turns it off. 192x108: 16 px caption band from y=92, 8 px lamps at y=96.
curl -sf -X PUT -H 'Content-Type: application/json' \
  -d '{"version":1,"name":"tally","tally_text":true,"tiles":[{"id":"a","input":1,"umd_source":"tsl","rect":{"x":0,"y":0,"w":0.5,"h":1}},{"id":"b","input":1,"tally_text":false,"rect":{"x":0.5,"y":0,"w":0.5,"h":1}}]}' \
  "http://127.0.0.1:${WEB_PORT}/api/v1/layouts/tally" >/dev/null
curl -sf -X POST "http://127.0.0.1:${WEB_PORT}/api/v1/layouts/tally/activate" >/dev/null
send_tsl() {
  python3 - "$TSL_PORT" <<'PY'
import socket, struct, sys
text = "Kamera Zürich".encode("utf-16-le")
display = struct.pack("<HHH", 0, 2 | (3 << 2) | (1 << 4), len(text)) + text
body = struct.pack("<BBH", 0, 0x01, 0) + display
socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(struct.pack("<H", len(body)) + body, ("127.0.0.1", int(sys.argv[1])))
PY
}
sample() { "$SAMPLE" --domain "$WORK/mv" --flow "$FLOW_OUT" --x "$1" --y "$2" --width 192; }
near() { (( $1 >= $2 - 8 && $1 <= $2 + 8 )); }
# 10-bit Y of the tally colours: red 367, green 485, amber 625.
tally_ok=0
for _ in $(seq 1 25); do
  send_tsl
  sleep 0.2
  A_LH="$(sample 7 99)"; A_RH="$(sample 87 99)"; A_BG="$(sample 48 93)"; B_LH="$(sample 103 99)"; B_BG="$(sample 144 93)"
  if near "$A_LH" 367 && near "$A_RH" 485 && near "$A_BG" 625 && near "$B_LH" 367 && (( B_BG < 150 )); then
    tally_ok=1
    break
  fi
done
echo "tally lh=$A_LH rh=$A_RH text_bg=$A_BG b_lh=$B_LH b_bg=$B_BG"
if [[ "$tally_ok" != 1 ]]; then
  echo "TSL tally is not on the output" >&2
  exit 1
fi
INPUTS="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/inputs")"
python3 -c 'import json,sys; i=json.loads(sys.argv[1])["inputs"][0]; assert (i["tsl_lh"], i["tsl_rh"], i["tsl_text_tally"], i["tally"]) == (1, 2, 3, 3), i; assert i["tsl_text"] == "Kamera Zürich", i["tsl_text"]' "$INPUTS"

# Image tile and audio bars beside the picture. Left tile: input 1 (Y 200) with bars right-beside,
# so its 16:9 picture is 58 px wide (x 0..57, y 37..69) and x 58..95 is the black bar strip.
# Right tile: an uploaded white PNG, fill (Y 940).
python3 - "$WORK/white.png" <<'PY'
import struct, sys, zlib
w = h = 16
raw = b"".join(b"\x00" + b"\xff\xff\xff" * w for _ in range(h))
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
PY
curl -sf -X PUT -H 'Content-Type: image/png' --data-binary @"$WORK/white.png" "http://127.0.0.1:${WEB_PORT}/api/v1/images/white.png" >/dev/null
curl -sf -X PUT -H 'Content-Type: application/json' \
  -d '{"version":1,"name":"pic","tiles":[{"id":"v","input":1,"umd":false,"format_label":false,"audio_bars":true,"audio_bar_position":"right-beside","rect":{"x":0,"y":0,"w":0.5,"h":1}},{"id":"i","content":"image","image_file":"white.png","scale":"fill","rect":{"x":0.5,"y":0,"w":0.5,"h":1}}]}' \
  "http://127.0.0.1:${WEB_PORT}/api/v1/layouts/pic" >/dev/null
curl -sf -X POST "http://127.0.0.1:${WEB_PORT}/api/v1/layouts/pic/activate" >/dev/null
picture_ok=0
for _ in $(seq 1 25); do
  sleep 0.2
  PICTURE="$(sample 28 50)"; STRIP="$(sample 66 50)"; IMAGE="$(sample 144 50)"
  if near "$PICTURE" 200 && (( STRIP < 80 )) && near "$IMAGE" 940; then
    picture_ok=1
    break
  fi
done
echo "picture=$PICTURE strip=$STRIP image=$IMAGE"
if [[ "$picture_ok" != 1 ]]; then
  echo "image tile or bars beside the picture are not on the output" >&2
  exit 1
fi

curl -sf "http://127.0.0.1:${WEB_PORT}/metrics" | grep -q "mxl_multiviewer_output_frames_total"
NODE="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["node_id"])' "$INFO")"
EXPORT="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/config/export")"
python3 -c 'import json,sys; doc=json.loads(sys.argv[1]); assert doc["version"]==1 and doc["secrets"] is False and "layouts" in doc and doc["routes"]["routes"][0]["flow_id"]' "$EXPORT"
kill -TERM "$MV_PID"
set +e
wait "$MV_PID"
CODE=$?
set -e
MV_PID=
if [[ "$CODE" != "143" ]]; then
  echo "expected SIGTERM exit 143, got $CODE" >&2
  exit 1
fi
if [[ -e "$WORK/mv" ]]; then
  echo "output domain was not removed" >&2
  exit 1
fi
QUERY="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$((REG_PORT + 1))/x-nmos/query/v1.3/nodes/${NODE}")"
if [[ "$QUERY" != "404" ]]; then
  echo "node still registered ($QUERY)" >&2
  exit 1
fi
echo "integration ok"
