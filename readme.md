# tilemap-maker-thingy

A modern, lightweight 2D Tilemap Editor and embedded Tileset Maker built with C++17, Dear ImGui, and SDL3. Runs natively on macOS, Linux, and Windows, as well as in the browser via WebAssembly (WASM).

## Features

- **Integrated Tilemap Editor**: Multi-layer tile painting, brush tools, bucket fill, rectangular selection, copy/cut/paste, pan/zoom canvas, and JSON map import/export.
- **Embedded Tileset Maker**: Dedicated tool for creating 12x4 blob/Wang tilesets and 5x3 RPG Maker style autotile sets.
- **Switch Views**: Seamlessly switch between the Map Editor (**F1**) and Tileset Maker (**F2**).
- **Format Support**: Import/export JSON maps, `.tilesetproj` projects, 12x4 Wang tilesets, 5x3 tilesets, PNG textures, and `.palette` files.
- **Cross-Platform**: Native desktop builds for macOS, Linux, and Windows, plus WebAssembly browser support.

---

## WebAssembly (WASM) Build

The WebAssembly build runs the full application in any modern browser via WebGL and WebAssembly.

### Prerequisites

- Emscripten SDK (`emsdk`). If installed at `~/build/emsdk` or active in your `$PATH`, the build script will automatically detect and activate it.

### Build Instructions

To build the WASM bundle:

```bash
./scripts/build-wasm.sh
```

This compiles the C++ codebase with Emscripten into `build-wasm/` and outputs:
- `tilemap-maker.html` & `index.html`: Responsive dark-themed shell with WebGL canvas.
- `tilemap-maker.js`: Emscripten runtime and JavaScript glue.
- `tilemap-maker.wasm`: Compiled WebAssembly binary.

### Running Locally

To start a local development server and open the application in your default browser:

```bash
./scripts/serve-wasm.sh
# or specify a custom port:
./scripts/serve-wasm.sh 8080
```

### Browser File Features
- **Open / Import**: Uses HTML `<input type="file">` to load `.json`, `.png`, `.tilesetproj`, `.palette`, and autotile images directly into the editor.
- **Drag and Drop**: Drag and drop supported files onto the browser window to open them immediately.
- **Save / Export**: Triggers instantaneous browser file downloads for maps and exported tilesets.

---

## Native Desktop Build

### Prerequisites

- CMake 3.20 or newer
- C++17 compiler (Clang, GCC, or MSVC)
- On Linux: Development packages for X11, Wayland, OpenGL/GLES, GTK3, ALSA/PulseAudio, and D-Bus:
  ```bash
  sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev \
    libxcursor-dev libxfixes-dev libxi-dev libwayland-dev libxkbcommon-dev \
    libgl1-mesa-dev libgtk-3-dev libdbus-1-dev
  ```

### Build Instructions

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

Run test suite:
```bash
ctest --test-dir build -C Release --output-on-failure
```

Launch application:
```bash
./build/tilemap-maker
```

---

## Keyboard Shortcuts

| Shortcut | Action |
| --- | --- |
| **F1** | Switch to Map Editor |
| **F2** | Switch to Tileset Maker |
| **Ctrl+O** / **Cmd+O** | Open Map |
| **Ctrl+S** / **Cmd+S** | Save Map |
| **Ctrl+Z** / **Cmd+Z** | Undo |
| **Ctrl+Y** / **Cmd+Y** | Redo |
| **B** | Brush Tool |
| **E** | Eraser Tool |
| **G** | Bucket Fill |
| **S** | Rectangular Select |
| **Space + Drag** / **Middle Drag** | Pan Canvas |
| **Mouse Wheel** | Zoom In / Out |

---

## License

MIT License. See individual dependency directories (`extern/`) for their respective licenses (SDL3, Dear ImGui, nativefiledialog-extended, nlohmann-json, stb).
