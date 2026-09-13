#include "core/collision.h"
#include "core/io.h"
#include "core/tilemap_doc.h"
#include "core/tileset.h"
#include "core/types.h"
#include "app/settings.h"
#include "gentileset.h"

#include <cmath>
#include <cstring>
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

bool create_dummy_tileset_png(const std::string& path, int tile_size, int cols = 13, int rows = 4) {
    Image im{};
    Image like{};
    like.bpp = 1;
    like.indexed = 1;
    like.palettesize = 16;
    for (int i = 0; i < 16; ++i) {
        like.palette[i][0] = static_cast<unsigned char>(i * 16);
        like.palette[i][1] = static_cast<unsigned char>(i * 16);
        like.palette[i][2] = static_cast<unsigned char>(i * 16);
        like.palette[i][3] = 255;
    }
    const unsigned w = static_cast<unsigned>(cols * tile_size);
    const unsigned h = static_cast<unsigned>(rows * tile_size);
    if (!image_alloc(&im, w, h, &like)) return false;
    std::memset(im.px, 1, w * h);
    int ok = save_png(path.c_str(), &im);
    image_free(&im);
    return ok != 0;
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

    // Test 1 solid tile (Type 1)
    // val = 0x0F, count = 1 -> "0000000f!"
    expect(compress_mde_collisions({1}) == "0000000f!", "single solid tile RLE");

    // Test 1 empty tile (Type 0)
    // val = 0x00, count = 1 -> "00000000!"
    expect(compress_mde_collisions({0}) == "00000000!", "single empty tile RLE");

    // Test 5 solid tiles (Type 1)
    // val = 0x0F, count = 5 -> "0000000f5+"
    expect(compress_mde_collisions({1, 1, 1, 1, 1}) == "0000000f5+", "5 solid tiles RLE");

    // Test Type 2 (Top, val = 0x00000001)
    expect(compress_mde_collisions({2}) == "00000001!", "Type 2 Top single tile RLE");
    // Test Type 3 (Bottom, val = 0x00000002)
    expect(compress_mde_collisions({3, 3}) == "000000022+", "Type 3 Bottom two tiles RLE");
    // Test Type 6 (Ladder, val = 0x00000010)
    expect(compress_mde_collisions({6}) == "00000010!", "Type 6 Ladder single tile RLE");
    // Test Type 7 (Tile Type 1, val = 0x00000020)
    expect(compress_mde_collisions({7}) == "00000020!", "Type 7 T1 single tile RLE");

    // Test run of mixed types: 3 empty, 2 Type 1, 4 Type 2
    // -> "000000003+0000000f2+000000014+"
    expect(compress_mde_collisions({0, 0, 0, 1, 1, 2, 2, 2, 2}) == "000000003+0000000f2+000000014+", "mixed types RLE");

    // Test the user's exact valid MD Engine collision string with 1 of each type:
    // "collisions": "00000000a2+0000000f!00000001!00000002!00000004!00000008!00000010!00000020!00000040!00000060!00000080!000000a0!000000c0!000000e0!0000000010d1+"
    const std::string user_example = "00000000a2+0000000f!00000001!00000002!00000004!00000008!00000010!00000020!00000040!00000060!00000080!000000a0!000000c0!000000e0!0000000010d1+";
    const int total_cells = 0xa2 + 13 + 0x10d1; // 162 + 13 + 4305 = 4480 cells
    std::vector<uint8_t> user_decomp = decompress_mde_collisions(user_example, total_cells);
    expect(static_cast<int>(user_decomp.size()) == total_cells, "user example decompressed size matches");
    for (int i = 0; i < 0xa2; ++i) {
        expect(user_decomp[i] == 0, "initial empty cells in user example");
    }
    for (int t = 1; t <= 13; ++t) {
        expect(user_decomp[0xa2 + t - 1] == t, "collision type 1..13 in user example");
    }
    for (int i = 0xa2 + 13; i < total_cells; ++i) {
        expect(user_decomp[i] == 0, "trailing empty cells in user example");
    }

    // Recompress and verify it matches the user string character-for-character
    std::string user_recomp = compress_mde_collisions(user_decomp);
    expect(user_recomp == user_example, "user example exact roundtrip recompression");

    // Test multi-type roundtrip decompression
    std::vector<uint8_t> orig = {0, 0, 1, 1, 2, 2, 3, 0, 1, 0, 2, 0, 0, 0, 1};
    std::string rle = compress_mde_collisions(orig);
    std::vector<uint8_t> decomp = decompress_mde_collisions(rle, static_cast<int>(orig.size()));
    expect(orig == decomp, "Multi-type RLE decompress roundtrip failed");

    // Test CollisionGrid count_types_used
    CollisionGrid cg;
    cg.width = 4;
    cg.height = 4;
    cg.data.assign(16, 0);
    expect(cg.count_types_used() == 0, "empty grid has 0 types used");
    cg.set_type(1, 1, 1);
    cg.set_type(2, 2, 1);
    expect(cg.count_types_used() == 1, "grid with only Type 1 has 1 type used");
    cg.set_type(3, 3, 2);
    expect(cg.count_types_used() == 2, "grid with Type 1 and Type 2 has 2 types used");

    // Test TilemapDoc collision type management
    TilemapDoc doc(4, 4, 16);
    expect(doc.collision_types.size() == 1, "default 1 collision type");
    expect(doc.collision_types[0].id == 1, "default collision type id is 1");
    expect(doc.collision_types[0].color == Rgb{235, 60, 50}, "default collision type is red");

    // Add Type 2 -> should default to Green
    uint8_t id2 = doc.add_collision_type();
    expect(id2 == 2, "added type id 2");
    expect(doc.collision_types.size() == 2, "now 2 collision types");
    expect(doc.collision_types[1].color == Rgb{40, 180, 100}, "Type 2 defaults to green");

    // Add Type 3 -> should default to Blue
    uint8_t id3 = doc.add_collision_type();
    expect(id3 == 3, "added type id 3");
    expect(doc.collision_types.size() == 3, "now 3 collision types");
    expect(doc.collision_types[2].color == Rgb{60, 130, 240}, "Type 3 defaults to blue");

    // Verify that tile (10, 1) is always empty (0) by default in standard autotiles
    Tileset ts_default;
    expect(ts_default.get_tile_collision(10, 1) == 0, "tile (10, 1) should default to 0 (empty)");
    ts_default.init_tile_collisions(1);
    expect(ts_default.get_tile_collision(10, 1) == 0, "tile (10, 1) remains 0 after init_tile_collisions(1)");
    expect(ts_default.get_tile_collision(9, 2) == 1, "other tiles like (9, 2) are 1");

    // Test mapping collision directly to tileset tiles:
    // Placed tile at (1, 1) uses atlas coords (9, 2)
    MapCell mc;
    mc.mode = TileMode::Stamp;
    mc.atlas_x = 9;
    mc.atlas_y = 2;
    doc.set_cell(1, 1, mc);

    // By default, tileset tile (9, 2) is Type 1 -> collision cells (2,2)..(3,3) are Type 1
    CollisionGrid col1 = doc.build_collision_grid();
    expect(col1.get_type(2, 2) == 1 && col1.get_type(3, 3) == 1, "tile at (1, 1) default Type 1 collision");
    expect(col1.count_types_used() == 1, "1 type used initially");

    // Modify collision on tileset tile (9, 2) to Type 2 (Green)
    doc.tileset.set_tile_collision(9, 2, 2);
    // Verify that ALL instances of that tile on the map immediately adjust!
    CollisionGrid col2 = doc.build_collision_grid();
    expect(col2.get_type(2, 2) == 2 && col2.get_type(3, 3) == 2, "tile at (1, 1) updated to Type 2 collision");
    expect(col2.count_types_used() == 1, "still 1 type used (Type 2)");

    // Add a second placed tile with Type 1 collision
    MapCell mc2;
    mc2.mode = TileMode::Stamp;
    mc2.atlas_x = 5;
    mc2.atlas_y = 1;
    doc.tileset.set_tile_collision(5, 1, 1);
    doc.set_cell(2, 2, mc2);

    CollisionGrid col_multi = doc.build_collision_grid();
    expect(col_multi.count_types_used() == 2, "map now uses 2 collision types");

    // Test conditional BIN export:
    // When multiple collision types are used, BIN export MUST be rejected!
    const std::string multi_bin_path = temp_path("multi_col.bin");
    std::string bin_err = export_collision_bin(doc, multi_bin_path);
    expect(!bin_err.empty(), "export_collision_bin should fail when multiple collision types exist");
    std::remove(multi_bin_path.c_str());

    // Clear tile (5, 1) to None (0) -> now only Type 2 remains on the map
    doc.tileset.set_tile_collision(5, 1, 0);
    CollisionGrid col_single = doc.build_collision_grid();
    expect(col_single.count_types_used() == 1, "now only 1 collision type used");

    // When only 1 collision type is used, BIN export MUST succeed
    std::string bin_ok = export_collision_bin(doc, multi_bin_path);
    expect(bin_ok.empty(), "export_collision_bin should succeed when single collision type used");
    std::remove(multi_bin_path.c_str());

    // Test setting tileset tile collision to None (0)
    doc.tileset.set_tile_collision(9, 2, 0);
    CollisionGrid col_none = doc.build_collision_grid();
    expect(!col_none.is_solid(2, 2) && col_none.get_type(2, 2) == 0, "tile set to None has no collision on map");
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

void test_large_level_fast_stroke() {
    using namespace tmm;
    TilemapDoc doc(200, 200, 16);
    doc.begin_stroke("Fast Stroke Large Map");
    for (int deg = 0; deg < 360; ++deg) {
        float rad = static_cast<float>(deg) * 3.14159265f / 180.0f;
        int cx = static_cast<int>(100.0f + 60.0f * std::cos(rad));
        int cy = static_cast<int>(100.0f + 60.0f * std::sin(rad));
        doc.paint_cell(cx, cy, TileMode::Stamp, 3, 2, 1);
    }
    doc.end_stroke();

    expect(doc.can_undo(), "can undo after large stroke");
    expect(doc.get_cell(160, 100).atlas_x == 3, "circle point painted");
    doc.undo();
    expect(doc.get_cell(160, 100).is_empty(), "circle point empty after undo");
    doc.redo();
    expect(doc.get_cell(160, 100).atlas_x == 3, "circle point restored after redo");
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
    doc.add_collision_type(); // Add Type 2 (Green)
    doc.tileset.cols = 12;
    doc.tileset.rows = 4;
    doc.tileset.init_tile_collisions(1);
    doc.tileset.set_tile_collision(2, 1, 2);

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
    std::string col_text;
    expect(read_text_file(col_file, col_text), "read exported collision JSON");
    expect(col_text.find("\"collisions\": \"") != std::string::npos, "contains collisions");
    expect(col_text.find("\",\n") != std::string::npos, "collisions line ends with trailing comma for copy/paste");
    expect(export_collision_bin(doc, bin_file).empty(), "export collision BIN (single type used on map)");

    TilemapDoc loaded;
    expect(load_map_json(loaded, map_file).empty(), "load map JSON");
    expect(loaded.name == "test_level", "loaded name");
    expect(loaded.width == 8 && loaded.height == 6, "loaded dims");
    expect(loaded.get_cell(3, 4).atlas_x == 2 && loaded.get_cell(3, 4).atlas_y == 1, "loaded cell content");
    expect(loaded.get_cell(0, 0).is_empty(), "loaded empty cell");
    expect(loaded.collision_types.size() == 2, "loaded 2 collision types");
    expect(loaded.collision_types[1].id == 2, "loaded type 2 id");
    expect(loaded.collision_types[1].color == Rgb{40, 180, 100}, "loaded type 2 green color");
    expect(loaded.tileset.get_tile_collision(2, 1) == 2, "loaded tile collision for (2, 1) is 2");

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
    const std::string ts16_path = temp_path("test_ts16.png");
    const std::string ts8_path = temp_path("test_ts8.png");
    expect(create_dummy_tileset_png(ts16_path, 16), "create dummy 16px tileset");
    expect(create_dummy_tileset_png(ts8_path, 8), "create dummy 8px tileset");

    // 16px tileset test
    Tileset ts16;
    expect(ts16.load_from_file(ts16_path), "load 16px tileset");
    expect(ts16.tile_size == 16, "ts16 tile_size should be 16");
    expect(ts16.cols == 13 && ts16.rows == 4, "ts16 dims should be 13x4");

    TilemapDoc doc16(10, 8, 16);
    doc16.tileset = ts16;
    // Paint a 3x3 patch with autotiling
    doc16.fill_rect({2, 2, 3, 3}, TileMode::Terrain);
    // Verify center of patch is (9, 2) or a variant of (9, 2)
    const MapCell& c33 = doc16.get_cell(3, 3);
    const bool is_center_or_var = (c33.atlas_x == 9 && c33.atlas_y == 2) ||
                                  (c33.atlas_x >= 12 && c33.atlas_y >= 0 && c33.atlas_y < 4);
    expect(is_center_or_var, "center of 3x3 terrain should be autotile center (9, 2) or its variant");

    const std::string out_png16 = temp_path("test_out_16.png");
    expect(export_composite_png(doc16, out_png16).empty(), "export composite PNG 16px");
    std::remove(out_png16.c_str());
    std::remove(ts16_path.c_str());

    // 8px tileset test
    Tileset ts8;
    expect(ts8.load_from_file(ts8_path), "load 8px tileset");
    expect(ts8.tile_size == 8, "ts8 tile_size should be 8");
    expect(ts8.cols == 13 && ts8.rows == 4, "ts8 dims should be 13x4");

    TilemapDoc doc8(12, 10, 8);
    doc8.tileset = ts8;
    doc8.paint_cell(4, 4, TileMode::Stamp, 5, 1, 1);
    expect(doc8.get_cell(4, 4).atlas_x == 5 && doc8.get_cell(4, 4).atlas_y == 1, "stamp placed on 8px map");

    const std::string out_png8 = temp_path("test_out_8.png");
    expect(export_composite_png(doc8, out_png8).empty(), "export composite PNG 8px");
    std::remove(out_png8.c_str());
    std::remove(ts8_path.c_str());
}

void test_terrain_import() {
    using namespace tmm;
    const std::string ts8_path = temp_path("test_terrain_ts8.png");
    expect(create_dummy_tileset_png(ts8_path, 8), "create dummy 8px tileset for terrain");
    const std::string tpath = temp_path("test_tileset.terrain");
    const std::string jpath = temp_path("test_tileset.json");

    // Write a .terrain file pointing to test_terrain_ts8.png
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
    std::remove(ts8_path.c_str());
}

void test_line_and_outline_rect() {
    using namespace tmm;
    TilemapDoc doc(20, 20, 16);

    // 1. Horizontal line with brush_size = 1
    doc.draw_line(2, 5, 8, 5, TileMode::Stamp, 4, 1, 1);
    for (int x = 2; x <= 8; ++x) {
        expect(doc.get_cell(x, 5).atlas_x == 4 && doc.get_cell(x, 5).atlas_y == 1,
               "horizontal line cell should be set");
    }
    expect(doc.get_cell(1, 5).is_empty(), "cell before line start should be empty");
    expect(doc.get_cell(9, 5).is_empty(), "cell after line end should be empty");
    expect(doc.get_cell(5, 4).is_empty(), "cell above line should be empty");

    // 2. Erase line
    doc.erase_line(4, 5, 6, 5, 1);
    expect(doc.get_cell(4, 5).is_empty() && doc.get_cell(5, 5).is_empty() && doc.get_cell(6, 5).is_empty(),
           "erased line cells should be empty");
    expect(!doc.get_cell(3, 5).is_empty() && !doc.get_cell(7, 5).is_empty(),
           "surrounding line cells should remain");

    // 3. Diagonal line
    doc.draw_line(0, 0, 4, 4, TileMode::Stamp, 1, 2, 1);
    for (int i = 0; i <= 4; ++i) {
        expect(doc.get_cell(i, i).atlas_x == 1 && doc.get_cell(i, i).atlas_y == 2,
               "diagonal line cell should be set");
    }

    // 4. Line with brush_size = 2
    doc.draw_line(10, 10, 12, 10, TileMode::Stamp, 2, 2, 2);
    for (int x = 10; x <= 13; ++x) {
        expect(doc.get_cell(x, 10).atlas_x == 2 && doc.get_cell(x, 11).atlas_x == 2,
               "brush_size 2 line cells should be set");
    }

    // 5. Undo and redo for draw_line
    expect(doc.can_undo(), "can undo after draw_line");
    doc.undo();
    expect(doc.get_cell(10, 10).is_empty() && doc.get_cell(12, 11).is_empty(),
           "undo should revert brush_size 2 line");
    doc.redo();
    expect(!doc.get_cell(10, 10).is_empty() && !doc.get_cell(12, 11).is_empty(),
           "redo should restore brush_size 2 line");

    // 6. Outline rect (brush_size = 1)
    TilemapDoc rect_doc(20, 20, 16);
    rect_doc.outline_rect({3, 3, 6, 6}, TileMode::Stamp, 7, 1, 1);
    // Border cells should be filled
    expect(rect_doc.get_cell(3, 3).atlas_x == 7, "rect top-left border");
    expect(rect_doc.get_cell(8, 3).atlas_x == 7, "rect top-right border");
    expect(rect_doc.get_cell(3, 8).atlas_x == 7, "rect bottom-left border");
    expect(rect_doc.get_cell(8, 8).atlas_x == 7, "rect bottom-right border");
    expect(rect_doc.get_cell(5, 3).atlas_x == 7, "rect top border");
    expect(rect_doc.get_cell(5, 8).atlas_x == 7, "rect bottom border");
    expect(rect_doc.get_cell(3, 5).atlas_x == 7, "rect left border");
    expect(rect_doc.get_cell(8, 5).atlas_x == 7, "rect right border");
    // Interior cells must be empty
    for (int y = 4; y <= 7; ++y) {
        for (int x = 4; x <= 7; ++x) {
            expect(rect_doc.get_cell(x, y).is_empty(), "outline rect interior cell must be empty");
        }
    }

    // 7. Outline rect with brush_size = 2
    TilemapDoc thick_doc(20, 20, 16);
    thick_doc.outline_rect({5, 5, 8, 8}, TileMode::Stamp, 3, 2, 2);
    // Thickness of 2: columns 5,6 and 11,12 are border; rows 5,6 and 11,12 are border
    expect(thick_doc.get_cell(5, 5).atlas_x == 3 && thick_doc.get_cell(6, 6).atlas_x == 3,
           "thick border top-left");
    expect(thick_doc.get_cell(11, 11).atlas_x == 3 && thick_doc.get_cell(12, 12).atlas_x == 3,
           "thick border bottom-right");
    // Center cell (8, 8) must be empty
    expect(thick_doc.get_cell(8, 8).is_empty(), "thick outline center (8, 8) should be empty");
    expect(thick_doc.get_cell(9, 9).is_empty(), "thick outline center (9, 9) should be empty");

    // 8. Erase outline rect
    rect_doc.erase_outline_rect({3, 3, 6, 6}, 1);
    expect(rect_doc.get_cell(3, 3).is_empty(), "erased outline border should be empty");
    expect(rect_doc.get_cell(8, 8).is_empty(), "erased outline border should be empty");

    // 9. Undo/redo outline rect
    rect_doc.undo();
    expect(rect_doc.get_cell(3, 3).atlas_x == 7, "undo erase outline should restore border");
    rect_doc.redo();
    expect(rect_doc.get_cell(3, 3).is_empty(), "redo erase outline should re-empty border");
}

void test_c_header_variants() {
    using namespace tmm;
    const std::string hpath = temp_path("test_color_variants.h");
    const std::string header_content =
        "#pragma once\n"
        "typedef struct {\n"
        "    int extra_x, extra_y;\n"
        "    int root_x, root_y;\n"
        "    int weight;\n"
        "} AtmVariantDef;\n\n"
        "static const AtmVariantDef kColorVariants[] = {\n"
        "    { 12, 0, 9, 2, 80 },\n"
        "    { 12, 1, 9, 2, 60 },\n"
        "    { 12, 2, 9, 2, 40 }\n"
        "};\n";

    {
        std::ofstream f(hpath);
        f << header_content;
    }

    Tileset ts;
    expect(ts.import_variants_file(hpath), "import variants from C header file");
    expect(ts.variants.size() == 3, "should parse 3 variants from header");
    expect(ts.variants[0].x == 12 && ts.variants[0].y == 0 && ts.variants[0].root_x == 9 && ts.variants[0].root_y == 2,
           "variant 0 mapping");
    expect(std::abs(ts.variants[0].probability - 0.8f) < 0.01f, "variant 0 weight converted to probability 0.8");
    expect(std::abs(ts.variants[1].probability - 0.6f) < 0.01f, "variant 1 weight converted to probability 0.6");
    expect(std::abs(ts.variants[2].probability - 0.4f) < 0.01f, "variant 2 weight converted to probability 0.4");

    // Test resolve_variant with high roll
    Cell resolved = ts.resolve_variant(9, 2, 0.95f);
    expect(resolved.x == 12, "high roll should pick one of the column 12 variants");

    std::remove(hpath.c_str());
}

void test_selection_and_clipping() {
    using namespace tmm;
    TilemapDoc doc(20, 20, 16);

    // 1. in_clip without clip rect returns true everywhere
    expect(doc.get_clip_rect() == nullptr, "initial clip rect should be null");
    expect(doc.in_clip(0, 0), "in_clip(0,0) without clip");
    expect(doc.in_clip(5, 5), "in_clip(5,5) without clip");

    // 2. Set clip rect to [4, 4, 6, 6] (x: 4..9, y: 4..9)
    Rect clip{4, 4, 6, 6};
    doc.set_clip_rect(&clip);
    expect(doc.get_clip_rect() != nullptr, "clip rect should be set");
    expect(doc.get_clip_rect()->x == 4 && doc.get_clip_rect()->w == 6, "clip rect coordinates");

    expect(doc.in_clip(4, 4), "in_clip top-left corner");
    expect(doc.in_clip(9, 9), "in_clip bottom-right corner");
    expect(!doc.in_clip(3, 4), "in_clip outside left");
    expect(!doc.in_clip(10, 4), "in_clip outside right");
    expect(!doc.in_clip(4, 3), "in_clip outside top");
    expect(!doc.in_clip(4, 10), "in_clip outside bottom");

    // 3. Drawing tools obey clipping
    // Fill the whole map with a fill_rect of size [0, 0, 20, 20]
    // Because clip is [4, 4, 6, 6], only cells inside [4, 4, 6, 6] should be painted!
    doc.fill_rect({0, 0, 20, 20}, TileMode::Stamp, 1, 1);

    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 20; ++x) {
            const bool inside = (x >= 4 && x < 10 && y >= 4 && y < 10);
            const MapCell& c = doc.get_cell(x, y);
            if (inside) {
                expect(!c.is_empty() && c.atlas_x == 1 && c.atlas_y == 1, "inside clip must be filled");
            } else {
                expect(c.is_empty(), "outside clip must remain empty");
            }
        }
    }

    // 4. Paint cell across the boundary
    // Paint at (3, 3) with brush size 3 (covers [3..5, 3..5])
    // Only (4,4), (4,5), (5,4), (5,5) are inside clip!
    doc.paint_cell(3, 3, TileMode::Stamp, 2, 2, 3);
    expect(doc.get_cell(3, 3).is_empty(), "outside clip (3,3) remains empty after paint");
    expect(doc.get_cell(4, 4).atlas_x == 2 && doc.get_cell(4, 4).atlas_y == 2, "inside clip (4,4) was painted");

    // 5. Erase cell across the boundary
    // Erase at (3, 3) with brush size 3
    doc.erase_cell(3, 3, 3);
    expect(doc.get_cell(3, 3).is_empty(), "(3,3) still empty");
    expect(doc.get_cell(4, 4).is_empty(), "inside clip (4,4) was erased");
    expect(!doc.get_cell(6, 6).is_empty(), "untouched cell (6,6) still painted");

    // 6. Flood fill is contained by clip
    // Fill inside clip from (6, 6)
    doc.flood_fill(6, 6, TileMode::Stamp, 3, 3);
    expect(doc.get_cell(6, 6).atlas_x == 3 && doc.get_cell(6, 6).atlas_y == 3, "flood fill inside clip");
    expect(doc.get_cell(0, 0).is_empty(), "flood fill did not escape clip");

    // 7. Erase rect (selection clear)
    doc.erase_rect(clip);
    for (int y = clip.y; y < clip.bottom(); ++y) {
        for (int x = clip.x; x < clip.right(); ++x) {
            expect(doc.get_cell(x, y).is_empty(), "erase_rect cleared cell in selection");
        }
    }

    // 8. Copy and paste
    doc.set_clip_rect(nullptr);
    doc.paint_cell(2, 2, TileMode::Stamp, 5, 5, 2); // 2x2 stamp at (2,2)
    Clipboard clip_data = doc.copy_rect({2, 2, 2, 2});
    expect(clip_data.w == 2 && clip_data.h == 2, "copy_rect size");
    expect(clip_data.cells.size() == 4, "copy_rect 4 cells copied");

    // Paste at (12, 12)
    doc.paste_clipboard(12, 12, clip_data);
    expect(!doc.get_cell(12, 12).is_empty() && doc.get_cell(12, 12).atlas_x == 5, "pasted cell at (12,12)");
    expect(!doc.get_cell(13, 13).is_empty() && doc.get_cell(13, 13).atlas_x == 5, "pasted cell at (13,13)");

    // 9. Move selected tiles and single-step undo
    // Place a 3x3 block at (5, 5)
    doc.paint_cell(5, 5, TileMode::Stamp, 7, 7, 3);
    expect(!doc.get_cell(5, 5).is_empty() && doc.get_cell(5, 5).atlas_x == 7, "tile at (5,5) before move");
    expect(doc.get_cell(10, 10).is_empty(), "target (10,10) empty before move");

    // Move selection from (5,5) to (10,10)
    Rect sel{5, 5, 3, 3};
    doc.begin_stroke("Move Selection");
    Clipboard moving_tiles = doc.copy_rect(sel);
    doc.erase_rect(sel);
    sel.x = 10;
    sel.y = 10;
    doc.paste_clipboard(sel.x, sel.y, moving_tiles);
    doc.end_stroke();

    // Verify origin is cleared and destination has the moved tiles
    expect(doc.get_cell(5, 5).is_empty(), "origin (5,5) cleared after move");
    expect(!doc.get_cell(10, 10).is_empty() && doc.get_cell(10, 10).atlas_x == 7, "destination (10,10) has moved tile");
    expect(!doc.get_cell(12, 12).is_empty() && doc.get_cell(12, 12).atlas_x == 7, "destination (12,12) has moved tile");

    // Single-step undo must restore origin and clear destination
    expect(doc.undo(), "undo move selection");
    expect(!doc.get_cell(5, 5).is_empty() && doc.get_cell(5, 5).atlas_x == 7, "origin (5,5) restored after undo");
    expect(doc.get_cell(10, 10).is_empty(), "destination (10,10) cleared after undo");

    // 10. Non-destructive moving lifecycle with cancel_stroke (Escape / cancellation)
    doc.paint_cell(3, 3, TileMode::Stamp, 2, 2, 2); // 2x2 stamp at (3,3)
    doc.paint_cell(15, 15, TileMode::Stamp, 9, 9, 2); // 2x2 stamp at (15,15)
    expect(doc.get_cell(3, 3).atlas_x == 2, "initial tile at (3,3)");
    expect(doc.get_cell(15, 15).atlas_x == 9, "underneath tile at (15,15) intact");

    // Lift selection at (3,3)
    doc.begin_stroke("Move Selection");
    Clipboard lifted = doc.copy_rect({3, 3, 2, 2});
    doc.erase_rect({3, 3, 2, 2});
    expect(doc.get_cell(3, 3).is_empty(), "origin (3,3) erased on lift");

    // Move floating selection over (15,15) - underlying tiles must NOT be erased while moving!
    expect(doc.get_cell(15, 15).atlas_x == 9, "underneath tile at (15,15) NOT erased during move");
    expect(doc.get_cell(16, 16).atlas_x == 9, "underneath tile at (16,16) NOT erased during move");

    // Cancel move (e.g. user presses Esc or cancels)
    doc.cancel_stroke();
    expect(doc.get_cell(3, 3).atlas_x == 2, "origin (3,3) restored after cancel_stroke");
    expect(doc.get_cell(15, 15).atlas_x == 9, "underneath tile at (15,15) still intact after cancel");

    // 11. Move selection over existing tiles and apply on deselect (Ctrl+D / right-click)
    doc.paint_cell(2, 2, TileMode::Stamp, 4, 4, 2); // 2x2 stamp at (2,2)
    doc.paint_cell(8, 8, TileMode::Stamp, 6, 6, 2); // 2x2 stamp at (8,8)
    expect(doc.get_cell(2, 2).atlas_x == 4, "origin tile at (2,2)");
    expect(doc.get_cell(8, 8).atlas_x == 6, "target tile at (8,8)");

    // Lift and move
    doc.begin_stroke("Move Selection");
    Clipboard lifted2 = doc.copy_rect({2, 2, 2, 2});
    doc.erase_rect({2, 2, 2, 2});
    // Target still untouched while hovering/moving:
    expect(doc.get_cell(8, 8).atlas_x == 6, "target (8,8) untouched while selection floats");

    // Apply on deselect:
    doc.paste_clipboard(8, 8, lifted2);
    doc.end_stroke();

    // Now target has the moved tiles:
    expect(doc.get_cell(2, 2).is_empty(), "origin (2,2) is empty after apply");
    expect(doc.get_cell(8, 8).atlas_x == 4, "target (8,8) now has moved tile");

    // Single-step undo restores origin AND destination's original tiles!
    expect(doc.undo(), "undo apply moved selection");
    expect(doc.get_cell(2, 2).atlas_x == 4, "origin (2,2) restored after undo");
    expect(doc.get_cell(8, 8).atlas_x == 6, "target (8,8) restored to previous tile after undo");

    // Redo re-applies the move:
    expect(doc.redo(), "redo apply moved selection");
    expect(doc.get_cell(2, 2).is_empty(), "origin (2,2) empty after redo");
    expect(doc.get_cell(8, 8).atlas_x == 4, "target (8,8) has moved tile after redo");
}

void test_brush_size_and_tools() {
    using namespace tmm;
    TilemapDoc doc(20, 20, 16);

    // Verify all brush sizes 1..4 produce expected bounds when drawing
    for (int bs = 1; bs <= 4; ++bs) {
        doc.begin_stroke("Test Brush Size");
        doc.paint_cell(5, 5, TileMode::Stamp, bs, bs, bs);
        doc.end_stroke();

        for (int dy = 0; dy < bs; ++dy) {
            for (int dx = 0; dx < bs; ++dx) {
                expect(doc.get_cell(5 + dx, 5 + dy).atlas_x == bs, "cell within brush thickness painted");
            }
        }
        expect(doc.get_cell(5 + bs, 5).is_empty(), "cell outside brush thickness empty");
        expect(doc.get_cell(5, 5 + bs).is_empty(), "cell outside brush thickness empty");

        // Erase with brush size
        doc.begin_stroke("Test Erase Size");
        doc.erase_cell(5, 5, bs);
        doc.end_stroke();
        for (int dy = 0; dy < bs; ++dy) {
            for (int dx = 0; dx < bs; ++dx) {
                expect(doc.get_cell(5 + dx, 5 + dy).is_empty(), "cell erased with brush size");
            }
        }
    }

    // Test brush thickness clamping logic [1, 4]
    int b = 1;
    b = std::max(1, b - 1);
    expect(b == 1, "cannot decrease brush size below 1");
    b = std::min(4, b + 1);
    expect(b == 2, "increase brush size to 2");
    b = std::min(4, b + 5);
    expect(b == 4, "cannot increase brush size above 4");

    // Test settings persistence of brush sizes
    Settings s;
    s.brush_size = 4;
    std::string text = format_settings(s);
    Settings loaded;
    expect(parse_settings_text(loaded, text), "parse settings with brush_size=4");
    expect(loaded.brush_size == 4, "loaded brush_size is 4");
}

void test_circle_mode_and_clipping() {
    using namespace tmm;
    TilemapDoc doc;
    doc.reset(20, 20, 16);

    // 1. fill_ellipse test
    // In a 9x9 box at (1, 1), center is (5.5, 5.5), radius is (4.5, 4.5).
    doc.fill_ellipse({1, 1, 9, 9}, TileMode::Stamp, 3, 3);
    expect(doc.get_cell(5, 5).atlas_x == 3, "center of filled circle is painted");
    expect(doc.get_cell(5, 1).atlas_x == 3, "top tangent of circle is painted");
    expect(doc.get_cell(5, 9).atlas_x == 3, "bottom tangent of circle is painted");
    expect(doc.get_cell(1, 5).atlas_x == 3, "left tangent of circle is painted");
    expect(doc.get_cell(9, 5).atlas_x == 3, "right tangent of circle is painted");
    expect(doc.get_cell(1, 1).is_empty(), "corner (1, 1) outside circle is empty");
    expect(doc.get_cell(9, 1).is_empty(), "corner (9, 1) outside circle is empty");
    expect(doc.get_cell(1, 9).is_empty(), "corner (1, 9) outside circle is empty");
    expect(doc.get_cell(9, 9).is_empty(), "corner (9, 9) outside circle is empty");

    // 2. outline_ellipse test
    doc.reset(20, 20, 16);
    doc.outline_ellipse({1, 1, 9, 9}, TileMode::Stamp, 4, 4, 1);
    expect(doc.get_cell(5, 5).is_empty(), "center of outline circle is empty");
    expect(doc.get_cell(5, 1).atlas_x == 4, "top border of outline circle is painted");
    expect(doc.get_cell(1, 1).is_empty(), "corner outside outline circle is empty");

    // Outline ellipse with thickness = 2
    doc.reset(20, 20, 16);
    doc.outline_ellipse({1, 1, 9, 9}, TileMode::Stamp, 4, 4, 2);
    expect(doc.get_cell(5, 5).is_empty(), "center of thick outline circle is empty");
    expect(doc.get_cell(5, 1).atlas_x == 4, "outer edge (5, 1) is painted");
    expect(doc.get_cell(5, 2).atlas_x == 4, "inner edge (5, 2) is painted with thickness 2");

    // 3. erase_ellipse and erase_outline_ellipse
    doc.fill_rect({0, 0, 20, 20}, TileMode::Stamp, 7, 7);
    expect(doc.get_cell(5, 5).atlas_x == 7, "map filled with tiles");
    doc.erase_ellipse({1, 1, 9, 9});
    expect(doc.get_cell(5, 5).is_empty(), "circle center erased");
    expect(doc.get_cell(1, 1).atlas_x == 7, "corner (1,1) untouched by erase_ellipse");

    doc.fill_rect({0, 0, 20, 20}, TileMode::Stamp, 7, 7);
    doc.erase_outline_ellipse({1, 1, 9, 9}, 1);
    expect(doc.get_cell(5, 5).atlas_x == 7, "circle center untouched by erase_outline_ellipse");
    expect(doc.get_cell(5, 1).is_empty(), "circle perimeter erased by erase_outline_ellipse");
    expect(doc.get_cell(1, 1).atlas_x == 7, "corner (1,1) untouched by erase_outline_ellipse");

    // 4. copy_ellipse and cut_ellipse
    doc.fill_rect({0, 0, 20, 20}, TileMode::Stamp, 8, 8);
    Clipboard clip = doc.copy_ellipse({1, 1, 9, 9});
    expect(!clip.is_empty(), "copy_ellipse returns non-empty clipboard");
    for (const auto& item : clip.cells) {
        const int map_x = 1 + item.first.x;
        const int map_y = 1 + item.first.y;
        expect(doc.in_bounds(map_x, map_y), "copied cell in bounds");
        expect(!(item.first.x == 0 && item.first.y == 0), "corner cell not copied in copy_ellipse");
    }

    Clipboard cut_clip;
    doc.cut_ellipse({1, 1, 9, 9}, cut_clip);
    expect(doc.get_cell(5, 5).is_empty(), "cut_ellipse erased circle interior");
    expect(doc.get_cell(1, 1).atlas_x == 8, "cut_ellipse preserved corner (1, 1)");

    // 5. Selection with ClipShape::Ellipse
    doc.reset(20, 20, 16);
    Rect sel_rect{1, 1, 9, 9};
    doc.set_clip_rect(&sel_rect, TilemapDoc::ClipShape::Ellipse);
    expect(doc.in_clip(5, 5), "center is inside circular clip");
    expect(!doc.in_clip(1, 1), "corner is outside circular clip");
    expect(!doc.in_clip(0, 0), "outside bounding box is outside clip");

    // Paint entire rectangle bounding box - only cells inside ellipse should be modified
    for (int y = 1; y <= 9; ++y) {
        for (int x = 1; x <= 9; ++x) {
            doc.paint_cell(x, y, TileMode::Stamp, 5, 5);
        }
    }
    expect(doc.get_cell(5, 5).atlas_x == 5, "in-clip cell painted");
    expect(doc.get_cell(1, 1).is_empty(), "out-of-clip corner remains empty");

    // Flood fill with circular clip
    doc.reset(20, 20, 16);
    doc.set_clip_rect(&sel_rect, TilemapDoc::ClipShape::Ellipse);
    doc.flood_fill(5, 5, TileMode::Stamp, 6, 6);
    expect(doc.get_cell(5, 5).atlas_x == 6, "center flooded");
    expect(doc.get_cell(1, 1).is_empty(), "corner untouched by flood fill due to circular clip");
    expect(doc.get_cell(0, 0).is_empty(), "outside untouched by flood fill");

    doc.set_clip_rect(nullptr);

    // 6. Shift lock aspect ratio
    auto test_drag_rect = [](int start_x, int start_y, int curr_x, int curr_y, bool square, int doc_w, int doc_h) -> Rect {
        start_x = std::clamp(start_x, 0, doc_w - 1);
        start_y = std::clamp(start_y, 0, doc_h - 1);
        const int dx = curr_x - start_x;
        const int dy = curr_y - start_y;
        const int sx = (dx >= 0) ? 1 : -1;
        const int sy = (dy >= 0) ? 1 : -1;
        if (square) {
            const int max_side_x = (sx >= 0) ? (doc_w - 1 - start_x) : start_x;
            const int max_side_y = (sy >= 0) ? (doc_h - 1 - start_y) : start_y;
            int side = std::max(std::abs(dx), std::abs(dy));
            side = std::min(side, std::min(max_side_x, max_side_y));
            const int target_x = start_x + sx * side;
            const int target_y = start_y + sy * side;
            const int rx = std::min(start_x, target_x);
            const int ry = std::min(start_y, target_y);
            return {rx, ry, side + 1, side + 1};
        } else {
            const int cx_clamped = std::clamp(curr_x, 0, doc_w - 1);
            const int cy_clamped = std::clamp(curr_y, 0, doc_h - 1);
            const int rx = std::min(start_x, cx_clamped);
            const int ry = std::min(start_y, cy_clamped);
            const int rx2 = std::max(start_x, cx_clamped);
            const int ry2 = std::max(start_y, cy_clamped);
            return {rx, ry, rx2 - rx + 1, ry2 - ry + 1};
        }
    };

    Rect non_square = test_drag_rect(2, 2, 8, 4, false, 20, 20);
    expect(non_square.w == 7 && non_square.h == 3, "non-shift drag has free aspect ratio");

    Rect square_se = test_drag_rect(2, 2, 8, 4, true, 20, 20);
    expect(square_se.w == square_se.h && square_se.w == 7, "shift drag enforces 1:1 aspect ratio SE");
    expect(square_se.x == 2 && square_se.y == 2, "square origin at top-left");

    Rect square_nw = test_drag_rect(8, 8, 3, 5, true, 20, 20);
    expect(square_nw.w == square_nw.h && square_nw.w == 6, "shift drag enforces 1:1 aspect ratio NW");
    expect(square_nw.x == 3 && square_nw.y == 3, "square origin at top-left for NW drag");

    Rect square_clamped = test_drag_rect(18, 18, 25, 22, true, 20, 20);
    expect(square_clamped.w == square_clamped.h, "shift drag clamped to bounds maintains 1:1");
    expect(square_clamped.right() <= 20 && square_clamped.bottom() <= 20, "shift drag stays within bounds");
}

void test_default_size_and_8px_dimensions() {
    using namespace tmm;

    // 1. Default TilemapDoc constructor
    TilemapDoc def_doc;
    expect(def_doc.width_8px() == 40, "default doc width in 8x8 tiles should be 40");
    expect(def_doc.height_8px() == 28, "default doc height in 8x8 tiles should be 28");
    expect(def_doc.pixel_width() == 320, "default doc pixel width should be 320");
    expect(def_doc.pixel_height() == 224, "default doc pixel height should be 224");

    // 2. Creating a 40x28 map with 8x8 tile size
    TilemapDoc doc8;
    doc8.reset_8px(40, 28, 8);
    expect(doc8.width == 40 && doc8.height == 28, "doc8 grid cells should be 40x28");
    expect(doc8.width_8px() == 40 && doc8.height_8px() == 28, "doc8 width/height in 8px should be 40x28");
    expect(doc8.pixel_width() == 320 && doc8.pixel_height() == 224, "doc8 pixel resolution should be 320x224");

    // 3. Creating a 40x28 map with 16x16 tile size
    TilemapDoc doc16;
    doc16.reset_8px(40, 28, 16);
    expect(doc16.width == 20 && doc16.height == 14, "doc16 grid cells should be 20x14 for 16x16 tiles");
    expect(doc16.width_8px() == 40 && doc16.height_8px() == 28, "doc16 width/height in 8px should be 40x28");
    expect(doc16.pixel_width() == 320 && doc16.pixel_height() == 224, "doc16 pixel resolution should be 320x224");

    // 4. Verify collision grid is 40x28 in both cases
    CollisionGrid col8 = doc8.build_collision_grid();
    expect(col8.width == 40 && col8.height == 28, "col8 dimensions should be 40x28");

    CollisionGrid col16 = doc16.build_collision_grid();
    expect(col16.width == 40 && col16.height == 28, "col16 dimensions should be 40x28");

    // 5. Verify composite PNG export creates 320x224 image in both cases
    const std::string ts8_path = temp_path("test_dim_ts8.png");
    const std::string ts16_path = temp_path("test_dim_ts16.png");
    expect(create_dummy_tileset_png(ts8_path, 8), "create dummy ts8");
    expect(create_dummy_tileset_png(ts16_path, 16), "create dummy ts16");

    Tileset ts8;
    expect(ts8.load_from_file(ts8_path), "load ts8");
    doc8.tileset = ts8;

    Tileset ts16;
    expect(ts16.load_from_file(ts16_path), "load ts16");
    doc16.tileset = ts16;

    const std::string out_png8 = temp_path("test_dim_out8.png");
    expect(export_composite_png(doc8, out_png8).empty(), "export composite png 8px map");

    Image out_im8{};
    expect(load_png(out_png8.c_str(), &out_im8), "load exported 8px composite PNG");
    expect(out_im8.w == 320 && out_im8.h == 224, "8px map exported image resolution should be 320x224");
    image_free(&out_im8);
    std::remove(out_png8.c_str());
    std::remove(ts8_path.c_str());

    const std::string out_png16 = temp_path("test_dim_out16.png");
    expect(export_composite_png(doc16, out_png16).empty(), "export composite png 16px map");

    Image out_im16{};
    expect(load_png(out_png16.c_str(), &out_im16), "load exported 16px composite PNG");
    expect(out_im16.w == 320 && out_im16.h == 224, "16px map exported image resolution should be 320x224");
    image_free(&out_im16);
    std::remove(out_png16.c_str());
    std::remove(ts16_path.c_str());

    // 6. Test resizing in 8x8 tile units
    doc16.resize_8px(80, 56);
    expect(doc16.width == 40 && doc16.height == 28, "resizing 16px doc to 80x56 (8px) yields 40x28 grid");
    expect(doc16.width_8px() == 80 && doc16.height_8px() == 56, "doc16 width/height in 8px should be 80x56");
    expect(doc16.pixel_width() == 640 && doc16.pixel_height() == 448, "doc16 pixel resolution should be 640x448");
}

} // namespace

int main() {
    test_autotile_rules();
    test_collision();
    test_tilemap_editing();
    test_large_level_fast_stroke();
    test_flood_fill();
    test_canvas_resize();
    test_io_and_settings();
    test_real_tilesets();
    test_terrain_import();
    test_line_and_outline_rect();
    test_c_header_variants();
    test_selection_and_clipping();
    test_brush_size_and_tools();
    test_circle_mode_and_clipping();
    test_default_size_and_8px_dimensions();

    if (g_fails) {
        std::cerr << g_fails << " test(s) failed\n";
        return 1;
    }
    std::cout << "self-test: ok\n";
    return 0;
}
