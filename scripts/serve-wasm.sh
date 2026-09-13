#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST_DIR="${DIST_DIR:-$PROJECT_ROOT/dist-wasm}"
BUILD_DIR="$PROJECT_ROOT/build-wasm"

PORT="${1:-8080}"

if [ ! -f "$DIST_DIR/index.html" ] && [ ! -f "$DIST_DIR/tilemap-maker.html" ]; then
    if [ -f "$BUILD_DIR/dist/index.html" ]; then
        DIST_DIR="$BUILD_DIR/dist"
    else
        echo "WASM distribution not found in $DIST_DIR. Running build-wasm.sh first..."
        "$SCRIPT_DIR/build-wasm.sh"
    fi
fi

echo "=== Serving Tilemap Maker WASM ==="
echo "Directory: $DIST_DIR"
echo "URL: http://localhost:$PORT/"
echo "Press Ctrl+C to stop the server."
echo ""

python3 -m http.server "$PORT" -d "$DIST_DIR"
