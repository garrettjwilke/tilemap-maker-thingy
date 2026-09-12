#include "tileset.h"
#include "gentileset.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace tmm {
namespace {

constexpr uint8_t kAtmE  = 0x01;
constexpr uint8_t kAtmSE = 0x02;
constexpr uint8_t kAtmS  = 0x04;
constexpr uint8_t kAtmSW = 0x08;
constexpr uint8_t kAtmW  = 0x10;
constexpr uint8_t kAtmNW = 0x20;
constexpr uint8_t kAtmN  = 0x40;
constexpr uint8_t kAtmNE = 0x80;

struct AtmTileDef {
    uint8_t mask;
    int8_t x;
    int8_t y;
};

// 47 Godot MATCH_CORNERS_AND_SIDES autotile peering definitions
static const AtmTileDef kAtmTiles[] = {
    { 0x00,  0, 3 }, { 0x01,  1, 3 }, { 0x04,  0, 0 }, { 0x05,  1, 0 }, { 0x07,  8, 0 },
    { 0x10,  3, 3 }, { 0x11,  2, 3 }, { 0x14,  3, 0 }, { 0x15,  2, 0 }, { 0x17,  5, 0 },
    { 0x1C, 11, 0 }, { 0x1D,  6, 0 }, { 0x1F, 10, 0 }, { 0x40,  0, 2 }, { 0x41,  1, 2 },
    { 0x44,  0, 1 }, { 0x45,  1, 1 }, { 0x47,  4, 1 }, { 0x50,  3, 2 }, { 0x51,  2, 2 },
    { 0x54,  3, 1 }, { 0x55,  2, 1 }, { 0x57,  7, 3 }, { 0x5C,  7, 1 }, { 0x5D,  4, 3 },
    { 0x5F,  9, 0 }, { 0x70, 11, 3 }, { 0x71,  6, 3 }, { 0x74,  7, 2 }, { 0x75,  4, 0 },
    { 0x77, 10, 2 }, { 0x7C, 11, 2 }, { 0x7D, 11, 1 }, { 0x7F,  6, 1 }, { 0xC1,  8, 3 },
    { 0xC5,  4, 2 }, { 0xC7,  8, 1 }, { 0xD1,  5, 3 }, { 0xD5,  7, 0 }, { 0xD7,  8, 2 },
    { 0xDD,  9, 1 }, { 0xDF,  5, 1 }, { 0xF1,  9, 3 }, { 0xF5, 10, 3 }, { 0xF7,  5, 2 },
    { 0xFD,  6, 2 }, { 0xFF,  9, 2 }
};

std::string dirname_of(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

std::string basename_of(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const std::string file = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot = file.find_last_of('.');
    return (dot == std::string::npos) ? file : file.substr(0, dot);
}

bool read_text_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

uint8_t Tileset::canonicalize_mask(uint8_t m) {
    if (!(m & kAtmE) || !(m & kAtmS)) m &= static_cast<uint8_t>(~kAtmSE);
    if (!(m & kAtmW) || !(m & kAtmS)) m &= static_cast<uint8_t>(~kAtmSW);
    if (!(m & kAtmW) || !(m & kAtmN)) m &= static_cast<uint8_t>(~kAtmNW);
    if (!(m & kAtmE) || !(m & kAtmN)) m &= static_cast<uint8_t>(~kAtmNE);
    return m;
}

Cell Tileset::autotile_cell_for_mask(uint8_t mask) {
    const uint8_t can = canonicalize_mask(mask);
    for (const auto& t : kAtmTiles) {
        if (t.mask == can) {
            return {t.x, t.y};
        }
    }
    return {kDefaultCenterCol, kDefaultCenterRow};
}

std::vector<VariantBinding> Tileset::variants_for_root(int root_x, int root_y) const {
    std::vector<VariantBinding> list;
    for (const auto& v : variants) {
        if (v.root_x == root_x && v.root_y == root_y) {
            list.push_back(v);
        }
    }
    return list;
}

Cell Tileset::resolve_variant(int root_x, int root_y, float roll01) const {
    const auto vars = variants_for_root(root_x, root_y);
    if (vars.empty()) {
        return {root_x, root_y};
    }
    // Base tile weight is 1.0f
    float total_weight = 1.0f;
    for (const auto& v : vars) {
        total_weight += std::max(v.probability, 0.01f);
    }

    float target = roll01 * total_weight;
    if (target < 1.0f) {
        return {root_x, root_y};
    }
    target -= 1.0f;
    for (const auto& v : vars) {
        const float w = std::max(v.probability, 0.01f);
        if (target < w) {
            return {v.x, v.y};
        }
        target -= w;
    }
    return {root_x, root_y};
}

uint8_t Tileset::get_pixel(int col, int row, int px, int py) const {
    if (!in_bounds(col, row) || px < 0 || py < 0 || px >= tile_size || py >= tile_size) {
        return 0;
    }
    const int img_w = image_width();
    const size_t idx = static_cast<size_t>((row * tile_size + py) * img_w + (col * tile_size + px));
    if (idx < pixels.size()) {
        return pixels[idx];
    }
    return 0;
}

std::vector<uint8_t> Tileset::get_tile_pixels(int col, int row) const {
    std::vector<uint8_t> buf(static_cast<size_t>(tile_size * tile_size), 0);
    if (!in_bounds(col, row)) return buf;
    const int img_w = image_width();
    for (int y = 0; y < tile_size; ++y) {
        for (int x = 0; x < tile_size; ++x) {
            const size_t idx = static_cast<size_t>((row * tile_size + y) * img_w + (col * tile_size + x));
            buf[static_cast<size_t>(y * tile_size + x)] = (idx < pixels.size()) ? pixels[idx] : 0;
        }
    }
    return buf;
}

bool Tileset::load_from_file(const std::string& path) {
    error.clear();
    if (path.empty()) {
        error = "Path is empty";
        return false;
    }
    // Check if .terrain extension
    if (path.size() >= 8 && path.substr(path.size() - 8) == ".terrain") {
        return load_terrain_file(path);
    }
    // Check if JSON file with terrain metadata
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") {
        std::string text;
        if (read_text_file(path, text)) {
            if (text.find("\"variants\"") != std::string::npos || text.find("\"tileset\"") != std::string::npos) {
                return load_terrain_file(path);
            }
        }
    }
    return load_png_file(path);
}

bool Tileset::load_png_raw(const std::string& path) {
    error.clear();
    Image im{};
    if (!load_png(path.c_str(), &im)) {
        error = "Could not read PNG: " + path;
        return false;
    }
    if (im.w == 0 || im.h == 0 || !im.px) {
        image_free(&im);
        error = "Invalid PNG dimensions";
        return false;
    }

    // Determine tile size: 4 rows standard.
    if (im.h % kBaseRows != 0) {
        image_free(&im);
        error = "PNG height must be a multiple of 4";
        return false;
    }
    const int detected_ts = static_cast<int>(im.h / kBaseRows);
    if (detected_ts != 8 && detected_ts != 16) {
        image_free(&im);
        error = "Tiles must be 8x8 or 16x16 (height must be 32 or 64)";
        return false;
    }
    if (im.w % static_cast<unsigned>(detected_ts) != 0) {
        image_free(&im);
        error = "PNG width must be a multiple of tile size";
        return false;
    }
    const int w_cols = static_cast<int>(im.w / static_cast<unsigned>(detected_ts));
    if (w_cols < kBaseCols) {
        image_free(&im);
        error = "PNG must be at least 12 tiles wide";
        return false;
    }

    tile_size = detected_ts;
    cols = w_cols;
    rows = kBaseRows;
    png_path = path;

    palette.clear();
    if (im.indexed && im.palettesize > 0) {
        palette.resize(im.palettesize);
        for (unsigned i = 0; i < im.palettesize; ++i) {
            palette[i] = Rgb{im.palette[i][0], im.palette[i][1], im.palette[i][2]};
        }
    } else {
        palette.push_back(Rgb{0, 0, 0});
        palette.push_back(Rgb{255, 255, 255});
    }

    const size_t total_px = static_cast<size_t>(im.w * im.h);
    pixels.resize(total_px);
    for (size_t i = 0; i < total_px; ++i) {
        pixels[i] = im.px[i];
    }
    image_free(&im);
    return true;
}

void Tileset::parse_terrain_text(const std::string& text, const std::string& path) {
    terrain_path = path;
    variants.clear();

    size_t pos = 0;
    while (true) {
        auto x_pos = text.find("\"x\"", pos);
        if (x_pos == std::string::npos) break;
        auto brace_end = text.find('}', x_pos);
        if (brace_end == std::string::npos) brace_end = text.size();
        std::string block = text.substr(x_pos, brace_end - x_pos);

        auto find_int = [&](const std::string& key, int def) -> int {
            auto kp = block.find("\"" + key + "\"");
            if (kp == std::string::npos) return def;
            auto cp = block.find(':', kp);
            if (cp == std::string::npos) return def;
            return std::atoi(block.c_str() + cp + 1);
        };
        auto find_float = [&](const std::string& key, float def) -> float {
            auto kp = block.find("\"" + key + "\"");
            if (kp == std::string::npos) return def;
            auto cp = block.find(':', kp);
            if (cp == std::string::npos) return def;
            return std::strtof(block.c_str() + cp + 1, nullptr);
        };

        VariantBinding b;
        b.x = find_int("x", -1);
        b.y = find_int("y", -1);
        b.root_x = find_int("root_x", -1);
        b.root_y = find_int("root_y", -1);
        b.probability = find_float("probability", 0.3f);
        if (b.x >= 0 && b.y >= 0 && b.root_x >= 0 && b.root_y >= 0) {
            variants.push_back(b);
        }
        pos = brace_end + 1;
    }
}

bool Tileset::load_png_file(const std::string& path) {
    if (!load_png_raw(path)) {
        return false;
    }

    // Look for sibling .terrain, .terrain.json, or _terrain.json file
    const std::string dir = dirname_of(path);
    const std::string stem = basename_of(path);
    const std::vector<std::string> candidates = {
        (dir.empty() ? "" : (dir + "/")) + stem + ".terrain",
        (dir.empty() ? "" : (dir + "/")) + stem + ".terrain.json",
        (dir.empty() ? "" : (dir + "/")) + stem + "_terrain.json",
        (dir.empty() ? "" : (dir + "/")) + stem + ".json"
    };

    bool found = false;
    for (const auto& cand : candidates) {
        std::string terrain_text;
        if (read_text_file(cand, terrain_text)) {
            if (terrain_text.find("\"variants\"") != std::string::npos ||
                terrain_text.find("\"tileset\"") != std::string::npos) {
                parse_terrain_text(terrain_text, cand);
                found = true;
                break;
            }
        }
    }

    if (!found) {
        variants.clear();
        terrain_path.clear();
    }
    return true;
}

bool Tileset::load_terrain_file(const std::string& path) {
    error.clear();
    std::string text;
    if (!read_text_file(path, text)) {
        error = "Could not read terrain file: " + path;
        return false;
    }
    if (text.find("\"version\"") == std::string::npos && text.find("\"variants\"") == std::string::npos) {
        error = "Terrain file is not valid JSON (missing 'version' or 'variants')";
        return false;
    }

    // Extract tileset PNG reference
    std::string ts_name;
    auto ts_pos = text.find("\"tileset\"");
    if (ts_pos != std::string::npos) {
        auto colon = text.find(':', ts_pos);
        if (colon != std::string::npos) {
            auto q1 = text.find('"', colon + 1);
            if (q1 != std::string::npos) {
                auto q2 = text.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    ts_name = text.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }

    const std::string dir = dirname_of(path);
    std::string stem = basename_of(path);
    if (stem.size() >= 8 && stem.substr(stem.size() - 8) == ".terrain") {
        stem = stem.substr(0, stem.size() - 8);
    } else if (stem.size() >= 8 && stem.substr(stem.size() - 8) == "_terrain") {
        stem = stem.substr(0, stem.size() - 8);
    }

    std::vector<std::string> png_candidates;
    if (!ts_name.empty()) {
        if (!dir.empty()) png_candidates.push_back(dir + "/" + ts_name);
        png_candidates.push_back(ts_name);
    }
    if (!dir.empty()) png_candidates.push_back(dir + "/" + stem + ".png");
    png_candidates.push_back(stem + ".png");

    std::string resolved_png;
    for (const auto& cand : png_candidates) {
        std::ifstream test(cand);
        if (test.good()) {
            resolved_png = cand;
            break;
        }
    }

    if (resolved_png.empty()) {
        error = "Could not find matching PNG image for terrain file: " + (ts_name.empty() ? (stem + ".png") : ts_name);
        return false;
    }

    if (!load_png_raw(resolved_png)) {
        return false;
    }

    parse_terrain_text(text, path);
    return true;
}

} // namespace tmm
