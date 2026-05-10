#!/usr/bin/env sh
# Flash PlatformIO firmware from Docker. Plug CYD via USB; use BOOT/RST if needed.

set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
. "${ROOT}/scripts/pio_image.env"

PORT="${ESPPORT:-/dev/serial-cyd}"
if ! test -e "${PORT}"; then
  echo "Serial port not found: ${PORT}" >&2
  echo "Set ESPPORT=/dev/ttyUSB0 (example) or create udev symlink serial-cyd." >&2
  exit 1
fi

if [ -t 0 ] && [ -t 1 ]; then
  DOCKER_TTY=-it
else
  DOCKER_TTY=-i
fi

docker run --rm ${DOCKER_TTY} \
  --device="${PORT}" \
  -v "${ROOT}:/project" \
  -e PLATFORMIO_CORE_DIR=/project/.cache/platformio \
  -w /project/firmware \
  "${PYTHON_IMAGE}" \
  bash -lc "pip install -q platformio && export PATH=\"/root/.local/bin:\$PATH\" && pio run -t upload --upload-port '${PORT}'"
