#pragma once

#include <string>

namespace tmm {

enum WebFileTarget {
    WebFileTarget_Auto = 0,
    WebFileTarget_MapJson = 1,
    WebFileTarget_Tileset = 2,
    WebFileTarget_TilesetProj = 3,
    WebFileTarget_Import12x4 = 4,
    WebFileTarget_Import5x3 = 5,
    WebFileTarget_Palette = 6,
    WebFileTarget_PendingTerrainPng = 7
};

#ifdef __EMSCRIPTEN__
void web_init_file_io();
void web_trigger_file_dialog(const char* accept, int target_type);
void web_download_file(const char* virtual_path, const char* download_name = nullptr);
void web_flush_downloads();
#endif

void handle_web_file_upload(const std::string& path, int target_type);
void handle_web_pinch(float factor, float center_x, float center_y);

} // namespace tmm
