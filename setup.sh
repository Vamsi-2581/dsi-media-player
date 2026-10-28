#!/bin/bash
# Fetch third-party headers that are not vendored, and generate the ROM icon.

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
LIBS_DIR="$ROOT/libs"

echo "=== DSi Media Player Setup ==="
echo ""

if [ ! -f "$LIBS_DIR/dr_flac.h" ]; then
    echo "Downloading dr_flac.h..."
    curl -fL -o "$LIBS_DIR/dr_flac.h" \
        "https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h"
    echo "  OK"
else
    echo "dr_flac.h already present"
fi

if [ ! -f "$ROOT/icon.bmp" ]; then
    echo "Generating icon.bmp..."
    python3 "$ROOT/tools/gen_icon.py" "$ROOT/icon.bmp"
else
    echo "icon.bmp already present"
fi

echo ""
echo "Setup complete. Build with: make"
echo ""
echo "Prerequisites:"
echo "  - devkitPro with devkitARM installed"
echo "  - libnds, libfat, maxmod (dkp-pacman -S nds-dev)"
echo "  - DEVKITPRO and DEVKITARM environment variables set"
