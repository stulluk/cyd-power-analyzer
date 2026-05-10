#!/usr/bin/env sh
# Pull the ESP-IDF Docker image used by indockerbuild.sh (optional).

set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
. "${SCRIPT_DIR}/scripts/idf_image.env"

echo "Pulling Docker image: ${ESP_IDF_IMAGE}"
docker pull "${ESP_IDF_IMAGE}"
