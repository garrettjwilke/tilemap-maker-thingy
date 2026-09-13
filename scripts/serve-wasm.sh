#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-wasm"

PORT="${1:-8080}"

if [ ! -f "$BUILD_DIR/tilemap-maker.html" ]; then
    echo "WASM build not found in $BUILD_DIR. Running build-wasm.sh first..."
    "$SCRIPT_DIR/build-wasm.sh"
fi

echo "=== Serving Tilemap Maker WASM ==="
echo "URL: http://localhost:$PORT/tilemap-maker.html"
echo "Press Ctrl+C to stop the server."
echo ""

python3 -m http.server "$PORT" -d "$BUILD_DIR"
