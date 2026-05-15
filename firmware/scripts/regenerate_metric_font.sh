#!/usr/bin/env sh
# Regenerate src/fonts/cyd_metric_mono.c (LVGL, 42 px, 4 bpp, glyph subset).
# Default TTF: Ubuntu Mono Regular (UFL) — static file on google/fonts; pairs with web Google Font.
# Roboto Mono: use a static .ttf (variable fonts are poor for lv_font_conv), e.g. FONT_PATH=...RobotoMono-Regular.ttf
# Requires: Node (npx), curl, perl — e.g. docker run --rm -v "$(pwd)/..:/project" -w /project/firmware node:20-bookworm-slim bash -lc "apt-get update -qq && apt-get install -y -qq curl perl && ./scripts/regenerate_metric_font.sh"

set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR_DIR="${ROOT}/fonts/vendor"
UBUNTU_MONO="${VENDOR_DIR}/UbuntuMono-Regular.ttf"
OUT="${ROOT}/src/fonts/cyd_metric_mono.c"

mkdir -p "${VENDOR_DIR}"

if test -n "${FONT_PATH:-}"; then
  FONT="${FONT_PATH}"
else
  FONT="${UBUNTU_MONO}"
  if ! test -r "${FONT}"; then
    echo "Downloading Ubuntu Mono Regular → ${UBUNTU_MONO}" >&2
    curl -sSL --fail -o "${UBUNTU_MONO}.part" \
      "https://raw.githubusercontent.com/google/fonts/main/ufl/ubuntumono/UbuntuMono-Regular.ttf"
    mv "${UBUNTU_MONO}.part" "${UBUNTU_MONO}"
  fi
fi

test -r "${FONT}" || {
  echo "Font not found: ${FONT} — set FONT_PATH=" >&2
  exit 1
}

cd "${ROOT}/src/fonts"
npx --yes lv_font_conv \
  --font "${FONT}" \
  --size 42 \
  --bpp 4 \
  --format lvgl \
  --no-compress \
  --lv-include lvgl.h \
  --lv-font-name cyd_metric_mono \
  --symbols 'Vbus I.APWError0123456789-+ ' \
  -o "$(basename "${OUT}")"

# Sparse-subset converts often emit too-small line_height; clip-safe minimum for 42 px design size.
perl -i -pe 's/\.line_height = \d+/.line_height = 48/' "$(basename "${OUT}")"

echo "Wrote ${OUT} from ${FONT}" >&2
