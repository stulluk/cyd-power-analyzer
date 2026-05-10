#!/usr/bin/env sh
# Wrapper: average STATS lines from CYD serial (requires pyserial).

set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${ESPPORT:-/dev/serial-cyd}"
exec python3 "${ROOT}/scripts/collect_cyd_stats.py" --port "${PORT}" "$@"
