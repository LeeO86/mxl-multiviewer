#!/usr/bin/env bash
# Run AMWA NMOS Testing suites IS-04-01, IS-05-01, and IS-05-02 against this node.
# Not part of default CI: the harness image is large and the suites are long.
# The multiviewer must already be listening (NMOS_PORT, default 3262).
set -euo pipefail

HOST="${NMOS_HOST:-127.0.0.1}"
PORT="${NMOS_PORT:-3262}"
IMAGE="${NMOS_TEST_IMAGE:-amwa/nmos-testing:latest}"

echo "Suites IS-04-01 IS-05-01 IS-05-02 against ${HOST}:${PORT} using ${IMAGE}"
docker run --rm --network host \
  -e "TEST_TARGET_HOST=${HOST}" \
  -e "TEST_TARGET_PORT=${PORT}" \
  "${IMAGE}" \
  python3 nmos-test.py --suite IS-04-01 --host "${HOST}" --port "${PORT}"
docker run --rm --network host "${IMAGE}" \
  python3 nmos-test.py --suite IS-05-01 --host "${HOST}" --port "${PORT}"
docker run --rm --network host "${IMAGE}" \
  python3 nmos-test.py --suite IS-05-02 --host "${HOST}" --port "${PORT}"
