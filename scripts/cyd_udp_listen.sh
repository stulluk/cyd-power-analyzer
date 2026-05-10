#!/usr/bin/env sh
# Listen for CYD UDP telemetry and save to a file (requires Python 3).

set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "${ROOT}/scripts/cyd_udp_listen.py" "$@"
