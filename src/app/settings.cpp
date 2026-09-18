#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tmm {
namespace {

namespace fs = std::filesystem;

std::string home_dir() {
    const char* home = std::getenv("HOME");
    if (home && home[0]) {
        return home;
    }
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile && userprofile[0]) {
        return userprofile;
    }
#endif
    return {};
}

std::string trim(std::string s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

} // namespace

std::string config_dir() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0]) {
        return (fs::path(appdata) / "tilemap-maker-thingy").string();
    }
    const std::string home = home_dir();
    if (!home.empty()) {
        return (fs::path(home) / "AppData" / "Roaming" / "tilemap-maker-thingy").string();
    }
#elif defined(__APPLE__)
    const std::string home = home_dir();
    if (!home.empty()) {
        return (fs::path(home) / "Library" / "Application Support" / "tilemap-maker-thingy").string();
    }
#else // Linux / BSD / Unix
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        return (fs::path(xdg) / "tilemap-maker-thingy").string();
    }
    const std::string home = home_dir();
    if (!home.empty()) {
        return (fs::path(home) / ".config" / "tilemap-maker-thingy").string();
    }
#endif
    return "tilemap-maker-thingy";
}

std::string settings_path() {
    return (fs::path(config_dir()) / "settings.cfg").string();
}

bool ensure_config_dir() {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    return !ec;
}

std::string format_settings(const Settings& s) {
    std::ostringstream ss;
    ss << "# tilemap-maker-thingy settings\n";
    ss << "theme=" << (s.dark ? "dark" : "light") << "\n";
    ss << "scale=" << s.scale << "\n";
    ss << "sidebar_w=" << s.sidebar_w << "\n";
    ss << "grid_lines=" << (s.grid_lines ? "true" : "false") << "\n";
    ss << "collision_overlay=" << (s.collision_overlay ? "true" : "false") << "\n";
    ss << "zoom=" << s.zoom << "\n";
    ss << "brush_size=" << s.brush_size << "\n";
    ss << "paint_mode=" << (s.paint_mode == 2 ? "slope" : (s.paint_mode == 1 ? "stamp" : "terrain")) << "\n";
    ss << "sidebar_page=" << s.sidebar_page << "\n";
    ss << "export_map_proj=" << (s.export_map_proj ? "true" : "false") << "\n";
    ss << "export_col_json=" << (s.export_col_json ? "true" : "false") << "\n";
    ss << "export_col_bin=" << (s.export_col_bin ? "true" : "false") << "\n";
    ss << "export_tileset_png=" << (s.export_tileset_png ? "true" : "false") << "\n";
    ss << "export_tileset_proj=" << (s.export_tileset_proj ? "true" : "false") << "\n";
    ss << "export_zip=" << (s.export_zip ? "true" : "false") << "\n";
    ss << "window_x=" << s.window_x << "\n";
    ss << "window_y=" << s.window_y << "\n";
    ss << "window_w=" << s.window_w << "\n";
    ss << "window_h=" << s.window_h << "\n";
    ss << "window_maximized=" << (s.window_maximized ? "true" : "false") << "\n";
    ss << "window_placed=" << (s.window_placed ? "true" : "false") << "\n";
    if (!s.last_tileset_path.empty()) ss << "last_tileset=" << s.last_tileset_path << "\n";
    if (!s.last_export_dir.empty()) ss << "last_export_dir=" << s.last_export_dir << "\n";
    if (!s.last_map_path.empty()) ss << "last_map=" << s.last_map_path << "\n";
    for (const auto& [k, v] : s.extra) {
        ss << k << "=" << v << "\n";
    }
    return ss.str();
}

bool parse_settings_text(Settings& s, const std::string& text) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "theme") s.dark = (val != "light" && val != "false" && val != "0");
        else if (key == "scale") s.scale = std::clamp(std::strtof(val.c_str(), nullptr), 0.75f, 2.0f);
        else if (key == "sidebar_w") s.sidebar_w = std::clamp(std::strtof(val.c_str(), nullptr), 200.0f, 2000.0f);
        else if (key == "grid_lines") s.grid_lines = (val == "true" || val == "1");
        else if (key == "collision_overlay") s.collision_overlay = (val == "true" || val == "1");
        else if (key == "zoom") s.zoom = std::clamp(std::strtof(val.c_str(), nullptr), 0.25f, 16.0f);
        else if (key == "brush_size") s.brush_size = std::clamp(std::atoi(val.c_str()), 1, 4);
        else if (key == "paint_mode") s.paint_mode = (val == "slope" || val == "2") ? 2 : ((val == "stamp" || val == "1") ? 1 : 0);
        else if (key == "sidebar_page") s.sidebar_page = std::clamp(std::atoi(val.c_str()), 0, 2);
        else if (key == "export_map_proj") s.export_map_proj = (val == "true" || val == "1");
        else if (key == "export_col_json") s.export_col_json = (val == "true" || val == "1");
        else if (key == "export_col_bin") s.export_col_bin = (val == "true" || val == "1");
        else if (key == "export_tileset_png") s.export_tileset_png = (val == "true" || val == "1");
        else if (key == "export_tileset_proj") s.export_tileset_proj = (val == "true" || val == "1");
        else if (key == "export_zip") s.export_zip = (val == "true" || val == "1");
        else if (key == "window_x") s.window_x = std::atoi(val.c_str());
        else if (key == "window_y") s.window_y = std::atoi(val.c_str());
        else if (key == "window_w") s.window_w = std::max(std::atoi(val.c_str()), 640);
        else if (key == "window_h") s.window_h = std::max(std::atoi(val.c_str()), 480);
        else if (key == "window_maximized") s.window_maximized = (val == "true" || val == "1");
        else if (key == "window_placed") s.window_placed = (val == "true" || val == "1");
        else if (key == "last_tileset") s.last_tileset_path = val;
        else if (key == "last_export_dir") s.last_export_dir = val;
        else if (key == "last_map") s.last_map_path = val;
        else s.extra.emplace_back(key, val);
    }
    return true;
}

bool load_settings_file(Settings& s, const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_settings_text(s, ss.str());
}

bool save_settings_file(const Settings& s, const std::string& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    const std::string text = format_settings(s);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

} // namespace tmm
