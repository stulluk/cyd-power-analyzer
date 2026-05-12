#!/usr/bin/env sh
# Build minimal INA226 serial-only probe (no LVGL / WiFi script).

set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
. "${ROOT}/scripts/pio_image.env"

docker run --rm \
  -v "${ROOT}:/project" \
  -e PLATFORMIO_CORE_DIR=/project/.cache/platformio \
  -w /project/firmware/ina226_serial_probe \
  "${PYTHON_IMAGE}" \
  bash -lc 'pip install -q platformio && export PATH="/root/.local/bin:$PATH" && pio run'
