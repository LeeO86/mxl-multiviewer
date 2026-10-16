#!/usr/bin/env bash
# CPU integration (1.4.4): interlaced inputs. MXL stores an interlaced flow as fields (one field of
# height / 2 lines per grain, at twice the declared grain_rate); 1.4.3 read whole frames from them
# and crashed (SIGSEGV). Inputs: 1 a 1080i-style tff writer, 2 the same as bff, 3 an interlaced
# writer whose flow_def.json was rewritten to say progressive (a grain smaller than the definition
# says), 4 a progressive writer. The multiviewer must keep running, show 1 and 2 at full height,
# mark 3 unsupported, and keep 4 running.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/build/mxl-multiviewer}"
WRITER="${2:-$ROOT/build/mxl-mv-writer}"
SAMPLE="${3:-$ROOT/build/mxl-mv-sample}"
LIB_DIR="${MXL_LIB_DIR:-/tmp/mxl-install/lib}"
WEB_PORT="${MV_TEST_WEB_PORT:-18120}"

if [[ ! -x "$BIN" || ! -x "$WRITER" || ! -x "$SAMPLE" ]]; then
  echo "missing binaries (pass mxl-multiviewer mxl-mv-writer mxl-mv-sample)" >&2
  exit 1
fi

WORK="$(mktemp -d /dev/shm/mv-ilace-XXXXXX)"
PIDS=()
cleanup() {
  if [[ -n "${MV_PID:-}" ]]; then kill "$MV_PID" 2>/dev/null || true; wait "$MV_PID" 2>/dev/null || true; fi
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  rm -rf "$WORK"
}
trap cleanup EXIT

DOMAIN_ID="cccccccc-cccc-4ccc-8ccc-cccccccccccc"
TFF="c0000001-0000-4000-8000-000000000001"
BFF="c0000002-0000-4000-8000-000000000002"
LIE="c0000003-0000-4000-8000-000000000003"
PROG="c0000004-0000-4000-8000-000000000004"

mkdir -p "$WORK/src" "$WORK/config"
echo "{\"id\":\"$DOMAIN_ID\",\"label\":\"src\"}" > "$WORK/src/domain_def.json"
echo '{"urn:x-mxl:option:history_duration/v1.0":200000000}' > "$WORK/src/options.json"
python3 - "$WORK/config/routes.json" "$DOMAIN_ID" "$TFF" "$BFF" "$LIE" "$PROG" <<'PY'
import json, sys
path, domain, *flows = sys.argv[1:]
routes = [{"input": i + 1, "video": True, "enable": True, "domain_id": domain, "flow_id": f, "sender_id": ""} for i, f in enumerate(flows)]
json.dump({"routes": routes}, open(path, "w"))
PY

export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Frame lines 0-53 luma 800, 54-107 luma 200: each field holds half of each.
"$WRITER" --domain "$WORK/src" --flow "$TFF" --width 192 --height 108 --rate 25 --interlace tff --y 800 --y2 200 >"$WORK/w1.log" 2>&1 &
PIDS+=($!)
"$WRITER" --domain "$WORK/src" --flow "$BFF" --width 192 --height 108 --rate 25 --interlace bff --y 800 --y2 200 >"$WORK/w2.log" 2>&1 &
PIDS+=($!)
"$WRITER" --domain "$WORK/src" --flow "$LIE" --width 192 --height 108 --rate 25 --interlace tff --y 600 >"$WORK/w3.log" 2>&1 &
PIDS+=($!)
"$WRITER" --domain "$WORK/src" --flow "$PROG" --width 192 --height 108 --rate 50 --y 500 >"$WORK/w4.log" 2>&1 &
PIDS+=($!)

for _ in $(seq 1 50); do
  [[ -f "$WORK/src/$LIE.mxl-flow/flow_def.json" && -f "$WORK/src/$PROG.mxl-flow/flow_def.json" && -f "$WORK/src/$BFF.mxl-flow/flow_def.json" ]] && break
  sleep 0.1
done
# Input 3: the definition now says progressive, so a reader expects 108 lines per grain; the
# grains still hold 54 (one field). This is the 1.4.3 crash, the other way round.
python3 - "$WORK/src/$LIE.mxl-flow/flow_def.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
doc["interlace_mode"] = "progressive"
doc["grain_rate"] = {"numerator": 50, "denominator": 1}
json.dump(doc, open(sys.argv[1], "w"))
PY

MXL_DOMAIN_SCAN_PATH="$WORK" \
MV_OUTPUT_DOMAIN_DIR="$WORK/mv" \
MV_STATE_DIR="$WORK/config" \
MXL_CLEANUP_ON_EXIT=true \
MV_BACKEND="${MV_TEST_BACKEND:-cpu}" \
MV_MAX_INPUTS=4 \
MV_OUTPUT_FORMAT=192x108p50 \
MV_ACTIVE_LAYOUT=2x2 \
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
leg() { api /api/v1/inputs | python3 -c "import json,sys; v=json.load(sys.stdin)['inputs'][$1-1]['video']; print(v.get('$2'))"; }

for _ in $(seq 1 50); do
  alive
  api /livez >/dev/null 2>&1 && break
  sleep 0.2
done

want_state() {
  local input=$1 want=$2 state=""
  for _ in $(seq 1 50); do
    alive
    state="$(leg "$input" state 2>/dev/null || true)"
    [[ "$state" == "$want" ]] && return 0
    sleep 0.2
  done
  echo "input $input: expected $want, got ${state:-none}" >&2
  api /api/v1/inputs >&2 || true
  tail -20 "$WORK/mv.log" >&2
  exit 1
}
want_state 1 running
want_state 2 running
want_state 4 running
want_state 3 unsupported
[[ "$(leg 3 reason)" == "grain_too_small" ]] || { echo "input 3 reason $(leg 3 reason)" >&2; exit 1; }
grep -q '"event":"input_unsupported"' "$WORK/mv.log" || { echo "no input_unsupported log line" >&2; exit 1; }

for input in 1 2; do
  [[ "$(leg $input interlaced)" == "True" ]] || { echo "input $input not interlaced" >&2; exit 1; }
  [[ "$(leg $input height)" == "108" ]] || { echo "input $input height $(leg $input height)" >&2; exit 1; }
done

# Field rate: about 50 grains per second for the 25/1 interlaced flows.
g0="$(leg 1 grains)"
sleep 2
alive
g1="$(leg 1 grains)"
python3 -c 'import sys; d=int(sys.argv[2])-int(sys.argv[1]); print("fields in 2 s:", d); sys.exit(0 if 80 <= d <= 120 else 1)' "$g0" "$g1" ||
  { echo "input 1 is not read at the field rate" >&2; exit 1; }

OUT="$(api /api/v1/outputs)"
FLOW_OUT="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["outputs"][0]["video_flow_id"])' "$OUT")"
sample() { "$SAMPLE" --domain "$WORK/mv" --flow "$FLOW_OUT" --x "$1" --y "$2" --width 192; }
# Tiles 1 (left) and 2 (right) of the top row are 96×54 (the format caption covers lines 2-16, the
# UMD lines 38-53): the upper half of each picture (to line 27) is 800,
# the lower half 200, from whichever field is newest. Sampled several times (both fields).
for _ in 1 2 3 4 5 6; do
  for x in 48 144; do
    upper="$(sample "$x" 22)"
    lower="$(sample "$x" 32)"
    echo "x=$x upper=$upper lower=$lower"
    if [[ "$upper" != "800" || "$lower" != "200" ]]; then
      echo "interlaced tile at x=$x is not the full picture; luma by line:" >&2
      for y in $(seq 0 2 53); do echo "  y=$y $(sample "$x" "$y")" >&2; done
      exit 1
    fi
  done
  sleep 0.05
done
# Input 4 (progressive) still shows.
[[ "$(sample 144 81)" == "500" ]] || { echo "progressive tile 4: $(sample 144 81)" >&2; exit 1; }

# The format alarm on the unsupported input, and the counter.
for _ in $(seq 1 20); do
  FORMAT="$(api /api/v1/inputs | python3 -c 'import json,sys; i=json.load(sys.stdin)["inputs"][2]; print("format_mismatch" in [a for a,v in i.get("alarms",{}).items() if v] if isinstance(i.get("alarms"),dict) else i.get("alarms"))')"
  [[ "$FORMAT" == "True" ]] && break
  sleep 0.2
done
[[ "$FORMAT" == "True" ]] || { echo "no format alarm on input 3: $FORMAT" >&2; api /api/v1/inputs >&2; exit 1; }
api /metrics | grep -q 'mxl_multiviewer_input_unsupported_total{input="3",reason="grain_too_small"}' ||
  { echo "no input_unsupported_total metric" >&2; exit 1; }

alive
kill -TERM "$MV_PID"
wait "$MV_PID" && CODE=0 || CODE=$?
MV_PID=""
[[ "$CODE" == "143" ]] || { echo "expected SIGTERM exit 143, got $CODE" >&2; exit 1; }
echo "interlaced integration ok"
