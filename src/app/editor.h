#pragma once

namespace tmm {

int run_editor();

#ifdef __EMSCRIPTEN__
void wasm_open_file(const char* filename);
void wasm_open_map(const char* filename);
void wasm_import_tileset(const char* filename);
void wasm_open_project(const char* filename);
void handle_web_pinch(float factor, float center_x, float center_y);
#endif

} // namespace tmm
