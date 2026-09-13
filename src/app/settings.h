#pragma once

#include "core/types.h"

#include <string>
#include <utility>
#include <vector>

namespace tmm {

struct Settings {
    bool dark = true;
    float scale = 1.0f;
    float sidebar_w = 380.0f;
    bool grid_lines = true;
    bool collision_overlay = false;
    float zoom = 2.0f;
    int brush_size = 1;
    int paint_mode = 0; // 0 = Terrain, 1 = Stamp
    int sidebar_page = 0; // 0 = Tileset, 1 = MapProperties, 2 = Export
    bool export_terrain = true;
    Rgb grid_color{100, 100, 100};
    int window_x = 0;
    int window_y = 0;
    int window_w = 1280;
    int window_h = 800;
    bool window_maximized = false;
    bool window_placed = false;
    std::string last_tileset_path;
    std::string last_export_dir;
    std::string last_map_path;
    std::vector<std::pair<std::string, std::string>> extra;
};

std::string config_dir();
std::string settings_path();
bool ensure_config_dir();

std::string format_settings(const Settings& s);
bool parse_settings_text(Settings& s, const std::string& text);
bool load_settings_file(Settings& s, const std::string& path);
bool save_settings_file(const Settings& s, const std::string& path);

} // namespace tmm
