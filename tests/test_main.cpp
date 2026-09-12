#include "core/collision.h"
#include "core/io.h"
#include "core/tilemap_doc.h"
#include "core/tileset.h"
#include "core/types.h"
#include "app/settings.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        ++g_fails;
    }
}

std::string temp_path(const char* name) {
    const char* dir = std::getenv("TMPDIR");
    if (!dir || !*dir) {
        dir = "/tmp";
    }
    return std::string(dir) + "/" + name;
}

void test_autotile_rules() {
    using namespace tmm;
    // 0x00: completely isolated tile -> (0, 3)
    expect(Tileset::autotile_cell_for_mask(0x00) == Cell{0, 3}, "mask 0x00 should be (0, 3)");
    // 0xFF: completely surrounded tile -> (9, 2)
    expect(Tileset::autotile_cell_for_mask(0xFF) == Cell{9, 2}, "mask 0xFF should be (9, 2)");
    // 0x11: W and E connected (horizontal platform) -> (2, 3)
    expect(Tileset::autotile_cell_for_mask(0x11) == Cell{2, 3}, "mask 0x11 should be (2, 3)");
    // 0x44: N and S connected (vertical pillar) -> (0, 1)
    expect(Tileset::autotile_cell_for_mask(0x44) == Cell{0, 1}, "mask 0x44 should be (0, 1)");

    // Test canonicalization: corner bit without adjacent sides is stripped
    // 0x02 = SE corner only. E(0x01) and S(0x04) are absent, so SE must be cleared
    expect(Tileset::canonicalize_mask(0x02) == 0x00, "SE corner without E and S should be cleared");
    // With E and S present: 0x01 | 0x04 | 0x02 = 0x07 -> SE stays
    expect(Tileset::canonicalize_mask(0x07) == 0x07, "SE corner with E and S should remain");
}

void test_collision() {
    using namespace tmm;
    // Test empty data
    expect(compress_mde_collisions({}).empty(), "empty data should give empty string");

    // Test 1 solid tile
    // val = 0x0F, count = 1 -> "0000000f!"
    expect(compress_mde_collisions({1}) == "0000000f!", "single solid tile RLE");

    // Test 1 empty tile
    // val = 0x00, count = 1 -> "00000000!"
    expect(compress_mde_collisions({0}) == "00000000!", "single empty tile RLE");

    // Test 5 solid tiles
    // val = 0x0F, count = 5 -> "0000000f5+"
    expect(compress_mde_collisions({1, 1, 1, 1, 1}) == "0000000f5+", "5 solid tiles RLE");

    // Test run of empty then solid
    // 3 empty, 2 solid -> "000000003+0000000f2+"
    expect(compress_mde_collisions({0, 0, 0, 1, 1}) == "000000003+0000000f2+", "mixed RLE");

    // Test roundtrip decompression
    std::vector<uint8_t> orig = {0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 1};
    std::string rle = compress_mde_collisions(orig);
    std::vector<uint8_t> decomp = decompress_mde_collisions(rle, static_cast<int>(orig.size()));
    expect(orig == decomp, "RLE decompress roundtrip failed");

    // Test collision grid from TilemapDoc
    TilemapDoc doc(4, 4, 16);
    // Placing 1 tile at (1, 1) in 16x16 tile_size -> covers 2x2 cells in 8x8 collision grid
    MapCell mc;
    mc.mode = TileMode::Stamp;
    mc.atlas_x = 9;
    mc.atlas_y = 2;
    doc.set_cell(1, 1, mc);

    CollisionGrid col = doc.build_collision_grid();
    expect(col.width == 8 && col.height == 8, "16px doc collision grid dims should be 8x8");
    expect(col.is_solid(2, 2) && col.is_solid(3, 2) && col.is_solid(2, 3) && col.is_solid(3, 3),
           "tile at (1, 1) should fill collision cells (2,2) to (3,3)");
    expect(!col.is_solid(0, 0) && !col.is_solid(1, 1) && !col.is_solid(4, 4),
           "unoccupied areas should be empty in collision");
}

void test_tilemap_editing() {
    using namespace tmm;
    TilemapDoc doc(10, 10, 16);

    // Test paint stamp
    doc.begin_stroke("Paint Stamp");
    doc.paint_cell(2, 3, TileMode::Stamp, 5, 2, 1);
    doc.end_stroke();

    expect(doc.get_cell(2, 3).mode == TileMode::Stamp, "paint cell mode");
    expect(doc.get_cell(2, 3).atlas_x == 5 && doc.get_cell(2, 3).atlas_y == 2, "paint cell atlas coords");

    // Test undo / redo
    expect(doc.can_undo(), "can undo after paint");
    doc.undo();
    expect(doc.get_cell(2, 3).is_empty(), "cell should be empty after undo");
    expect(doc.can_redo(), "can redo");
    doc.redo();
    expect(doc.get_cell(2, 3).atlas_x == 5, "cell restored after redo");

    // Test brush size 2x2
    doc.begin_stroke("Paint 2x2");
    doc.paint_cell(5, 5, TileMode::Stamp, 1, 1, 2);
    doc.end_stroke();
    expect(doc.get_cell(5, 5).atlas_x == 1 && doc.get_cell(6, 5).atlas_x == 1 &&
           doc.get_cell(5, 6).atlas_x == 1 && doc.get_cell(6, 6).atlas_x == 1,
           "brush 2x2 should paint 4 cells");

    // Test erase
    doc.begin_stroke("Erase 2x2");
    doc.erase_cell(5, 5, 2);
    doc.end_stroke();
    expect(doc.get_cell(5, 5).is_empty() && doc.get_cell(6, 6).is_empty(), "cells erased");

    // Test rect fill
    doc.fill_rect({1, 1, 3, 2}, TileMode::Stamp, 4, 3);
    for (int y = 1; y <= 2; ++y) {
        for (int x = 1; x <= 3; ++x) {
            expect(doc.get_cell(x, y).atlas_x == 4 && doc.get_cell(x, y).atlas_y == 3, "rect fill check");
        }
    }

    // Test copy & paste
    Clipboard clip = doc.copy_rect({1, 1, 3, 2});
    expect(clip.w == 3 && clip.h == 2, "clip dims");
    expect(clip.cells.size() == 6, "clip count");
    doc.paste_clipboard(6, 6, clip);
    expect(doc.get_cell(6, 6).atlas_x == 4 && doc.get_cell(8, 7).atlas_x == 4, "paste check");
}

void test_flood_fill() {
    using namespace tmm;
    TilemapDoc doc(6, 6, 16);
    // Draw boundary around (1,1) to (3,3)
    doc.fill_rect({0, 0, 5, 1}, TileMode::Stamp, 1, 1);
    doc.fill_rect({0, 4, 5, 1}, TileMode::Stamp, 1, 1);
    doc.fill_rect({0, 0, 1, 5}, TileMode::Stamp, 1, 1);
    doc.fill_rect({4, 0, 1, 5}, TileMode::Stamp, 1, 1);

    // Flood fill inside empty pocket at (2, 2)
    doc.flood_fill(2, 2, TileMode::Stamp, 7, 7);
    expect(doc.get_cell(2, 2).atlas_x == 7, "filled center");
    expect(doc.get_cell(1, 1).atlas_x == 7, "filled corner of pocket");
    expect(doc.get_cell(0, 0).atlas_x == 1, "boundary unchanged");
    expect(doc.get_cell(5, 5).is_empty(), "outside pocket unchanged");
}

void test_canvas_resize() {
    using namespace tmm;
    TilemapDoc doc(4, 4, 16);
    MapCell mc;
    mc.mode = TileMode::Stamp;
    mc.atlas_x = 3;
    mc.atlas_y = 2;
    doc.set_cell(1, 1, mc);

    // Resize with anchor top-left (-1, -1) -> expands to right and bottom
    doc.resize(6, 6, -1, -1);
    expect(doc.width == 6 && doc.height == 6, "resized dims");
    expect(doc.get_cell(1, 1).atlas_x == 3, "cell at (1,1) unchanged with top-left anchor");

    // Test loss check
    expect(!doc.would_lose_tiles(4, 4, -1, -1), "shrinking to 4x4 does not lose (1,1)");
    expect(doc.would_lose_tiles(1, 1, -1, -1), "shrinking to 1x1 loses (1,1)");
}

void test_io_and_settings() {
    using namespace tmm;
    TilemapDoc doc(8, 6, 16);
    doc.name = "test_level";
    MapCell mc;
    mc.mode = TileMode::Stamp;
    mc.atlas_x = 2;
    mc.atlas_y = 1;
    doc.set_cell(3, 4, mc);

    const std::string map_file = temp_path("test_level.json");
    const std::string col_file = temp_path("test_level_collisions.json");
    const std::string bin_file = temp_path("test_level_col.bin");

    expect(save_map_json(doc, map_file).empty(), "save map JSON");
    expect(export_mde_collision_json(doc, col_file).empty(), "export collision JSON");
    expect(export_collision_bin(doc, bin_file).empty(), "export collision BIN");

    TilemapDoc loaded;
    expect(load_map_json(loaded, map_file).empty(), "load map JSON");
    expect(loaded.name == "test_level", "loaded name");
    expect(loaded.width == 8 && loaded.height == 6, "loaded dims");
    expect(loaded.get_cell(3, 4).atlas_x == 2 && loaded.get_cell(3, 4).atlas_y == 1, "loaded cell content");
    expect(loaded.get_cell(0, 0).is_empty(), "loaded empty cell");

    std::remove(map_file.c_str());
    std::remove(col_file.c_str());
    std::remove(bin_file.c_str());

    // Test settings format, parse, and file roundtrip
    Settings s;
    s.dark = false; // light theme
    s.scale = 1.5f;
    s.sidebar_w = 425.0f;
    s.grid_lines = false;
    s.collision_overlay = true;
    s.zoom = 3.5f;
    s.brush_size = 3;
    s.paint_mode = 1; // Stamp
    s.window_x = 120;
    s.window_y = 150;
    s.window_w = 1400;
    s.window_h = 900;
    s.window_maximized = true;
    s.window_placed = true;
    s.last_tileset_path = "assets/tiles.png";
    s.last_export_dir = "/tmp/export_dir";
    s.last_map_path = "/tmp/map.json";
    s.extra.push_back({"custom_key", "custom_value"});

    const std::string s_text = format_settings(s);
    Settings s2;
    expect(parse_settings_text(s2, s_text), "parse settings text");
    expect(!s2.dark, "settings dark==false");
    expect(std::fabs(s2.scale - 1.5f) < 0.01f, "settings scale");
    expect(std::fabs(s2.sidebar_w - 425.0f) < 0.01f, "settings sidebar_w");
    expect(!s2.grid_lines, "settings grid_lines==false");
    expect(s2.collision_overlay, "settings collision_overlay==true");
    expect(std::fabs(s2.zoom - 3.5f) < 0.01f, "settings zoom");
    expect(s2.brush_size == 3, "settings brush_size");
    expect(s2.paint_mode == 1, "settings paint_mode stamp");
    expect(s2.window_x == 120 && s2.window_y == 150, "settings window_x and window_y");
    expect(s2.window_w == 1400 && s2.window_h == 900, "settings window_w and window_h");
    expect(s2.window_maximized, "settings window_maximized");
    expect(s2.window_placed, "settings window_placed");
    expect(s2.last_tileset_path == "assets/tiles.png", "settings last_tileset");
    expect(s2.last_export_dir == "/tmp/export_dir", "settings last_export_dir");
    expect(s2.last_map_path == "/tmp/map.json", "settings last_map");
    expect(!s2.extra.empty() && s2.extra[0].first == "custom_key" && s2.extra[0].second == "custom_value",
           "settings extra preserve");

    // Test settings file save & load roundtrip
    const std::string s_file = temp_path("test_settings.cfg");
    expect(save_settings_file(s, s_file), "save settings file");
    Settings s3;
    expect(load_settings_file(s3, s_file), "load settings file");
    expect(!s3.dark && s3.brush_size == 3 && s3.paint_mode == 1, "loaded settings file content matches");
    std::remove(s_file.c_str());

    // Test platform config directory resolution
    const std::string cdir = config_dir();
    expect(!cdir.empty(), "config_dir is not empty");
    const std::string spath = settings_path();
    expect(!spath.empty() && spath.find("settings.cfg") != std::string::npos, "settings_path ends with settings.cfg");
    expect(ensure_config_dir(), "ensure_config_dir succeeds");
}

void test_real_tilesets() {
    using namespace tmm;
    const std::string ts16_path = "/Users/homeless/build/mde/tools/md-tilemap-editor/purple-grass-tiles.png";
    const std::string ts8_path = "/Users/homeless/build/mde/tools/md-tilemap-editor/tinytiles-blue.png";

    // 16px tileset test
    Tileset ts16;
    expect(ts16.load_from_file(ts16_path), "load 16px tileset purple-grass-tiles");
    expect(ts16.tile_size == 16, "ts16 tile_size should be 16");
    expect(ts16.cols == 13 && ts16.rows == 4, "ts16 dims should be 13x4");

    TilemapDoc doc16(10, 8, 16);
    doc16.tileset = ts16;
    // Paint a 3x3 patch with autotiling
    doc16.fill_rect({2, 2, 3, 3}, TileMode::Terrain);
    // Verify center of patch is (9, 2)
    expect(doc16.get_cell(3, 3).atlas_x == 9 && doc16.get_cell(3, 3).atlas_y == 2,
           "center of 3x3 terrain should be autotile center (9, 2)");

    const std::string out_png16 = temp_path("test_out_16.png");
    expect(export_composite_png(doc16, out_png16).empty(), "export composite PNG 16px");
    std::remove(out_png16.c_str());

    // 8px tileset test
    Tileset ts8;
    expect(ts8.load_from_file(ts8_path), "load 8px tileset tinytiles-blue");
    expect(ts8.tile_size == 8, "ts8 tile_size should be 8");
    expect(ts8.cols == 13 && ts8.rows == 4, "ts8 dims should be 13x4");

    TilemapDoc doc8(12, 10, 8);
    doc8.tileset = ts8;
    doc8.paint_cell(4, 4, TileMode::Stamp, 5, 1, 1);
    expect(doc8.get_cell(4, 4).atlas_x == 5 && doc8.get_cell(4, 4).atlas_y == 1, "stamp placed on 8px map");

    const std::string out_png8 = temp_path("test_out_8.png");
    expect(export_composite_png(doc8, out_png8).empty(), "export composite PNG 8px");
    std::remove(out_png8.c_str());
}

void test_terrain_import() {
    using namespace tmm;
    const std::string ts8_path = "/Users/homeless/build/mde/tools/md-tilemap-editor/tinytiles-blue.png";
    const std::string tpath = temp_path("test_tileset.terrain");
    const std::string jpath = temp_path("test_tileset.json");

    // Write a .terrain file pointing to tinytiles-blue.png
    const std::string json_content =
        "{\n"
        "  \"version\": 1,\n"
        "  \"tile_size\": 8,\n"
        "  \"tileset\": \"" + ts8_path + "\",\n"
        "  \"variants\": [\n"
        "    { \"x\": 12, \"y\": 2, \"root_x\": 9, \"root_y\": 2, \"probability\": 0.45 },\n"
        "    { \"x\": 12, \"y\": 0, \"root_x\": 0, \"root_y\": 3, \"probability\": 0.25 }\n"
        "  ]\n"
        "}\n";

    {
        std::ofstream f(tpath);
        f << json_content;
    }
    {
        std::ofstream f(jpath);
        f << json_content;
    }

    // 1. Test loading via .terrain path
    Tileset ts_terrain;
    expect(ts_terrain.load_from_file(tpath), "load tileset directly from .terrain");
    expect(ts_terrain.tile_size == 8, ".terrain tile_size should be 8");
    expect(ts_terrain.cols == 13 && ts_terrain.rows == 4, ".terrain dims 13x4");
    expect(ts_terrain.variants.size() == 2, ".terrain variants count 2");
    expect(ts_terrain.variants[0].x == 12 && ts_terrain.variants[0].root_x == 9, "variant 0 mapping");
    expect(std::abs(ts_terrain.variants[0].probability - 0.45f) < 0.01f, "variant 0 probability");

    // 2. Test loading via .json path containing terrain metadata
    Tileset ts_json;
    expect(ts_json.load_from_file(jpath), "load tileset directly from terrain .json");
    expect(ts_json.variants.size() == 2, ".json variants count 2");
    expect(ts_json.tile_size == 8, ".json tile_size 8");

    std::remove(tpath.c_str());
    std::remove(jpath.c_str());
}

} // namespace

int main() {
    test_autotile_rules();
    test_collision();
    test_tilemap_editing();
    test_flood_fill();
    test_canvas_resize();
    test_io_and_settings();
    test_real_tilesets();
    test_terrain_import();

    if (g_fails) {
        std::cerr << g_fails << " test(s) failed\n";
        return 1;
    }
    std::cout << "self-test: ok\n";
    return 0;
}
