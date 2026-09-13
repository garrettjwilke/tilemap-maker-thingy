#!/usr/bin/env bash
set -e

# Resolve script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== Building Tilemap Maker for WebAssembly (WASM) ==="

# Check for emsdk in common locations if emcc is not already available
if ! command -v emcc &> /dev/null; then
    if [ -n "$EMSDK" ] && [ -f "$EMSDK/emsdk_env.sh" ]; then
        echo "Activating EMSDK from $EMSDK..."
        source "$EMSDK/emsdk_env.sh"
    elif [ -f "$HOME/build/emsdk/emsdk_env.sh" ]; then
        echo "Activating EMSDK from $HOME/build/emsdk..."
        source "$HOME/build/emsdk/emsdk_env.sh"
    elif [ -f "/opt/emsdk/emsdk_env.sh" ]; then
        echo "Activating EMSDK from /opt/emsdk..."
        source "/opt/emsdk/emsdk_env.sh"
    else
        echo "Error: emcc not found and emsdk_env.sh could not be located."
        echo "Please set EMSDK environment variable or install emsdk at ~/build/emsdk."
        exit 1
    fi
fi

echo "Using Emscripten: $(emcc -v 2>&1 | head -n 1)"

BUILD_DIR="$PROJECT_ROOT/build-wasm"

# Configure with CMake
echo "Configuring CMake in $BUILD_DIR..."
emcmake cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    "$@"

# Build
echo "Compiling..."
cmake --build "$BUILD_DIR" --parallel

echo ""
echo "=== Build Complete! ==="
echo "Output files in $BUILD_DIR:"
ls -lh "$BUILD_DIR"/tilemap-maker.* 2>/dev/null || true
echo ""
echo "To run locally, execute:"
echo "  ./scripts/serve-wasm.sh"
