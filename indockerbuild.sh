#!/usr/bin/env sh
# Build firmware (PlatformIO / Arduino + TFT_eSPI) inside Docker — no host PlatformIO install.

set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
. "${ROOT}/scripts/pio_image.env"

docker run --rm \
  -v "${ROOT}:/project" \
  -e PLATFORMIO_CORE_DIR=/project/.cache/platformio \
  -w /project/firmware \
  "${PYTHON_IMAGE}" \
  bash -lc 'pip install -q platformio && export PATH="/root/.local/bin:$PATH" && pio run'
