#!/usr/bin/env sh
# Flash prebuilt ESP32 Marauder (CYD) from Fr4nkFletcher repo — no Arduino build.
# Default: dual-USB CYD, no GPS. Override MARAUDER_CACHE / MARAUDER_APP_BIN if needed.

set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
. "${ROOT}/scripts/idf_image.env"

PORT="${ESPPORT:-/dev/serial-cyd}"
CACHE="${MARAUDER_CACHE:-$ROOT/.cache/marauder-cyd}"

if ! test -e "${PORT}"; then
  echo "Serial not found: ${PORT}" >&2
  exit 1
fi

if ! test -d "${CACHE}/.git"; then
  mkdir -p "$(dirname "${CACHE}")"
  echo "Cloning Marauder CYD repo into ${CACHE} ..."
  git clone --depth 1 \
    https://github.com/Fr4nkFletcher/ESP32-Marauder-Cheap-Yellow-Display.git \
    "${CACHE}"
fi

APP_HOST="${MARAUDER_APP_BIN:-${CACHE}/bins/esp32_marauder_v1_4_2_20250413_cyd2usb_nogps.bin}"
if ! test -f "${APP_HOST}"; then
  echo "App bin missing: ${APP_HOST}" >&2
  exit 1
fi

APP_BASE="$(basename "${APP_HOST}")"

if [ -t 0 ] && [ -t 1 ]; then
  DOCKER_TTY=-it
else
  DOCKER_TTY=-i
fi

echo "Flashing Marauder: ${APP_BASE} via ${PORT}"

docker run --rm ${DOCKER_TTY} \
  --device="${PORT}" \
  -v "${CACHE}/bins:/bins:ro" \
  -e "SERIAL=${PORT}" \
  -e "APP=/bins/${APP_BASE}" \
  "${ESP_IDF_IMAGE}" \
  bash -lc 'python -m esptool --chip esp32 -p "${SERIAL}" -b 460800 \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_freq 40m --flash_size 4MB \
    0x1000 /bins/esp32_marauder.ino.bootloader.bin \
    0x8000 /bins/esp32_marauder.ino.partitions.bin \
    0x10000 "${APP}"'
