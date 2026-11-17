#!/bin/sh
# Build and run the menu preview on a PC; writes PNG screenshots to $1 (default: ./menu-preview).
# Needs gcc and python3-pil.
set -e
cd "$(dirname "$0")/.."
OUT="${1:-menu-preview}"
mkdir -p "$OUT"
gcc -O1 -Wall -Wextra -Wno-unused-parameter -o "$OUT/menu_preview" \
	tools/menu_preview.c src/menu.c src/ui.c src/font_data.c src/config.c ../lib/src/base64.c \
	-I../lib/include -Isrc -lm
"$OUT/menu_preview" "$OUT"
python3 - "$OUT" <<'EOF'
import sys, glob
from PIL import Image
for p in sorted(glob.glob(sys.argv[1] + "/*.ppm")):
    Image.open(p).save(p[:-4] + ".png")
EOF
rm -f "$OUT"/*.ppm "$OUT/menu_preview"
