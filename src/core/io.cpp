#include "io.h"
#include "gentileset.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace tmm {
namespace {

std::string dirname_of(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

std::string filename_of(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

} // namespace

bool write_text_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

bool read_text_file(const std::string& path, std::string& text) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

bool write_binary_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return f.good();
}

bool read_binary_file(const std::string& path, std::vector<uint8_t>& bytes) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    return f.good();
}

std::string export_composite_png(const TilemapDoc& doc, const std::string& path) {
    if (!doc.tileset.is_valid()) {
        return "No valid tileset loaded to export map image";
    }
    const int out_w = doc.width * doc.tile_size;
    const int out_h = doc.height * doc.tile_size;
    if (out_w <= 0 || out_h <= 0) {
        return "Invalid map dimensions";
    }

    Image like{};
    like.bpp = 1;
    like.indexed = 1;
    like.palettesize = static_cast<unsigned>(std::min(static_cast<int>(doc.tileset.palette.size()), 256));
    std::memset(like.palette, 0, sizeof(like.palette));
    for (unsigned i = 0; i < like.palettesize; ++i) {
        like.palette[i][0] = doc.tileset.palette[i].r;
        like.palette[i][1] = doc.tileset.palette[i].g;
        like.palette[i][2] = doc.tileset.palette[i].b;
        like.palette[i][3] = 255;
    }
    Image im{};
    if (!image_alloc(&im, static_cast<unsigned>(out_w), static_cast<unsigned>(out_h), &like)) {
        return "Failed to allocate composite image memory";
    }
    std::memset(im.px, 0, static_cast<size_t>(out_w * out_h));

    // Blit cells
    const int ts = doc.tile_size;
    for (int cy = 0; cy < doc.height; ++cy) {
        for (int cx = 0; cx < doc.width; ++cx) {
            const MapCell& cell = doc.get_cell(cx, cy);
            int ax = cell.atlas_x;
            int ay = cell.atlas_y;
            if (cell.mode == TileMode::Empty || ax < 0 || ay < 0) {
                ax = 10;
                ay = 1;
            }
            if (doc.tileset.in_bounds(ax, ay)) {
                for (int py = 0; py < ts; ++py) {
                    for (int px = 0; px < ts; ++px) {
                        const uint8_t idx = doc.tileset.get_pixel(ax, ay, px, py);
                        const int dest_x = cx * ts + px;
                        const int dest_y = cy * ts + py;
                        im.px[dest_y * out_w + dest_x] = idx;
                    }
                }
            }
        }
    }

    const int ok = save_png(path.c_str(), &im);
    image_free(&im);
    return ok ? std::string() : ("Could not save composite PNG to " + path);
}

std::string export_mde_collision_json(const TilemapDoc& doc, const std::string& path) {
    const CollisionGrid grid = doc.build_collision_grid();
    const std::string rle = compress_mde_collisions(grid.data);
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"width\": " << grid.width << ",\n";
    ss << "  \"height\": " << grid.height << ",\n";
    ss << "  \"collisions\": \"" << rle << "\",\n";
    ss << "}\n";
    if (!write_text_file(path, ss.str())) {
        return "Could not write collision JSON to " + path;
    }
    return "";
}

std::string export_collision_bin(const TilemapDoc& doc, const std::string& path) {
    const CollisionGrid grid = doc.build_collision_grid();
    if (grid.count_types_used() > 1) {
        return "Cannot export BIN: map uses more than 1 collision type";
    }
    std::vector<uint8_t> bin_bytes(grid.data.size(), 0);
    for (size_t i = 0; i < grid.data.size(); ++i) {
        bin_bytes[i] = (grid.data[i] != 0) ? 1 : 0;
    }
    if (!write_binary_file(path, bin_bytes)) {
        return "Could not write collision BIN to " + path;
    }
    return "";
}

std::string save_map_json(const TilemapDoc& doc, const std::string& path) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "\t\"version\": 4,\n";
    ss << "\t\"name\": \"" << doc.name << "\",\n";
    ss << "\t\"tile_size\": " << doc.tile_size << ",\n";
    ss << "\t\"width\": " << doc.width << ",\n";
    ss << "\t\"height\": " << doc.height << ",\n";
    ss << "\t\"buffer\": " << doc.buffer << ",\n";
    ss << "\t\"origin_x\": " << doc.origin_x << ",\n";
    ss << "\t\"origin_y\": " << doc.origin_y << ",\n";
    if (!doc.tileset.png_path.empty()) {
        ss << "\t\"tileset\": \"" << doc.tileset.png_path << "\",\n";
    }
    ss << "\t\"collision_types\": [\n";
    for (size_t i = 0; i < doc.collision_types.size(); ++i) {
        const auto& ct = doc.collision_types[i];
        ss << "\t\t{\"id\": " << static_cast<int>(ct.id)
           << ", \"name\": \"" << ct.name << "\""
           << ", \"r\": " << static_cast<int>(ct.color.r)
           << ", \"g\": " << static_cast<int>(ct.color.g)
           << ", \"b\": " << static_cast<int>(ct.color.b)
           << "}" << (i + 1 < doc.collision_types.size() ? ",\n" : "\n");
    }
    ss << "\t],\n";
    if (!doc.tileset.tile_collisions.empty()) {
        ss << "\t\"tileset_collisions\": [";
        for (size_t i = 0; i < doc.tileset.tile_collisions.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << static_cast<int>(doc.tileset.tile_collisions[i]);
        }
        ss << "],\n";
    }
    ss << "\t\"cells\": [\n";
    bool first = true;
    for (int y = -doc.buffer; y < doc.height + doc.buffer; ++y) {
        for (int x = -doc.buffer; x < doc.width + doc.buffer; ++x) {
            const MapCell& c = doc.get_cell(x, y);
            if (c.is_empty()) continue;
            if (!first) ss << ",\n";
            first = false;
            ss << "\t\t{\n";
            ss << "\t\t\t\"x\": " << x << ",\n";
            ss << "\t\t\t\"y\": " << y << ",\n";
            ss << "\t\t\t\"source\": 0,\n";
            ss << "\t\t\t\"atlas_x\": " << c.atlas_x << ",\n";
            ss << "\t\t\t\"atlas_y\": " << c.atlas_y << ",\n";
            ss << "\t\t\t\"mode\": \"" << (c.mode == TileMode::Terrain ? "terrain" : "stamp") << "\",\n";
            ss << "\t\t\t\"roll\": " << c.roll << ",\n";
            ss << "\t\t\t\"alt\": 0\n";
            ss << "\t\t}";
        }
    }
    ss << "\n\t]\n}\n";
    if (!write_text_file(path, ss.str())) {
        return "Could not write map JSON to " + path;
    }
    return "";
}

std::string load_map_json(TilemapDoc& doc, const std::string& path) {
    std::string text;
    if (!read_text_file(path, text)) {
        return "Could not read map file: " + path;
    }
    if (text.find("\"version\"") == std::string::npos || text.find("\"cells\"") == std::string::npos) {
        return "Invalid map JSON format";
    }

    auto find_str = [&](const std::string& key) -> std::string {
        auto kp = text.find("\"" + key + "\"");
        if (kp == std::string::npos) return "";
        auto cp = text.find(':', kp);
        if (cp == std::string::npos) return "";
        auto q1 = text.find('"', cp + 1);
        if (q1 == std::string::npos) return "";
        auto q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) return "";
        return text.substr(q1 + 1, q2 - q1 - 1);
    };

    auto find_int = [&](const std::string& key, int def) -> int {
        auto kp = text.find("\"" + key + "\"");
        if (kp == std::string::npos) return def;
        auto cp = text.find(':', kp);
        if (cp == std::string::npos) return def;
        return std::atoi(text.c_str() + cp + 1);
    };

    const std::string name = find_str("name");
    const int tile_size = find_int("tile_size", 16);
    const int width = find_int("width", 30);
    const int height = find_int("height", 20);
    const int buffer = find_int("buffer", 1);
    const int origin_x = find_int("origin_x", 0);
    const int origin_y = find_int("origin_y", 0);
    const std::string tileset_field = find_str("tileset");

    doc.reset(width, height, tile_size);
    doc.buffer = buffer;
    doc.name = name.empty() ? "untitled" : name;
    doc.origin_x = origin_x;
    doc.origin_y = origin_y;

    // Resolve tileset path
    if (!tileset_field.empty()) {
        std::string resolved = tileset_field;
        if (!file_exists(resolved)) {
            const std::string map_dir = dirname_of(path);
            const std::string beside = (map_dir.empty() ? "" : (map_dir + "/")) + filename_of(tileset_field);
            if (file_exists(beside)) {
                resolved = beside;
            } else {
                const std::string rel = (map_dir.empty() ? "" : (map_dir + "/")) + tileset_field;
                if (file_exists(rel)) {
                    resolved = rel;
                }
            }
        }
        if (file_exists(resolved)) {
            doc.tileset.load_from_file(resolved);
        }
    }

    // Parse collision types if present
    size_t col_types_pos = text.find("\"collision_types\"");
    if (col_types_pos != std::string::npos) {
        size_t arr_end = text.find(']', col_types_pos);
        if (arr_end != std::string::npos) {
            std::vector<CollisionType> parsed_types;
            size_t pos = col_types_pos;
            while (pos < arr_end) {
                auto b_start = text.find('{', pos);
                if (b_start == std::string::npos || b_start >= arr_end) break;
                auto b_end = text.find('}', b_start);
                if (b_end == std::string::npos || b_end > arr_end) break;
                std::string block = text.substr(b_start, b_end - b_start + 1);

                auto find_block_int = [&](const std::string& key, int def) -> int {
                    auto kp = block.find("\"" + key + "\"");
                    if (kp == std::string::npos) return def;
                    auto cp = block.find(':', kp);
                    if (cp == std::string::npos) return def;
                    return std::atoi(block.c_str() + cp + 1);
                };
                auto find_block_str = [&](const std::string& key) -> std::string {
                    auto kp = block.find("\"" + key + "\"");
                    if (kp == std::string::npos) return "";
                    auto cp = block.find(':', kp);
                    if (cp == std::string::npos) return "";
                    auto q1 = block.find('"', cp + 1);
                    if (q1 == std::string::npos) return "";
                    auto q2 = block.find('"', q1 + 1);
                    if (q2 == std::string::npos) return "";
                    return block.substr(q1 + 1, q2 - q1 - 1);
                };

                CollisionType ct;
                ct.id = static_cast<uint8_t>(find_block_int("id", 1));
                ct.name = find_block_str("name");
                if (ct.name.empty()) ct.name = "Type " + std::to_string(ct.id);
                ct.color.r = static_cast<uint8_t>(find_block_int("r", 235));
                ct.color.g = static_cast<uint8_t>(find_block_int("g", 60));
                ct.color.b = static_cast<uint8_t>(find_block_int("b", 50));
                parsed_types.push_back(ct);
                pos = b_end + 1;
            }
            if (!parsed_types.empty()) {
                doc.collision_types = std::move(parsed_types);
            }
        }
    }

    // Parse tileset collisions if present
    size_t tc_pos = text.find("\"tileset_collisions\"");
    if (tc_pos != std::string::npos) {
        auto bracket_open = text.find('[', tc_pos);
        auto bracket_close = text.find(']', bracket_open);
        if (bracket_open != std::string::npos && bracket_close != std::string::npos) {
            std::vector<uint8_t> cols_arr;
            size_t p = bracket_open + 1;
            while (p < bracket_close) {
                while (p < bracket_close && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n' || text[p] == ',')) {
                    p++;
                }
                if (p < bracket_close && std::isdigit(static_cast<unsigned char>(text[p]))) {
                    cols_arr.push_back(static_cast<uint8_t>(std::atoi(text.c_str() + p)));
                    while (p < bracket_close && std::isdigit(static_cast<unsigned char>(text[p]))) {
                        p++;
                    }
                } else {
                    p++;
                }
            }
            if (!cols_arr.empty()) {
                doc.tileset.tile_collisions = std::move(cols_arr);
                if (doc.tileset.in_bounds(10, 1)) {
                    doc.tileset.tile_collisions[static_cast<size_t>(1 * doc.tileset.cols + 10)] = 0;
                }
            }
        }
    }

    // Parse cells
    size_t cells_arr = text.find("\"cells\"");
    if (cells_arr != std::string::npos) {
        size_t pos = cells_arr;
        while (true) {
            auto brace_start = text.find('{', pos);
            if (brace_start == std::string::npos) break;
            auto brace_end = text.find('}', brace_start);
            if (brace_end == std::string::npos) break;
            std::string block = text.substr(brace_start, brace_end - brace_start + 1);

            auto cell_int = [&](const std::string& key, int def) -> int {
                auto kp = block.find("\"" + key + "\"");
                if (kp == std::string::npos) return def;
                auto cp = block.find(':', kp);
                if (cp == std::string::npos) return def;
                return std::atoi(block.c_str() + cp + 1);
            };

            auto cell_str = [&](const std::string& key) -> std::string {
                auto kp = block.find("\"" + key + "\"");
                if (kp == std::string::npos) return "";
                auto cp = block.find(':', kp);
                if (cp == std::string::npos) return "";
                auto q1 = block.find('"', cp + 1);
                if (q1 == std::string::npos) return "";
                auto q2 = block.find('"', q1 + 1);
                if (q2 == std::string::npos) return "";
                return block.substr(q1 + 1, q2 - q1 - 1);
            };

            auto cell_float = [&](const std::string& key, float def) -> float {
                auto kp = block.find("\"" + key + "\"");
                if (kp == std::string::npos) return def;
                auto cp = block.find(':', kp);
                if (cp == std::string::npos) return def;
                return std::strtof(block.c_str() + cp + 1, nullptr);
            };

            const int cx = cell_int("x", -999999);
            const int cy = cell_int("y", -999999);
            const int ax = cell_int("atlas_x", -1);
            const int ay = cell_int("atlas_y", -1);

            if (doc.in_bounds(cx, cy) && ax >= 0 && ay >= 0) {
                MapCell mc;
                const std::string mode_str = cell_str("mode");
                if (mode_str == "stamp") {
                    mc.mode = TileMode::Stamp;
                } else if (mode_str == "terrain") {
                    mc.mode = TileMode::Terrain;
                } else {
                    if (ay < 4 && ax < doc.tileset.cols) {
                        mc.mode = TileMode::Terrain;
                    } else {
                        mc.mode = TileMode::Stamp;
                    }
                }
                const float roll_val = cell_float("roll", -1.0f);
                if (roll_val >= 0.0f && roll_val <= 1.0f) {
                    mc.roll = roll_val;
                } else {
                    mc.roll = std::fmod(std::abs(std::sin(static_cast<float>(cx * 127 + cy * 311))) * 43758.5453f, 1.0f);
                }
                mc.atlas_x = ax;
                mc.atlas_y = ay;
                doc.set_cell(cx, cy, mc);
            }
            pos = brace_end + 1;
        }
    }
    doc.solve_all_autotiles();
    doc.clear_dirty();
    return "";
}

} // namespace tmm
