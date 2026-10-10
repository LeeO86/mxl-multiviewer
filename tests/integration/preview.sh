#!/usr/bin/env bash
# Preview integration (§8.4, §8.5), CPU backend, no NMOS:
#  1. MV_PREVIEW_MODE unset: JPEG per head, no MediaMTX started, nothing published.
#  2. webrtc, own mode: the built-in MediaMTX (from PATH) gets the H.264 mosaic of two heads (RTSP DESCRIBE
#     and HLS), /preview.jpg is refused, the tile map, /widgets and the widget page's CSP; SIGTERM stops MediaMTX.
#  3. webrtc, shared mode: a separate MediaMTX gets <prefix>/heads, none is started here; that MediaMTX
#     goes away (error) and comes back (publishing).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-multiviewer}"
MEDIAMTX="${MEDIAMTX_BIN:-/tmp/mediamtx/mediamtx}"
LIB_DIR="${MXL_LIB_DIR:-/tmp/mxl-install/lib}"
if [[ ! -x "$BIN" || ! -x "$MEDIAMTX" ]]; then
  echo "missing binary: multiviewer=$BIN mediamtx=$MEDIAMTX" >&2
  exit 1
fi

WORK="$(mktemp -d /dev/shm/mv-preview-XXXXXX)"
MV_PID=""
SHARED_PID=""
cleanup() {
  local status=$?
  if [[ -n "$MV_PID" ]]; then kill "$MV_PID" 2>/dev/null || true; wait "$MV_PID" 2>/dev/null || true; fi
  if [[ -n "$SHARED_PID" ]]; then kill "$SHARED_PID" 2>/dev/null || true; wait "$SHARED_PID" 2>/dev/null || true; fi
  if [[ "$status" -ne 0 ]]; then
    for log in "$WORK"/*.log; do echo "== $log" >&2; tail -40 "$log" >&2 || true; done
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

WEB_PORT=18120
RTSP_PORT=18754
WHEP_PORT=18789
HLS_PORT=18788
ICE_PORT=18389
SHARED_RTSP_PORT=28554
SHARED_HLS_PORT=28888
export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Own mode starts `mediamtx` from PATH (the image has it in /usr/local/bin).
export PATH="$(dirname "$MEDIAMTX"):$PATH"

start_mv() { # <name> [KEY=value ...]
  local name=$1
  shift
  mkdir -p "$WORK/$name"
  env MXL_DOMAIN_SCAN_PATH="$WORK/$name" MV_OUTPUT_DOMAIN_DIR="$WORK/$name/mv" MV_STATE_DIR="$WORK/$name/config" \
    MV_BACKEND=cpu MV_MAX_INPUTS=4 MV_OUTPUTS=2 MV_OUTPUT_FORMAT=320x180p25 MV_OUT2_FORMAT=640x480p25 MV_AUDIO_CHANNELS=0 \
    NMOS_ENABLE=false TSL_ENABLE=false WEB_PORT="$WEB_PORT" \
    MEDIAMTX_RTSP_PORT="$RTSP_PORT" MEDIAMTX_WHEP_PORT="$WHEP_PORT" MEDIAMTX_HLS_PORT="$HLS_PORT" MEDIAMTX_ICE_UDP_PORT="$ICE_PORT" \
    WIDGET_FRAME_ANCESTORS="'self' http://designer.test" LOG_LEVEL=info "$@" \
    "$BIN" >"$WORK/$name.log" 2>&1 &
  MV_PID=$!
  for _ in $(seq 1 50); do
    if curl -sf "http://127.0.0.1:${WEB_PORT}/livez" >/dev/null; then return 0; fi
    if ! kill -0 "$MV_PID" 2>/dev/null; then echo "$name: multiviewer exited early" >&2; exit 1; fi
    sleep 0.2
  done
  echo "$name: /livez did not answer" >&2
  exit 1
}

stop_mv() {
  kill -TERM "$MV_PID"
  local code=0
  wait "$MV_PID" || code=$?
  MV_PID=""
  if [[ "$code" != 143 ]]; then echo "exit code $code, expected 143" >&2; exit 1; fi
}

statusz() { # <python expression on the preview object `p`>
  curl -sf "http://127.0.0.1:${WEB_PORT}/statusz" | python3 -c "import json,sys; p=json.load(sys.stdin)['preview']; print(($1))"
}

wait_state() { # <state> <seconds>
  for _ in $(seq 1 $(($2 * 4))); do
    if [[ "$(statusz "p.get('state')")" == "$1" ]]; then return 0; fi
    sleep 0.25
  done
  echo "preview state is not $1: $(curl -s "http://127.0.0.1:${WEB_PORT}/statusz" | python3 -c 'import json,sys; print(json.load(sys.stdin)["preview"])')" >&2
  exit 1
}

# RTSP DESCRIBE: 200 once the path has a publisher.
describe() { # <port> <path>
  python3 - "$1" "$2" <<'PY'
import socket, sys
port, path = int(sys.argv[1]), sys.argv[2]
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=2)
    s.sendall(f"DESCRIBE rtsp://127.0.0.1:{port}/{path} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n".encode())
    reply = s.recv(4096).decode(errors="replace")
except OSError:
    sys.exit(1)
sys.exit(0 if reply.startswith("RTSP/1.0 200") and "H264" in reply else 1)
PY
}

wait_describe() { # <port> <path> <seconds>
  for _ in $(seq 1 $(($3 * 2))); do
    if describe "$1" "$2"; then return 0; fi
    sleep 0.5
  done
  echo "rtsp://127.0.0.1:$1/$2 has no H.264 publisher" >&2
  exit 1
}

closed() { # <port>: nothing listens there
  ! python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=1)" "$1" 2>/dev/null
}

# ---- 1. JPEG (default): a JPEG per head, nothing published -------------------------------------
start_mv jpeg
for _ in $(seq 1 40); do
  if [[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${WEB_PORT}/preview.jpg?head=2")" == 200 ]]; then break; fi
  sleep 0.25
done
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${WEB_PORT}/preview.jpg?head=2")" == 200 ]] || { echo "jpeg: no /preview.jpg" >&2; exit 1; }
[[ "$(statusz "p['mode']")" == jpeg ]] || { echo "jpeg: statusz mode" >&2; exit 1; }
closed "$RTSP_PORT" || { echo "jpeg: a MediaMTX runs" >&2; exit 1; }
curl -sf "http://127.0.0.1:${WEB_PORT}/metrics" | grep -q '^mxl_multiviewer_preview_mode{mode="jpeg"} 1$' || { echo "jpeg: mode metric" >&2; exit 1; }
stop_mv
echo "jpeg mode: ok"

# ---- 2. WebRTC, own MediaMTX ----------------------------------------------------------------------
start_mv own MV_PREVIEW_MODE=webrtc
wait_state publishing 30
wait_describe "$RTSP_PORT" mxl-multiviewer/heads 10
[[ "$(statusz "p['publish'], p['mediamtx']['running'], p['encoder'] in ('nvenc', 'x264')")" == "('own', True, True)" ]] || {
  echo "own: statusz $(statusz p)" >&2
  exit 1
}
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${WEB_PORT}/preview.jpg")" == 404 ]] || { echo "own: /preview.jpg is served" >&2; exit 1; }
MAP="$(curl -sf "http://127.0.0.1:${WEB_PORT}/api/v1/preview/map")"
python3 - "$MAP" <<'PY'
import json, sys
m = json.loads(sys.argv[1])
assert m["mode"] == "webrtc" and m["width"] == 1920 and m["height"] == 1080, m
assert [(h["x"], h["y"], h["w"], h["h"]) for h in m["heads"]] == [(0, 0, 960, 540), (1080, 0, 720, 540)], m
PY
hls=0
for _ in $(seq 1 60); do
  # MediaMTX answers a first request with a cookie-check redirect.
  if curl -sfL "http://127.0.0.1:${HLS_PORT}/mxl-multiviewer/heads/index.m3u8" | grep -q '#EXTM3U'; then hls=1; break; fi
  sleep 0.5
done
[[ "$hls" == 1 ]] || { echo "own: no HLS playlist" >&2; exit 1; }
curl -sf -D "$WORK/widget.headers" -o /dev/null "http://127.0.0.1:${WEB_PORT}/widget/head?head=2&theme=transparent"
grep -qi "^content-security-policy: frame-ancestors 'self' http://designer.test" "$WORK/widget.headers" || { echo "own: widget CSP" >&2; exit 1; }
! grep -qi "^x-frame-options" "$WORK/widget.headers" || { echo "own: widget X-Frame-Options" >&2; exit 1; }
[[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${WEB_PORT}/widget/head?head=3")" == 400 ]] || { echo "own: widget head=3" >&2; exit 1; }
curl -sf -D "$WORK/widgets.headers" -H 'Origin: http://designer.test' "http://127.0.0.1:${WEB_PORT}/widgets" | python3 -c 'import json,sys; assert [w["id"] for w in json.load(sys.stdin)] == ["head", "tile-editor"]'
grep -qi "^access-control-allow-origin: http://designer.test" "$WORK/widgets.headers" || { echo "own: /widgets CORS" >&2; exit 1; }
METRICS="$(curl -sf "http://127.0.0.1:${WEB_PORT}/metrics")"
for line in 'preview_mode{mode="webrtc"} 1' 'preview_publish_mode{mode="own"} 1' 'preview_publish_state{state="publishing"} 1'; do
  grep -qF "mxl_multiviewer_${line}" <<<"$METRICS" || { echo "own: metric $line" >&2; exit 1; }
done
stop_mv
sleep 0.5
closed "$RTSP_PORT" || { echo "own: MediaMTX still runs after SIGTERM" >&2; exit 1; }
echo "own mode: ok"

# ---- 3. WebRTC, shared MediaMTX ------------------------------------------------------------------
cat >"$WORK/shared.yml" <<EOF
logLevel: warn
moq: false
api: false
rtsp: true
rtspAddress: 127.0.0.1:${SHARED_RTSP_PORT}
rtspTransports: [tcp]
rtmp: false
srt: false
webrtc: false
hls: true
hlsAddress: 127.0.0.1:${SHARED_HLS_PORT}
paths:
  all_others:
EOF
start_shared() {
  "$MEDIAMTX" "$WORK/shared.yml" >>"$WORK/shared-mediamtx.log" 2>&1 &
  SHARED_PID=$!
}
start_shared
start_mv shared MV_PREVIEW_MODE=webrtc PREVIEW_PUBLISH_URL="rtsp://127.0.0.1:${SHARED_RTSP_PORT}" PREVIEW_PATH_PREFIX=test-all/mv1
wait_state publishing 30
wait_describe "$SHARED_RTSP_PORT" test-all/mv1/heads 10
[[ "$(statusz "p['publish'], 'mediamtx' in p, p['publish_url']")" == "('shared', False, 'rtsp://127.0.0.1:${SHARED_RTSP_PORT}')" ]] || {
  echo "shared: statusz $(statusz p)" >&2
  exit 1
}
closed "$RTSP_PORT" || { echo "shared: an own MediaMTX runs" >&2; exit 1; }
kill "$SHARED_PID"
wait "$SHARED_PID" 2>/dev/null || true
SHARED_PID=""
wait_state error 15
start_shared
wait_state publishing 30
wait_describe "$SHARED_RTSP_PORT" test-all/mv1/heads 10
stop_mv
echo "shared mode: ok"
