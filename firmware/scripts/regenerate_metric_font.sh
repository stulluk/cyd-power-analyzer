#!/usr/bin/env sh
# Regenerate cyd_metric_mono.c (DejaVu Sans Mono subset, 36px, 4 bpp) after installing Node.
# Requires: npx (Node.js), DejaVuSansMono.ttf at system path below.

set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FONT="${FONT_PATH:-/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf}"
OUT="${ROOT}/src/fonts/cyd_metric_mono.c"

test -r "${FONT}" || {
  echo "Font not found: ${FONT} — set FONT_PATH=" >&2
  exit 1
}

cd "${ROOT}/src/fonts"
npx --yes lv_font_conv \
  --font "${FONT}" \
  --size 36 \
  --bpp 4 \
  --format lvgl \
  --no-compress \
  --lv-include lvgl.h \
  --lv-font-name cyd_metric_mono \
  --symbols 'Vbus I.APWError0123456789-+ ' \
  -o "$(basename "${OUT}")"

# Sparse-subset converts often emit too-small line_height; clip-safe minimum for 36 px design size.
perl -i -pe 's/\.line_height = \d+/.line_height = 40/' "$(basename "${OUT}")"
