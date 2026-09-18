#include "core/collision.h"
#include "core/io.h"
#include "core/tilemap_doc.h"
#include "core/tileset.h"
#include "core/types.h"
#include "core/zip.h"
#include "app/settings.h"
#include "app/theme.h"
#include "gentileset.h"

#include "../deps/tileset-maker-thingy/src/app/settings.h"
#include "../deps/tileset-maker-thingy/src/app/tileset_editor.h"
#include "../deps/tileset-maker-thingy/src/core/convert.h"
#include "../deps/tileset-maker-thingy/src/core/io.h"
#include "../deps/tileset-maker-thingy/src/core/project.h"

#include "imgui.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
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
    std::error_code ec;
    auto p = std::filesystem::temp_directory_path(ec);
    if (!ec && !p.empty()) {
        return (p / name).string();
    }
    const char* dir = std::getenv("TMPDIR");
    if (!dir || !*dir) dir = std::getenv("TEMP");
    if (!dir || !*dir) dir = std::getenv("TMP");
    if (!dir || !*dir) dir = ".";
    return (std::filesystem::path(dir) / name).string();
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

    // Test mutable get_collision_type overload
    CollisionType* mut_ct = doc.get_collision_type(1);
    expect(mut_ct != nullptr, "get_collision_type(1) non-const found");
    mut_ct->name = "Custom Wall";
    mut_ct->color = Rgb{100, 150, 200};
    const CollisionType* const_ct = const_cast<const TilemapDoc&>(doc).get_collision_type(1);
    expect(const_ct != nullptr && const_ct->name == "Custom Wall", "customized collision type name persisted");
    expect(const_ct->color.r == 100 && const_ct->color.g == 150 && const_ct->color.b == 200, "customized collision color persisted");

    // Test batch fill and clear on tileset with tile (10, 1) protection
    doc.tileset.cols = 12;
    doc.tileset.rows = 4;
    doc.tileset.init_tile_collisions(1);
    for (int r = 0; r < doc.tileset.rows; ++r) {
        for (int c = 0; c < doc.tileset.cols; ++c) {
            doc.tileset.set_tile_collision(c, r, 2);
        }
    }
    expect(doc.tileset.get_tile_collision(0, 0) == 2, "batch fill set tile (0, 0) to 2");
    expect(doc.tileset.get_tile_collision(9, 2) == 2, "batch fill set tile (9, 2) to 2");
    expect(doc.tileset.get_tile_collision(10, 1) == 0, "batch fill preserved tile (10, 1) as 0");

    for (int r = 0; r < doc.tileset.rows; ++r) {
        for (int c = 0; c < doc.tileset.cols; ++c) {
            doc.tileset.set_tile_collision(c, r, 0);
        }
    }
    expect(doc.tileset.get_tile_collision(0, 0) == 0, "batch clear set tile (0, 0) to 0");
    expect(doc.tileset.get_tile_collision(10, 1) == 0, "batch clear kept tile (10, 1) as 0");
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
    if (loaded.collision_types.size() >= 2) {
        expect(loaded.collision_types[1].id == 2, "loaded type 2 id");
        expect(loaded.collision_types[1].color == Rgb{40, 180, 100}, "loaded type 2 green color");
    }
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
    s.sidebar_page = 2; // Export
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
    expect(s2.sidebar_page == 2, "settings sidebar_page export");
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
    expect(!s3.dark && s3.brush_size == 3 && s3.paint_mode == 1 && s3.sidebar_page == 2, "loaded settings file content matches");
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
    if (ts_terrain.variants.size() >= 2) {
        expect(ts_terrain.variants[0].x == 12 && ts_terrain.variants[0].root_x == 9, "variant 0 mapping");
        expect(std::abs(ts_terrain.variants[0].probability - 0.45f) < 0.01f, "variant 0 probability");
    }

    // 2. Test loading via .json path containing terrain metadata
    Tileset ts_json;
    expect(ts_json.load_from_file(jpath), "load tileset directly from terrain .json");
    expect(ts_json.variants.size() == 2, ".json variants count 2");
    expect(ts_json.tile_size == 8, ".json tile_size 8");

    // 3. Test that loading a different terrain without its companion PNG fails and does not reuse old PNG
    const std::string diff_tpath = temp_path("different_terrain.terrain");
    {
        std::ofstream f(diff_tpath);
        f << "{\n  \"version\": 1,\n  \"tileset\": \"non_existent_diff.png\",\n  \"variants\": []\n}\n";
    }
    expect(!ts_terrain.load_from_file(diff_tpath), "different terrain without matching PNG must fail even if tileset was valid");
    expect(!ts_terrain.error.empty(), "error message is set when companion PNG is missing");

    // 4. Test loading different terrain with override_png succeeds
    const std::string diff_png = temp_path("different_terrain.png");
    expect(create_dummy_tileset_png(diff_png, 16), "create dummy 16px tileset");
    expect(ts_terrain.load_from_file(diff_tpath, diff_png), "loading different terrain with override_png succeeds");
    expect(ts_terrain.tile_size == 16, "reloaded tileset tile_size is 16");

    std::remove(diff_tpath.c_str());
    std::remove(diff_png.c_str());
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
    if (ts.variants.size() >= 3) {
        expect(ts.variants[0].x == 12 && ts.variants[0].y == 0 && ts.variants[0].root_x == 9 && ts.variants[0].root_y == 2,
               "variant 0 mapping");
        expect(std::abs(ts.variants[0].probability - 0.8f) < 0.01f, "variant 0 weight converted to probability 0.8");
        expect(std::abs(ts.variants[1].probability - 0.6f) < 0.01f, "variant 1 weight converted to probability 0.6");
        expect(std::abs(ts.variants[2].probability - 0.4f) < 0.01f, "variant 2 weight converted to probability 0.4");
    }

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

void test_buffer_and_outside_zone() {
    using namespace tmm;

    // 1. Check default buffer properties
    TilemapDoc doc(20, 14, 16);
    expect(doc.buffer == 1, "default buffer should be 1 tile");
    expect(doc.total_width() == 22, "total_width should be 20 + 2 = 22");
    expect(doc.total_height() == 16, "total_height should be 14 + 2 = 16");
    expect(doc.total_cells() == 22 * 16, "total_cells should be 22 * 16 = 352");

    // Bounds checking
    expect(doc.in_bounds(-1, -1), "(-1, -1) should be in_bounds");
    expect(doc.in_bounds(20, 14), "(20, 14) should be in_bounds");
    expect(!doc.in_bounds(-2, 0), "(-2, 0) should NOT be in_bounds");
    expect(!doc.in_bounds(21, 0), "(21, 0) should NOT be in_bounds");
    expect(!doc.in_bounds(0, -2), "(0, -2) should NOT be in_bounds");
    expect(!doc.in_bounds(0, 15), "(0, 15) should NOT be in_bounds");

    expect(doc.in_active_bounds(0, 0), "(0, 0) is active bounds");
    expect(doc.in_active_bounds(19, 13), "(19, 13) is active bounds");
    expect(!doc.in_active_bounds(-1, 0), "(-1, 0) is NOT active bounds");
    expect(!doc.in_active_bounds(20, 0), "(20, 0) is NOT active bounds");
    expect(doc.is_buffer_cell(-1, 0), "(-1, 0) is a buffer cell");
    expect(!doc.is_buffer_cell(0, 0), "(0, 0) is NOT a buffer cell");

    // 2. Autotiling: painting active area vs buffer area
    // Paint terrain across all active cells 0..19, 0..13
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < 20; ++x) {
            MapCell c;
            c.mode = TileMode::Terrain;
            doc.set_cell(x, y, c);
        }
    }
    doc.solve_all_autotiles();

    // Cell (0, 0) has no West or North neighbor in the buffer yet, so it is a corner edge
    const MapCell c00_before = doc.get_cell(0, 0);
    expect(c00_before.atlas_x != 9 || c00_before.atlas_y != 2,
           "cell (0, 0) without buffer painted should be an edge tile, not center (9, 2)");

    // Now paint the outside buffer zone with terrain (filling the 1-tile ring around the outside)
    for (int y = -1; y <= 14; ++y) {
        for (int x = -1; x <= 20; ++x) {
            if (doc.is_buffer_cell(x, y)) {
                MapCell c;
                c.mode = TileMode::Terrain;
                doc.set_cell(x, y, c);
            }
        }
    }
    doc.solve_all_autotiles();

    // Now every cell in the active map (including cell (0, 0) and (19, 13)) has terrain neighbors on all 8 sides!
    const MapCell c00_after = doc.get_cell(0, 0);
    expect(c00_after.atlas_x == 9 && c00_after.atlas_y == 2,
           "cell (0, 0) with buffer painted should now be a seamless center tile (9, 2)");
    const MapCell c_br_after = doc.get_cell(19, 13);
    expect(c_br_after.atlas_x == 9 && c_br_after.atlas_y == 2,
           "bottom-right active cell (19, 13) should also be a seamless center tile (9, 2)");

    // Meanwhile, the edge is out in the buffer (e.g. at (-1, -1))
    const MapCell c_buf = doc.get_cell(-1, -1);
    expect(c_buf.atlas_x != 9 || c_buf.atlas_y != 2,
           "buffer corner cell (-1, -1) should hold the outer edge tile");

    // 3. Composite PNG export ignores buffer and outputs exact active map dimensions
    const std::string ts_path = temp_path("test_buf_ts.png");
    expect(create_dummy_tileset_png(ts_path, 16), "create dummy tileset");
    expect(doc.tileset.load_from_file(ts_path), "load dummy tileset");
    const std::string out_png = temp_path("test_buf_out.png");
    expect(export_composite_png(doc, out_png).empty(), "export composite png");

    Image im{};
    expect(load_png(out_png.c_str(), &im), "load exported image");
    expect(im.w == 320 && im.h == 224, "exported PNG should be active map size 320x224, not 352x256");
    image_free(&im);
    std::remove(out_png.c_str());
    std::remove(ts_path.c_str());

    // 4. Collision generation only covers active map
    CollisionGrid cg = doc.build_collision_grid();
    expect(cg.width == 40 && cg.height == 28, "collision grid is 40x28 (active map)");

    // 5. JSON save & load round-trip preserves buffer cells
    const std::string map_json_path = temp_path("test_buf_map.json");
    expect(save_map_json(doc, map_json_path).empty(), "save map JSON with buffer");

    TilemapDoc reloaded;
    expect(load_map_json(reloaded, map_json_path).empty(), "load map JSON with buffer");
    expect(reloaded.buffer == 1, "reloaded buffer is 1");
    expect(reloaded.width == 20 && reloaded.height == 14, "reloaded active dimensions 20x14");
    expect(reloaded.get_cell(-1, -1).mode == TileMode::Terrain, "reloaded preserves buffer cell at (-1, -1)");
    expect(reloaded.get_cell(0, 0).atlas_x == 9 && reloaded.get_cell(0, 0).atlas_y == 2,
           "reloaded preserves solved seamless center tile at (0, 0)");
    std::remove(map_json_path.c_str());

    // 6. Clear map clears active map AND buffer cells
    doc.clear_cells();
    expect(doc.get_cell(0, 0).is_empty(), "active cell cleared");
    expect(doc.get_cell(-1, -1).is_empty(), "buffer cell cleared");
    expect(doc.get_cell(0, 0).atlas_x == 10 && doc.get_cell(0, 0).atlas_y == 1,
           "cleared active cell is reset to tile (10, 1)");
    expect(doc.get_cell(-1, -1).atlas_x == 10 && doc.get_cell(-1, -1).atlas_y == 1,
           "cleared buffer cell is reset to tile (10, 1)");
}

void test_empty_background_tile_10_1() {
    using namespace tmm;

    // 1. Newly constructed doc fills entire drawing area (active + buffer) with tile (10, 1)
    TilemapDoc doc(20, 14, 16);
    for (int y = -doc.buffer; y < doc.height + doc.buffer; ++y) {
        for (int x = -doc.buffer; x < doc.width + doc.buffer; ++x) {
            const MapCell& c = doc.get_cell(x, y);
            expect(c.is_empty(), "initial cell must be empty");
            expect(c.atlas_x == 10 && c.atlas_y == 1, "initial cell must have atlas coords (10, 1)");
        }
    }

    // 2. Tile (10, 1) is strictly and always empty / no collision
    expect(doc.tileset.get_tile_collision(10, 1) == 0, "(10, 1) must be 0 collision by default");
    doc.tileset.set_tile_collision(10, 1, 4);
    expect(doc.tileset.get_tile_collision(10, 1) == 0, "(10, 1) collision must remain 0 after set_tile_collision");
    doc.tileset.init_tile_collisions(1);
    expect(doc.tileset.get_tile_collision(10, 1) == 0, "(10, 1) collision must remain 0 after init_tile_collisions(1)");
    doc.tileset.init_tile_collisions(0);
    expect(doc.tileset.get_tile_collision(10, 1) == 0, "(10, 1) collision must remain 0 after init_tile_collisions(0)");

    // 3. Collision grid generation produces zero collisions for empty map filled with (10, 1)
    CollisionGrid cg = doc.build_collision_grid();
    expect(cg.count_types_used() == 0, "empty map filled with (10, 1) must have zero collisions");

    // 4. Erasing tiles sets them back to tile (10, 1)
    doc.paint_cell(5, 5, TileMode::Stamp, 3, 2, 1);
    expect(!doc.get_cell(5, 5).is_empty(), "cell painted with stamp");
    expect(doc.get_cell(5, 5).atlas_x == 3 && doc.get_cell(5, 5).atlas_y == 2, "painted coords (3, 2)");

    doc.erase_cell(5, 5, 1);
    expect(doc.get_cell(5, 5).is_empty(), "erased cell must be empty");
    expect(doc.get_cell(5, 5).atlas_x == 10 && doc.get_cell(5, 5).atlas_y == 1,
           "erased cell must be reset to tile (10, 1)");

    // Erase rect
    doc.fill_rect({2, 2, 4, 4}, TileMode::Stamp, 6, 2);
    expect(!doc.get_cell(3, 3).is_empty(), "rect filled cell");
    doc.erase_rect({2, 2, 4, 4});
    expect(doc.get_cell(3, 3).is_empty(), "rect erased cell must be empty");
    expect(doc.get_cell(3, 3).atlas_x == 10 && doc.get_cell(3, 3).atlas_y == 1,
           "rect erased cell must be tile (10, 1)");

    // 5. Exported composite PNG blits tile (10, 1) into empty cells
    const std::string ts_path = temp_path("test_10_1_ts.png");
    expect(create_dummy_tileset_png(ts_path, 16), "create dummy tileset");
    expect(doc.tileset.load_from_file(ts_path), "load dummy tileset");

    const std::string out_png = temp_path("test_10_1_out.png");
    expect(export_composite_png(doc, out_png).empty(), "export composite png");

    Image im{};
    expect(load_png(out_png.c_str(), &im), "load exported image");
    expect(im.w == 320 && im.h == 224, "correct image dimensions");
    // Verify pixels were written (create_dummy_tileset_png writes index 1 into pixels)
    expect(im.px[0] == 1, "pixels from tileset tile (10, 1) were blitted to composite image");
    image_free(&im);
    std::remove(out_png.c_str());
    std::remove(ts_path.c_str());
}

void test_tileset_terrain_and_variants() {
    using namespace tmm;

    // 1. Basic variant manipulation on Tileset
    Tileset ts;
    ts.cols = 16;
    ts.rows = 4;
    ts.tile_size = 16;
    ts.pixels.assign(16 * 16 * 4 * 16, 1); // mock pixels

    expect(ts.set_variant(12, 0, 9, 2, 0.45f), "set variant (12, 0) -> (9, 2)");
    expect(ts.is_variant(12, 0), "tile (12, 0) is variant");
    expect(!ts.is_variant(9, 2), "tile (9, 2) is not variant");
    expect(ts.is_origin(9, 2), "tile (9, 2) is origin");
    expect(ts.count_variants_for_root(9, 2) == 1, "origin (9, 2) has 1 variant");

    const VariantBinding* vb = ts.find_variant(12, 0);
    expect(vb != nullptr && vb->root_x == 9 && vb->root_y == 2, "find_variant returns correct root");
    expect(std::abs(vb->probability - 0.45f) < 0.01f, "find_variant probability matches");

    // Remap to new origin
    expect(ts.set_variant(12, 0, 0, 3, 0.60f), "remap variant (12, 0) -> (0, 3)");
    expect(ts.count_variants_for_root(9, 2) == 0, "origin (9, 2) has 0 variants after remap");
    expect(ts.count_variants_for_root(0, 3) == 1, "origin (0, 3) has 1 variant after remap");

    // Setting tile as variant of itself unbinds it
    expect(!ts.set_variant(12, 0, 12, 0, 0.5f), "set_variant to self should fail and remove");
    expect(!ts.is_variant(12, 0), "tile (12, 0) is no longer a variant");

    // Strict 12x4 Origins and Extra Variants rules:
    expect(Tileset::is_base_origin_tile(0, 0), "0,0 is base origin");
    expect(Tileset::is_base_origin_tile(11, 3), "11,3 is base origin");
    expect(!Tileset::is_base_origin_tile(12, 0), "12,0 is not base origin");
    expect(!Tileset::is_base_origin_tile(0, 4), "0,4 is not base origin");
    expect(Tileset::is_variant_tile(12, 0), "12,0 is variant tile");
    expect(!Tileset::is_variant_tile(11, 0), "11,0 is not variant tile");

    // Base origin tiles cannot be assigned as variants:
    expect(!ts.set_variant(5, 2, 9, 2, 0.5f), "base 12x4 tile cannot be variant");
    // Extra tiles cannot be assigned as root origins:
    expect(!ts.set_variant(12, 0, 13, 0, 0.5f), "extra tile cannot be root origin");

    // 2. Auto-bind extra columns
    ts.auto_bind_extra_columns(9, 2, 0.75f);
    // Cols 12, 13, 14, 15 (4 cols * 4 rows = 16 variants)
    expect(ts.variants.size() == 16, "auto_bind_extra_columns bound 16 variants");
    expect(ts.count_variants_for_root(9, 2) == 16, "origin (9, 2) has 16 variants");

    expect(ts.remove_variant(12, 1), "remove variant (12, 1)");
    expect(ts.variants.size() == 15, "variant count decremented to 15");

    ts.clear_variants();
    expect(ts.variants.empty(), "clear_variants clears all variants");

    // 3. save_terrain_file and export_tileset_terrain
    const std::string ts_png = temp_path("test_terrain_save_ts.png");
    expect(create_dummy_tileset_png(ts_png, 16), "create dummy tileset for terrain save");
    expect(ts.load_from_file(ts_png), "load dummy tileset");

    ts.clear_variants();
    ts.set_variant(12, 0, 9, 2, 0.35f);
    ts.set_variant(12, 1, 1, 0, 0.50f);

    const std::string t_save_path = temp_path("test_saved.terrain");
    const std::string t_export_path = temp_path("test_exported.terrain");

    expect(save_terrain_file(ts, t_save_path).empty(), "save_terrain_file succeeded");
    expect(ts.terrain_path == t_save_path, "save_terrain_file updated ts.terrain_path");
    expect(export_tileset_terrain(ts, t_export_path).empty(), "export_tileset_terrain succeeded");

    // Load saved terrain into new Tileset and verify
    Tileset loaded_ts;
    expect(loaded_ts.load_terrain_file(t_save_path), "load_terrain_file loads saved terrain");
    expect(loaded_ts.variants.size() == 2, "loaded terrain has 2 variants");
    const VariantBinding* lv1 = loaded_ts.find_variant(12, 0);
    expect(lv1 != nullptr && lv1->root_x == 9 && lv1->root_y == 2, "loaded variant 1 mapping");
    expect(std::abs(lv1->probability - 0.35f) < 0.01f, "loaded variant 1 probability");
    const VariantBinding* lv2 = loaded_ts.find_variant(12, 1);
    expect(lv2 != nullptr && lv2->root_x == 1 && lv2->root_y == 0, "loaded variant 2 mapping");
    expect(std::abs(lv2->probability - 0.50f) < 0.01f, "loaded variant 2 probability");

    // Unified import: load_from_file handles .terrain directly
    Tileset unified_ts;
    expect(unified_ts.load_from_file(t_save_path), "load_from_file loads .terrain file");
    expect(unified_ts.variants.size() == 2, "unified load_from_file has 2 variants");

    std::remove(t_save_path.c_str());
    std::remove(t_export_path.c_str());
    std::remove(ts_png.c_str());

    // 4. Settings export settings defaults and persistence
    Settings s_def;
    expect(s_def.export_map_proj == true, "default export_map_proj is true");
    expect(s_def.export_col_json == false, "default export_col_json is false");
    expect(s_def.export_col_bin == false, "default export_col_bin is false");

    Settings s;
    s.export_map_proj = false;
    s.export_col_json = true;
    s.export_col_bin = true;
    s.export_tileset_png = false;
    s.export_tileset_proj = false;
    const std::string cfg_text = format_settings(s);
    expect(cfg_text.find("export_terrain=") == std::string::npos, "format_settings does not include export_terrain");
    expect(cfg_text.find("export_map_proj=false") != std::string::npos, "format_settings includes export_map_proj=false");
    expect(cfg_text.find("export_col_json=true") != std::string::npos, "format_settings includes export_col_json=true");
    expect(cfg_text.find("export_col_bin=true") != std::string::npos, "format_settings includes export_col_bin=true");
    expect(cfg_text.find("export_tileset_png=false") != std::string::npos, "format_settings includes export_tileset_png=false");

    Settings s2;
    expect(parse_settings_text(s2, cfg_text), "parse_settings_text succeeded");
    expect(s2.export_map_proj == false, "parsed export_map_proj is false");
    expect(s2.export_col_json == true, "parsed export_col_json is true");
    expect(s2.export_col_bin == true, "parsed export_col_bin is true");
    expect(s2.export_tileset_png == false, "parsed export_tileset_png is false");
    expect(s2.export_tileset_proj == false, "parsed export_tileset_proj is false");

    // 5. Live autotile re-evaluation on variant remapping
    TilemapDoc doc(10, 10, 16);
    const std::string ts_doc_png = temp_path("test_doc_ts.png");
    expect(create_dummy_tileset_png(ts_doc_png, 16, 14, 4), "create dummy tileset 14x4 for doc");
    expect(doc.tileset.load_from_file(ts_doc_png), "load doc tileset");
    doc.tileset.clear_variants();

    // Fill 3x3 block with terrain: center cell (2, 2) has 8 terrain neighbors -> root (9, 2)
    doc.fill_rect({1, 1, 3, 3}, TileMode::Terrain);
    expect(doc.get_cell(2, 2).atlas_x == 9 && doc.get_cell(2, 2).atlas_y == 2, "center cell initially root (9, 2)");

    // Bind (12, 2) as variant of (9, 2) with 10.0f weight, and set roll to 0.99f
    doc.tileset.set_variant(12, 2, 9, 2, 1.0f);
    MapCell mc = doc.get_cell(2, 2);
    mc.roll = 0.99f;
    doc.set_cell(2, 2, mc);
    doc.solve_all_autotiles();
    expect(doc.get_cell(2, 2).atlas_x == 12 && doc.get_cell(2, 2).atlas_y == 2, "center cell remapped to variant (12, 2)");

    // Clear variants and re-solve
    doc.tileset.clear_variants();
    doc.solve_all_autotiles();
    expect(doc.get_cell(2, 2).atlas_x == 9 && doc.get_cell(2, 2).atlas_y == 2, "center cell reverted to root (9, 2)");

    std::remove(ts_doc_png.c_str());
}

void test_tileset_preview_scaling() {
    using namespace tmm;

    // 1. Resizing sidebar changes preview tile scale (smaller when narrow, larger when wide)
    const float avail_sidebar_h = 700.0f;
    const int cols = 12;
    const int rows = 4;

    const TilesetPreviewLayout layout_narrow = compute_tileset_preview_layout(280.0f, avail_sidebar_h, cols, rows);
    const TilesetPreviewLayout layout_standard = compute_tileset_preview_layout(360.0f, avail_sidebar_h, cols, rows);
    const TilesetPreviewLayout layout_wide = compute_tileset_preview_layout(620.0f, avail_sidebar_h, cols, rows);

    expect(layout_standard.tile_ui_size > layout_narrow.tile_ui_size, "standard sidebar tile size > narrow sidebar");
    expect(layout_wide.tile_ui_size > layout_standard.tile_ui_size, "wide sidebar tile size > standard sidebar");

    // 2. Preview always fills the available horizontal space
    // inner_w = outer_w - 2 * border(1) - 2 * pad_x(4) = outer_w - 10
    const float expected_inner_w = 360.0f - 10.0f;
    expect(std::fabs(layout_standard.total_w - expected_inner_w) < 0.001f, "preview fills horizontal space available exactly");

    // 3. Aspect ratio preservation (12x4 tileset: height is roughly 1/3 of width)
    expect(layout_standard.total_h < layout_standard.total_w * 0.5f, "12x4 tileset total_h proportional to aspect ratio");
    expect(!layout_standard.needs_vscroll, "12x4 tileset does not need vertical scroll");
    expect(layout_standard.child_h == layout_standard.total_h + 10.0f, "child_h fits content tightly with no blank space");

    // 4. Large tileset like untitled.png (30 cols x 20 rows)
    const TilesetPreviewLayout layout_30x20 = compute_tileset_preview_layout(360.0f, avail_sidebar_h, 30, 20);
    expect(std::fabs(layout_30x20.total_w - expected_inner_w) < 0.001f, "30x20 tileset fills available width");
    expect(!layout_30x20.needs_vscroll, "30x20 tileset fits without vertical scrolling in normal window");

    // 5. Extremely tall tileset caps child_h and enables vertical scrolling
    const TilesetPreviewLayout layout_tall = compute_tileset_preview_layout(360.0f, avail_sidebar_h, 16, 64);
    expect(layout_tall.needs_vscroll, "extremely tall tileset enables vertical scrolling");
    expect(layout_tall.child_h <= 420.0f, "extremely tall tileset child_h is capped");
    // With scrollbar enabled, total_w fills the remaining width next to scrollbar
    const float expected_inner_with_vscroll = expected_inner_w - 14.0f;
    expect(std::fabs(layout_tall.total_w - expected_inner_with_vscroll) < 0.001f, "tall tileset fills width next to scrollbar");

    // 6. Zero / invalid dimensions do not crash or divide by zero
    const TilesetPreviewLayout layout_invalid = compute_tileset_preview_layout(0.0f, avail_sidebar_h, 0, 0);
    expect(layout_invalid.tile_ui_size == 0.0f, "invalid dimensions yield zero tile_ui_size safely");
}

void test_viewport_zoom_pan_math() {
    // Zoom around cursor calculation:
    // pan_new = mx - (mx - pan_old) * (new_zoom / old_zoom)
    auto zoom_around = [](float pan, float mx, float old_zoom, float new_zoom) -> float {
        return mx - (mx - pan) * (new_zoom / old_zoom);
    };

    const float old_zoom = 2.0f;
    const float new_zoom = 4.0f;
    const float cursor_x = 100.0f;
    const float old_pan_x = 20.0f;

    // Point in world/tileset space before zoom:
    // world_x = (cursor_x - old_pan_x) / old_zoom = (100 - 20) / 2.0 = 40.0
    // After zoom, screen pos = pan_new + world_x * new_zoom = cursor_x
    // pan_new = cursor_x - world_x * new_zoom = 100 - 40 * 4 = -60.0
    const float new_pan_x = zoom_around(old_pan_x, cursor_x, old_zoom, new_zoom);
    expect(std::abs(new_pan_x - (-60.0f)) < 0.001f, "zoom_around cursor keeps point under cursor stationary");

    // Clamp range [0.25f, 16.0f]
    const float min_clamped = std::clamp(0.1f, 0.25f, 16.0f);
    const float max_clamped = std::clamp(20.0f, 0.25f, 16.0f);
    expect(min_clamped == 0.25f, "min zoom clamps to 0.25f (25%)");
    expect(max_clamped == 16.0f, "max zoom clamps to 16.0f (1600%)");

    // Pinch scaling preserves aspect and direction
    const float pinch_in = std::clamp(2.0f * 0.8f, 0.25f, 16.0f);
    const float pinch_out = std::clamp(2.0f * 1.5f, 0.25f, 16.0f);
    expect(pinch_in == 1.6f, "pinch in scales zoom down");
    expect(pinch_out == 3.0f, "pinch out scales zoom up");
}

void test_tileset_maker_integration() {
    using namespace tmm;

    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();

    // 1. Theme application and background colors
    {
        const Rgb dark_bg = background_clear_color(true);
        const Rgb light_bg = background_clear_color(false);
        expect(dark_bg.r < 50 && dark_bg.g < 50 && dark_bg.b < 50, "dark clear color is dark");
        expect(light_bg.r > 200 && light_bg.g > 200 && light_bg.b > 200, "light clear color is light");
        apply_theme(true, 1.0f);
        apply_theme(false, 1.25f);
    }

    // 2. TilesetEditor embedded initialization and reset_new at Step 1
    {
        tsm::TilesetEditor ed;
        ed.embedded = true;
        ed.reset_new("dungeon_wall", 16);
        expect(ed.embedded, "embedded flag is preserved across reset_new");
        expect(ed.step == tsm::Step::Center, "new tileset starts at Step 1 (Center tile)");
        expect(ed.doc.tile_size == 16, "tile size defaults to 16");
        expect(std::string(ed.project_name) == "dungeon_wall", "project name matches");

        // Paint center tile
        ed.doc.set_pixel(tsm::TilesetDoc::kCenter.x, tsm::TilesetDoc::kCenter.y, 4, 4, 1);
        expect(ed.doc.get_pixel(tsm::TilesetDoc::kCenter.x, tsm::TilesetDoc::kCenter.y, 4, 4) == 1, "center tile painted");

        // Ensure atlas auto-generates 12x4 from Step 1
        const std::string err = ed.ensure_atlas();
        expect(err.empty(), "ensure_atlas succeeds from step 1");
        expect(ed.has_atlas, "has_atlas set to true");
        expect(ed.atlas.cols >= 12, "atlas has 12 columns");
    }

    // 3. In-memory AtlasDoc <-> Tileset synchronization
    {
        tsm::AtlasDoc atlas(16);
        atlas.reset(16, 13);
        atlas.apply_palette({tsm::Rgb{10, 20, 30}, tsm::Rgb{200, 210, 220}});
        atlas.set_pixel(9, 2, 0, 0, 1);
        atlas.set_pixel(9, 2, 15, 15, 1);
        atlas.add_variant({9, 2}, 0.40f);

        Tileset ts;
        ts.cols = atlas.cols;
        ts.rows = atlas.rows;
        ts.tile_size = atlas.tile_size;
        ts.palette.clear();
        for (const auto& c : atlas.palette) {
            ts.palette.push_back(Rgb{c.r, c.g, c.b});
        }
        const int img_w = ts.cols * ts.tile_size;
        const int img_h = ts.rows * ts.tile_size;
        ts.pixels.assign(static_cast<size_t>(img_w * img_h), 0);
        for (int row = 0; row < ts.rows; ++row) {
            for (int col = 0; col < ts.cols; ++col) {
                const auto tile = atlas.get_tile(col, row);
                for (int y = 0; y < ts.tile_size; ++y) {
                    for (int x = 0; x < ts.tile_size; ++x) {
                        const int dest = (row * ts.tile_size + y) * img_w + (col * ts.tile_size + x);
                        const int src = y * ts.tile_size + x;
                        ts.pixels[static_cast<size_t>(dest)] = (src < static_cast<int>(tile.size())) ? tile[static_cast<size_t>(src)] : 0;
                    }
                }
            }
        }
        ts.variants.clear();
        for (const auto& b : atlas.bindings) {
            VariantBinding vb;
            vb.x = b.x;
            vb.y = b.y;
            vb.root_x = b.root_x;
            vb.root_y = b.root_y;
            vb.probability = b.probability;
            ts.variants.push_back(vb);
        }

        expect(ts.is_valid(), "synced tileset is valid");
        expect(ts.cols == 12, "synced tileset has 12 cols");
        expect(ts.rows == 6, "synced tileset has 6 rows (4 base + 1 slope + 1 variant)");
        expect(ts.tile_size == 16, "synced tileset has 16px tiles");
        expect(ts.palette.size() == 2, "palette has 2 colors");
        expect(ts.palette[0] == Rgb{10, 20, 30}, "palette color 0 matches");
        expect(ts.variants.size() == 1, "variant count matches");
        expect(ts.variants[0].x == 0 && ts.variants[0].y == 5, "variant placed in bottom variant row 5");
        expect(ts.variants[0].root_x == 9 && ts.variants[0].root_y == 2, "variant root matches");
        expect(std::abs(ts.variants[0].probability - 0.40f) < 0.01f, "variant probability matches");

        const int px_idx = (2 * 16 + 0) * img_w + (9 * 16 + 0);
        expect(ts.pixels[static_cast<size_t>(px_idx)] == 1, "synced pixel at (0,0) of tile (9,2) is 1");
    }

    // 4. Companion file automatic discovery and import
    {
        tsm::AtlasDoc src_atlas(16);
        src_atlas.reset(16, 13);
        src_atlas.apply_palette({tsm::Rgb{0, 0, 0}, tsm::Rgb{255, 255, 255}, tsm::Rgb{120, 150, 180}});
        src_atlas.add_variant({9, 2}, 0.35f);

        const std::string png_file = temp_path("test_auto_companion.png");
        const std::string terr_file = temp_path("test_auto_companion.terrain");

        std::string err = tsm::save_atlas_png(src_atlas, png_file);
        expect(err.empty(), "save_atlas_png succeeded");
        err = tsm::save_terrain(src_atlas, terr_file, png_file);
        expect(err.empty(), "save_terrain succeeded");

        // Test A: Import by PNG path -> should automatically find companion .terrain file
        tsm::AtlasDoc imported_atlas(16);
        std::string loaded_png;
        bool had_terrain = false;
        err = tsm::import_12x4_tileset(imported_atlas, png_file, loaded_png, had_terrain);
        expect(err.empty(), "import_12x4_tileset by PNG succeeded");
        expect(had_terrain, "companion .terrain file automatically discovered and loaded");
        expect(imported_atlas.bindings.size() == 1, "variant loaded from companion terrain");
        expect(imported_atlas.bindings[0].root_x == 9 && imported_atlas.bindings[0].root_y == 2, "variant root is (9, 2)");
        expect(std::abs(imported_atlas.bindings[0].probability - 0.35f) < 0.01f, "variant probability matches");

        // Test B: Import by .terrain path -> should automatically find companion PNG file
        tsm::AtlasDoc imported_atlas_b(16);
        loaded_png.clear();
        had_terrain = false;
        err = tsm::import_12x4_tileset(imported_atlas_b, terr_file, loaded_png, had_terrain);
        expect(err.empty(), "import_12x4_tileset by .terrain succeeded");
        expect(had_terrain, "terrain loaded");
        expect(!loaded_png.empty(), "companion PNG file automatically discovered and loaded");
        expect(imported_atlas_b.bindings.size() == 1, "variant loaded from companion terrain");

        std::remove(png_file.c_str());
        std::remove(terr_file.c_str());
    }

    // 5. Embedded TilesetMaker UI autosizing to fit available height without overflow
    {
        tsm::TilesetEditor ed;
        ed.embedded = true;
        ed.reset_new("autosize_test", 16);

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1000.0f, 700.0f);
        unsigned char* font_pixels = nullptr;
        int font_w = 0, font_h = 0;
        io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_w, &font_h);
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1000.0f, 700.0f));
        ImGui::Begin("TestFitWindow", nullptr, ImGuiWindowFlags_NoScrollbar);

        const float target_avail_h = 550.0f;
        const float cursor_before = ImGui::GetCursorPosY();
        ed.draw_content(nullptr, nullptr, target_avail_h);
        const float cursor_after = ImGui::GetCursorPosY();
        const float total_drawn_h = cursor_after - cursor_before;

        // Total drawn height should match target_avail_h within 4px (no overflow!)
        expect(std::abs(total_drawn_h - target_avail_h) <= 4.0f, "draw_content height respects avail_height without overflow");
        expect(ed.zoom >= 1, "tileset editor zoom is positive and fits");

        // Verify zoom adjusts when smaller height is given
        const int zoom_large = ed.zoom;
        ImGui::End();
        ImGui::Render();

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(800.0f, 400.0f));
        ImGui::Begin("TestFitSmall", nullptr, ImGuiWindowFlags_NoScrollbar);
        ed.draw_content(nullptr, nullptr, 250.0f);
        expect(ed.zoom < zoom_large, "tileset editor zoom scales down to fit smaller height");
        ImGui::End();
        ImGui::Render();
    }

    ImGui::DestroyContext(ctx);
}

void test_ui_scale_and_sync() {
    using namespace tmm;

    // 1. Clamping in settings parser
    Settings s_low;
    expect(parse_settings_text(s_low, "scale = 0.4\n"), "parse scale 0.4");
    expect(std::abs(s_low.scale - 0.75f) < 0.001f, "scale clamps to min 0.75");

    Settings s_high;
    expect(parse_settings_text(s_high, "scale = 3.5\n"), "parse scale 3.5");
    expect(std::abs(s_high.scale - 2.0f) < 0.001f, "scale clamps to max 2.0");

    Settings s_valid;
    expect(parse_settings_text(s_valid, "scale = 1.35\n"), "parse scale 1.35");
    expect(std::abs(s_valid.scale - 1.35f) < 0.001f, "valid scale parses correctly");

    // 2. apply_theme font and layout scaling
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);

    apply_theme(true, 1.5f);
    expect(std::abs(ImGui::GetStyle().FontScaleMain - 1.5f) < 0.001f, "apply_theme sets FontScaleMain to 1.5");
    expect(ImGui::GetStyle().WindowPadding.x > 10.0f, "apply_theme scales window padding");

    apply_theme(false, 0.85f);
    expect(std::abs(ImGui::GetStyle().FontScaleMain - 0.85f) < 0.001f, "apply_theme sets FontScaleMain to 0.85");

    ImGui::DestroyContext(ctx);

    // 3. Synchronization between tmm::Settings and tsm::Settings
    tsm::Settings tsm_s;
    tsm_s.scale = 1.6f;
    tsm_s.dark = true;

    const std::string tsm_file = temp_path("test_tsm_sync_settings.cfg");
    const std::string tmm_file = temp_path("test_tmm_sync_settings.cfg");

    expect(tsm::save_settings_file(tsm_s, tsm_file), "save tsm settings file");

    Settings tmm_s;
    tsm::Settings tsm_loaded;
    expect(tsm::load_settings_file(tsm_loaded, tsm_file), "load tsm settings file");
    tmm_s.scale = tsm_loaded.scale;
    tmm_s.dark = tsm_loaded.dark;
    expect(save_settings_file(tmm_s, tmm_file), "save tmm settings file");

    Settings tmm_verify;
    expect(load_settings_file(tmm_verify, tmm_file), "load tmm settings file");
    expect(std::abs(tmm_verify.scale - 1.6f) < 0.001f, "tmm and tsm share scale 1.6f");
    expect(tmm_verify.dark == true, "tmm and tsm share dark theme");

    std::remove(tsm_file.c_str());
    std::remove(tmm_file.c_str());
}

void test_new_map_empty_tileset() {
    using namespace tmm;
    const std::string png = temp_path("test_new_map_ts.png");
    expect(create_dummy_tileset_png(png, 16, 13, 4), "create dummy tileset for new map test");

    TilemapDoc doc(20, 14, 16);
    expect(doc.tileset.load_from_file(png), "load tileset into doc");
    doc.tileset.set_variant(12, 0, 9, 2, 0.5f);
    doc.tileset.terrain_path = "some_path.terrain";
    expect(doc.tileset.is_valid(), "tileset should be valid");
    expect(!doc.tileset.variants.empty(), "tileset has variants");
    expect(!doc.tileset.png_path.empty(), "png path set");
    expect(!doc.tileset.terrain_path.empty(), "terrain path set");

    // Test that default reset_8px keeps the tileset intact
    doc.reset_8px(40, 28, 16);
    expect(doc.tileset.is_valid(), "reset_8px should retain existing tileset");
    expect(!doc.tileset.variants.empty(), "reset_8px should retain variants");

    // Test clear_tileset resets tileset and terrain completely
    doc.clear_tileset();
    expect(!doc.tileset.is_valid(), "clear_tileset should make tileset invalid/empty");
    expect(doc.tileset.pixels.empty(), "clear_tileset should clear pixels");
    expect(doc.tileset.palette.empty(), "clear_tileset should clear palette");
    expect(doc.tileset.variants.empty(), "clear_tileset should clear variants");
    expect(doc.tileset.png_path.empty(), "clear_tileset should clear png_path");
    expect(doc.tileset.terrain_path.empty(), "clear_tileset should clear terrain_path");
    expect(doc.tileset.tile_size == doc.tile_size, "clear_tileset should set tile_size to match doc");
    expect(doc.tileset.get_tile_collision(10, 1) == 0, "empty tile (10, 1) should have 0 collision");
    expect(doc.tileset.get_tile_collision(0, 0) == 1, "default tile (0, 0) should have 1 collision");

    // Test embedded TilesetEditor reset_new
    tsm::TilesetEditor ed;
    ed.reset_new("tileset", 16);
    expect(ed.step == tsm::Step::Center, "fresh tileset editor should be at Step::Center");
    expect(ed.doc.tile_size == 16, "tileset editor should have matching tile size");
    expect(std::string(ed.project_name) == "tileset", "tileset editor project name should be 'tileset'");

    std::remove(png.c_str());
}

void test_tilesetproj_import() {
    using namespace tmm;
    std::string proj_path = temp_path("test_project.tilesetproj");

    tsm::TilesetEditor ed;
    ed.reset_new("my_tileset", 16);
    ed.doc.set_pixel(0, 0, 0, 0, 1);
    ed.project_path = proj_path;
    std::string save_err = ed.save_project(false);
    expect(save_err.empty(), "save_project should succeed");

    tsm::ProjectData data;
    std::string err = tsm::load_project(data, proj_path);
    expect(err.empty(), "load_project should succeed");
    expect(data.name == "my_tileset", "project name should match");

    tsm::TilesetEditor ed2;
    ed2.doc.restore(data.tileset);
    if (data.has_atlas) ed2.atlas.restore(data.atlas);
    else ed2.atlas.reset(data.tileset.tile_size);
    ed2.has_atlas = data.has_atlas;
    expect(ed2.ensure_atlas().empty(), "ensure_atlas should succeed");

    tmm::Tileset ts;
    ts.cols = ed2.atlas.cols;
    ts.rows = tsm::AtlasDoc::kRows;
    ts.tile_size = ed2.atlas.tile_size;
    expect(ts.tile_size == 16, "tile size should be 16");
    expect(ts.cols == 12, "atlas cols should be 12");
    expect(ts.rows == 4, "atlas rows should be 4");

    std::remove(proj_path.c_str());
}

void test_tileset_export() {
    using namespace tmm;
    const std::string dummy_png = temp_path("test_export_orig.png");
    expect(create_dummy_tileset_png(dummy_png, 16, 12, 4), "create dummy 12x4 tileset");

    Tileset ts;
    expect(ts.load_from_file(dummy_png), "load dummy tileset for export test");
    ts.palette[1] = Rgb{255, 128, 64};
    ts.pixels[0] = 1;
    ts.set_variant(12, 0, 9, 2, 0.45f);

    // 1. Test export_tileset_png
    const std::string exported_png = temp_path("test_exported_ts.png");
    std::string err = export_tileset_png(ts, exported_png);
    expect(err.empty(), "export_tileset_png should succeed");

    Tileset reloaded_ts;
    expect(reloaded_ts.load_from_file(exported_png), "load exported tileset PNG");
    expect(reloaded_ts.cols == 12, "reloaded ts cols match");
    expect(reloaded_ts.rows == 4, "reloaded ts rows match");
    expect(reloaded_ts.tile_size == 16, "reloaded ts tile_size matches");
    expect(reloaded_ts.palette.size() >= 2, "reloaded ts palette size");
    expect(reloaded_ts.palette[1].r == 255 && reloaded_ts.palette[1].g == 128 && reloaded_ts.palette[1].b == 64, "reloaded ts palette color preserved");
    expect(reloaded_ts.pixels[0] == 1, "reloaded ts pixel preserved");

    // 2. Test export_tileset_terrain with image name override
    const std::string exported_terr = temp_path("test_exported_ts.terrain");
    err = export_tileset_terrain(ts, exported_terr, "custom_name.png");
    expect(err.empty(), "export_tileset_terrain with override should succeed");

    std::string terr_text;
    expect(read_text_file(exported_terr, terr_text), "read exported terrain file");
    expect(terr_text.find("\"tileset\": \"custom_name.png\"") != std::string::npos, "terrain file has overridden tileset image name");

    // 3. Test save_map_json with tileset override
    TilemapDoc doc(20, 14, 16);
    doc.tileset = ts;
    const std::string map_json_path = temp_path("test_exported_map.json");
    err = save_map_json(doc, map_json_path, "my_tileset.png");
    expect(err.empty(), "save_map_json with tileset override should succeed");

    std::string map_text;
    expect(read_text_file(map_json_path, map_text), "read exported map json");
    expect(map_text.find("\"tileset\": \"my_tileset.png\"") != std::string::npos, "map json references companion tileset");

    // 4. Test tileset project export & reload
    tsm::TilesetEditor ed;
    ed.reset_new("exported_project", 16);
    const std::string proj_path = temp_path("test_exported_project.tilesetproj");
    ed.project_path = proj_path;
    err = ed.save_project(false);
    expect(err.empty(), "saving tilesetproj should succeed");

    tsm::ProjectData pdata;
    err = tsm::load_project(pdata, proj_path);
    expect(err.empty(), "loading exported tilesetproj should succeed");
    expect(pdata.name == "exported_project", "project name matches in loaded tilesetproj");
    expect(pdata.tileset.tile_size == 16, "tile_size matches in loaded tilesetproj");

    // 5. Test Settings persistence for export_tileset_png and export_tileset_proj
    Settings s;
    s.export_tileset_png = true;
    s.export_tileset_proj = true;
    std::string s_text = format_settings(s);
    expect(s_text.find("export_tileset_png=true") != std::string::npos, "settings text formats export_tileset_png");
    expect(s_text.find("export_tileset_proj=true") != std::string::npos, "settings text formats export_tileset_proj");

    Settings s_parsed;
    expect(parse_settings_text(s_parsed, "export_tileset_png=false\nexport_tileset_proj=false\n"), "parse settings text");
    expect(!s_parsed.export_tileset_png, "parsed export_tileset_png is false");
    expect(!s_parsed.export_tileset_proj, "parsed export_tileset_proj is false");

    // Cleanup
    std::remove(dummy_png.c_str());
    std::remove(exported_png.c_str());
    std::remove(exported_terr.c_str());
    std::remove(map_json_path.c_str());
    std::remove(proj_path.c_str());
}

void test_zip_export() {
    using namespace tmm;

    // 1. Settings persistence for export_zip
    Settings s;
#ifdef __EMSCRIPTEN__
    expect(s.export_zip, "default export_zip should be true on WASM");
#else
    expect(!s.export_zip, "default export_zip should be false on native desktop");
#endif
    s.export_zip = true;
    std::string s_text = format_settings(s);
    expect(s_text.find("export_zip=true") != std::string::npos, "settings text formats export_zip=true");

    Settings s_parsed;
    expect(parse_settings_text(s_parsed, "export_zip=true\n"), "parse export_zip=true");
    expect(s_parsed.export_zip, "parsed export_zip is true");

    expect(parse_settings_text(s_parsed, "export_zip=false\n"), "parse export_zip=false");
    expect(!s_parsed.export_zip, "parsed export_zip is false");

    // 2. In-memory ZipWriter basic tests
    ZipWriter mem_zip;
    std::string hello = "Hello World! This is a test file for ZIP export.\n";
    std::vector<uint8_t> hello_bytes(hello.begin(), hello.end());
    mem_zip.add_file("sub/hello.txt", hello_bytes);

    std::string json_data = "{\"name\": \"test_map\", \"width\": 20, \"height\": 14}";
    std::vector<uint8_t> json_bytes(json_data.begin(), json_data.end());
    mem_zip.add_file("map.json", json_bytes);

    expect(mem_zip.file_count() == 2, "mem_zip file count is 2");
    expect(mem_zip.finalize(), "mem_zip finalize");

    const std::vector<uint8_t>& zip_buf = mem_zip.buffer();
    expect(zip_buf.size() > 50, "zip_buf has valid size");
    // Verify PK header
    expect(zip_buf[0] == 'P' && zip_buf[1] == 'K' && zip_buf[2] == 0x03 && zip_buf[3] == 0x04, "valid local header signature");

    // 3. File packing from disk and write_to_file
    const std::string tmp_txt = temp_path("tmm_test_file.txt");
    const std::string tmp_bin = temp_path("tmm_test_file.bin");
    const std::string tmp_zip = temp_path("tmm_test_archive.zip");

    {
        std::ofstream f(tmp_txt);
        f << "Text file contents to pack into zip.";
    }
    {
        std::ofstream f(tmp_bin, std::ios::binary);
        for (int i = 0; i < 256; ++i) {
            uint8_t b = static_cast<uint8_t>(i);
            f.write(reinterpret_cast<char*>(&b), 1);
        }
    }

    ZipWriter disk_zip;
    expect(disk_zip.add_file_from_disk("file.txt", tmp_txt), "add_file_from_disk txt");
    expect(disk_zip.add_file_from_disk("binary.dat", tmp_bin), "add_file_from_disk bin");
    expect(!disk_zip.add_file_from_disk("bad.xyz", temp_path("non_existent_file.xyz")), "add non-existent file fails");
    expect(disk_zip.file_count() == 2, "disk_zip file count is 2");

    expect(disk_zip.write_to_file(tmp_zip), "write zip archive to disk");

    // Verify written file exists and has size
    std::ifstream z_in(tmp_zip, std::ios::binary | std::ios::ate);
    expect(z_in.is_open(), "zip file opened for verification");
    std::streamsize z_size = z_in.tellg();
    expect(z_size > 80, "written zip file has non-trivial size");
    z_in.seekg(0);
    char magic[4];
    z_in.read(magic, 4);
    expect(magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4, "magic PK 03 04 present in written file");
    z_in.close();

    // 4. End-to-end multi-file export bundling simulation (map PNG, collision JSON, tileset PNG)
    const std::string exp_png = temp_path("bundle_map.png");
    const std::string exp_col = temp_path("bundle_map_collisions.json");
    const std::string exp_ts_png = temp_path("bundle_ts.png");
    const std::string bundle_zip = temp_path("bundle_all.zip");

    create_dummy_tileset_png(exp_ts_png, 16);

    TilemapDoc doc(16, 12, 16);
    expect(doc.tileset.load_from_file(exp_ts_png), "load tileset for doc");
    expect(export_composite_png(doc, exp_png).empty(), "export composite png for bundle");
    expect(export_mde_collision_json(doc, exp_col).empty(), "export collision json for bundle");

    ZipWriter bundle;
    expect(bundle.add_file_from_disk("bundle_map.png", exp_png), "bundle add map.png");
    expect(bundle.add_file_from_disk("bundle_map_collisions.json", exp_col), "bundle add col.json");
    expect(bundle.add_file_from_disk("bundle_ts.png", exp_ts_png), "bundle add ts.png");
    expect(bundle.file_count() == 3, "bundle file count is 3");
    expect(bundle.write_to_file(bundle_zip), "bundle write to file");

    std::ifstream b_in(bundle_zip, std::ios::binary | std::ios::ate);
    expect(b_in.is_open(), "bundle zip opened");
    expect(b_in.tellg() > 200, "bundle zip size > 200 bytes");
    b_in.close();

    // Cleanup
    std::remove(tmp_txt.c_str());
    std::remove(tmp_bin.c_str());
    std::remove(tmp_zip.c_str());
    std::remove(exp_png.c_str());
    std::remove(exp_col.c_str());
    std::remove(exp_ts_png.c_str());
    std::remove(bundle_zip.c_str());
}

void test_zip_reader() {
    using namespace tmm;
    const std::string zpath = temp_path("reader_test.zip");

    // 1. Create a zip file using ZipWriter
    ZipWriter writer;
    const std::string text_content = "Hello, Tilemap Maker tmproj format!";
    expect(writer.add_file("message.txt", text_content), "writer add message.txt");

    std::vector<uint8_t> bin_content = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0xAA, 0xFF};
    expect(writer.add_file("binary.dat", bin_content), "writer add binary.dat");

    // Add a compressible string to test DEFLATE
    std::string large_text(2000, 'A');
    for (size_t i = 0; i < large_text.size(); i += 2) large_text[i] = 'B';
    expect(writer.add_file("large.txt", large_text), "writer add large.txt");

    expect(writer.write_to_file(zpath), "writer write_to_file");

    // 2. Open using ZipReader
    ZipReader reader;
    expect(reader.open_from_file(zpath), "reader open_from_file");
    expect(reader.is_open(), "reader is_open is true");
    expect(reader.file_count() == 3, "reader file_count is 3");
    expect(reader.has_file("message.txt"), "has message.txt");
    expect(reader.has_file("binary.dat"), "has binary.dat");
    expect(reader.has_file("large.txt"), "has large.txt");
    expect(!reader.has_file("nonexistent.txt"), "does not have nonexistent.txt");

    // 3. Test extraction
    std::string extracted_text;
    expect(reader.extract_to_text("message.txt", extracted_text), "extract message.txt");
    expect(extracted_text == text_content, "message.txt content matches");

    std::vector<uint8_t> extracted_bin;
    expect(reader.extract_to_buffer("binary.dat", extracted_bin), "extract binary.dat");
    expect(extracted_bin == bin_content, "binary.dat content matches");

    std::string extracted_large;
    expect(reader.extract_to_text("large.txt", extracted_large), "extract large.txt (deflated)");
    expect(extracted_large == large_text, "large.txt content matches");

    // 4. Test opening from memory
    std::ifstream zfile(zpath, std::ios::binary);
    std::vector<uint8_t> zmem((std::istreambuf_iterator<char>(zfile)), std::istreambuf_iterator<char>());
    zfile.close();

    ZipReader mem_reader;
    expect(mem_reader.open_from_memory(zmem), "reader open_from_memory");
    expect(mem_reader.file_count() == 3, "mem_reader file_count is 3");
    std::string mem_text;
    expect(mem_reader.extract_to_text("message.txt", mem_text), "mem extract text");
    expect(mem_text == text_content, "mem extracted text matches");

    // 5. Test invalid zip data
    ZipReader bad_reader;
    std::vector<uint8_t> bad_data = {1, 2, 3, 4, 5};
    expect(!bad_reader.open_from_memory(bad_data), "bad data fails gracefully");

    reader.close();
    expect(!reader.is_open(), "reader closed");

    std::remove(zpath.c_str());
}

void test_map_project_tmproj() {
    using namespace tmm;
    const std::string ts_png_path = temp_path("tmproj_test_ts.png");
    const std::string tmproj_path = temp_path("test_project.tmproj");

    create_dummy_tileset_png(ts_png_path, 16, 12, 4);

    TilemapDoc doc(20, 15, 16);
    doc.name = "dungeon_zone";
    doc.origin_x = 4;
    doc.origin_y = -2;
    doc.buffer = 2;
    expect(doc.tileset.load_from_file(ts_png_path), "load tileset for test map");

    // Set custom collision types
    doc.collision_types.clear();
    doc.collision_types.push_back(CollisionType{1, "SolidWall", {255, 0, 0}});
    doc.collision_types.push_back(CollisionType{2, "WaterHazard", {0, 100, 255}});
    doc.collision_types.push_back(CollisionType{3, "LadderRung", {255, 200, 50}});

    // Set custom tileset collisions
    doc.tileset.tile_collisions.assign(doc.tileset.cols * doc.tileset.rows, 0);
    doc.tileset.set_tile_collision(1, 1, 1); // SolidWall
    doc.tileset.set_tile_collision(2, 1, 2); // WaterHazard
    doc.tileset.set_tile_collision(3, 1, 3); // LadderRung

    // Paint cells
    MapCell c1;
    c1.mode = TileMode::Terrain;
    c1.atlas_x = 1;
    c1.atlas_y = 1;
    c1.roll = 0.42f;
    doc.set_cell(5, 5, c1);

    MapCell c2;
    c2.mode = TileMode::Stamp;
    c2.atlas_x = 3;
    c2.atlas_y = 1;
    doc.set_cell(6, 5, c2);

    // Create a mock tileset project text
    tsm::ProjectData ts_data;
    ts_data.name = "dungeon_tileset";
    ts_data.tile_mode = true;
    ts_data.tileset.tile_size = 16;
    const std::string mock_ts_proj_text = tsm::project_to_text(ts_data);

    // 1. Save map project as .tmproj
    std::string err = save_map_project(doc, tmproj_path, mock_ts_proj_text);
    expect(err.empty(), "save_map_project succeeds");

    // 2. Inspect ZIP archive contents
    ZipReader zip;
    expect(zip.open_from_file(tmproj_path), "open .tmproj as zip");
    expect(zip.has_file("map.json"), ".tmproj contains map.json");
    expect(zip.has_file("collisions.json"), ".tmproj contains collisions.json");
    expect(zip.has_file("tileset.tilesetproj"), ".tmproj contains tileset.tilesetproj");
    expect(zip.has_file("tileset.png"), ".tmproj contains tileset.png");

    // Check collisions.json content
    std::string col_json_text;
    expect(zip.extract_to_text("collisions.json", col_json_text), "extract collisions.json");
    expect(col_json_text.find("\"collision_types\"") != std::string::npos, "collisions.json has collision_types");
    expect(col_json_text.find("\"SolidWall\"") != std::string::npos, "collisions.json has SolidWall");
    expect(col_json_text.find("\"WaterHazard\"") != std::string::npos, "collisions.json has WaterHazard");
    expect(col_json_text.find("\"LadderRung\"") != std::string::npos, "collisions.json has LadderRung");
    expect(col_json_text.find("\"tileset_collisions\"") != std::string::npos, "collisions.json has tileset_collisions");

    // Check map.json content
    std::string map_json_text;
    expect(zip.extract_to_text("map.json", map_json_text), "extract map.json");
    expect(map_json_text.find("\"dungeon_zone\"") != std::string::npos, "map.json has doc name");
    expect(map_json_text.find("\"cells\"") != std::string::npos, "map.json has cells");

    zip.close();

    // 3. Load map project into a fresh TilemapDoc
    TilemapDoc loaded_doc;
    std::string loaded_ts_text;
    err = load_map_project(loaded_doc, tmproj_path, &loaded_ts_text);
    expect(err.empty(), "load_map_project succeeds");

    expect(loaded_doc.name == "dungeon_zone", "loaded doc name matches");
    expect(loaded_doc.width == 20, "loaded doc width matches");
    expect(loaded_doc.height == 15, "loaded doc height matches");
    expect(loaded_doc.tile_size == 16, "loaded doc tile_size matches");
    expect(loaded_doc.origin_x == 4, "loaded doc origin_x matches");
    expect(loaded_doc.origin_y == -2, "loaded doc origin_y matches");
    expect(loaded_doc.buffer == 2, "loaded doc buffer matches");

    // Verify collision types
    expect(loaded_doc.collision_types.size() == 3, "loaded collision types count is 3");
    expect(loaded_doc.collision_types[0].name == "SolidWall", "first collision type name is SolidWall");
    expect(loaded_doc.collision_types[1].name == "WaterHazard", "second collision type name is WaterHazard");
    expect(loaded_doc.collision_types[2].name == "LadderRung", "third collision type name is LadderRung");

    // Verify tileset collisions
    expect(!loaded_doc.tileset.tile_collisions.empty(), "loaded tileset collisions not empty");
    expect(loaded_doc.tileset.get_tile_collision(1, 1) == 1, "loaded tile collision (1,1) is 1");
    expect(loaded_doc.tileset.get_tile_collision(2, 1) == 2, "loaded tile collision (2,1) is 2");
    expect(loaded_doc.tileset.get_tile_collision(3, 1) == 3, "loaded tile collision (3,1) is 3");

    // Verify cell data
    const MapCell& lc1 = loaded_doc.get_cell(5, 5);
    expect(lc1.mode == TileMode::Terrain, "loaded cell (5,5) is Terrain");
    expect(lc1.atlas_x == 0 && lc1.atlas_y == 3, "loaded cell (5,5) solved autotile coords");
    expect(std::abs(lc1.roll - 0.42f) < 0.001f, "loaded cell (5,5) roll");

    const MapCell& lc2 = loaded_doc.get_cell(6, 5);
    expect(lc2.mode == TileMode::Stamp, "loaded cell (6,5) is Stamp");
    expect(lc2.atlas_x == 3 && lc2.atlas_y == 1, "loaded cell (6,5) coords");

    // Verify tileset project text
    expect(loaded_ts_text == mock_ts_proj_text, "extracted tileset project text matches");

    // 4. Test error handling on non-zip or corrupt file
    const std::string dummy_bad = temp_path("not_a_zip.tmproj");
    write_text_file(dummy_bad, "This is not a zip file");
    TilemapDoc bad_doc;
    std::string bad_err = load_map_project(bad_doc, dummy_bad);
    expect(!bad_err.empty(), "load_map_project on non-zip returns error");

    // Cleanup
    std::remove(ts_png_path.c_str());
    std::remove(tmproj_path.c_str());
    std::remove(dummy_bad.c_str());
}

static void test_slopes_and_layout() {
    using namespace tmm;
    // 1. Layout checks: Row 4 reserved for slopes, Row 5+ for variants
    {
        expect(Tileset::kSlopeRow == 4, "Slope row is strictly row 4");
        expect(Tileset::kVariantStartRow == 5, "Variant start row is 5");

        // Base origin tiles (0..11, 0..3)
        expect(Tileset::is_base_origin_tile(0, 0), "(0,0) is base origin");
        expect(Tileset::is_base_origin_tile(11, 3), "(11,3) is base origin");
        expect(!Tileset::is_base_origin_tile(0, 4), "(0,4) is NOT base origin");
        expect(!Tileset::is_base_origin_tile(0, 5), "(0,5) is NOT base origin");

        // Slope tiles (0..11, 4)
        expect(Tileset::is_slope_tile(0, 4), "(0,4) is slope tile");
        expect(Tileset::is_slope_tile(11, 4), "(11,4) is slope tile");
        expect(!Tileset::is_slope_tile(12, 4), "(12,4) is not slope tile");
        expect(!Tileset::is_slope_tile(0, 3), "(0,3) is not slope tile");
        expect(!Tileset::is_slope_tile(0, 5), "(0,5) is not slope tile");

        // Bottom variant tiles (row 5+)
        expect(Tileset::is_variant_tile(0, 5), "(0,5) is variant tile");
        expect(Tileset::is_variant_tile(11, 6), "(11,6) is variant tile");
        expect(!Tileset::is_variant_tile(0, 4), "(0,4) is NOT variant tile");

        // ensure_slope_row
        Tileset ts;
        ts.cols = 12;
        ts.rows = 4;
        ts.tile_size = 16;
        ts.pixels.resize(12 * 4 * 16 * 16, 0);
        expect(!ts.has_slope_row(), "4-row tileset has no slope row initially");
        ts.ensure_slope_row();
        expect(ts.has_slope_row(), "after ensure_slope_row, tileset has slope row");
        expect(ts.rows == 5, "tileset rows grew to 5");
    }

    // 2. Slope type inference and painting (1x1 and 2x1)
    {
        TilemapDoc doc;
        doc.reset(20, 20, 16);
        doc.tileset.cols = 12;
        doc.tileset.rows = 5;
        doc.tileset.tile_size = 16;
        doc.tileset.pixels.resize(12 * 5 * 16 * 16, 0);

        // A. Incline Floor 1x1: dragging up-right (dx=1, dy=-1)
        SlopeType st = doc.infer_slope_type(5, 5, SlopeSize::Slope1x1, 1, -1, false);
        expect(st == SlopeType::FloorIncline1x1, "dx=1, dy=-1 infers FloorIncline1x1");
        expect(slope_is_floor(st), "FloorIncline is floor slope");
        expect(slope_is_incline(st), "FloorIncline is incline");

        // B. Decline Floor 1x1: dragging down-right (dx=1, dy=1)
        st = doc.infer_slope_type(5, 5, SlopeSize::Slope1x1, 1, 1, false);
        expect(st == SlopeType::FloorDecline1x1, "dx=1, dy=1 infers FloorDecline1x1");
        expect(slope_is_floor(st), "FloorDecline is floor slope");
        expect(!slope_is_incline(st), "FloorDecline is not incline");

        // C. Flip toggle
        st = doc.infer_slope_type(5, 5, SlopeSize::Slope1x1, 1, -1, true);
        expect(st == SlopeType::FloorDecline1x1, "flip toggles FloorIncline to FloorDecline");

        // D. Adjacency inference:
        // Solid ground on left (x-1, y) leads to a declining ramp (\)
        doc.paint_cell(4, 5, TileMode::Terrain);
        st = doc.infer_slope_type(5, 5, SlopeSize::Slope1x1, 0, 0, false);
        expect(st == SlopeType::FloorDecline1x1, "solid ground neighbor on left infers decline ramp");

        // Solid ground on right (x+1, y) leads to an incline ramp (/)
        doc.erase_cell(4, 5);
        doc.paint_cell(6, 5, TileMode::Terrain);
        st = doc.infer_slope_type(5, 5, SlopeSize::Slope1x1, 0, 0, false);
        expect(st == SlopeType::FloorIncline1x1, "solid ground neighbor on right infers incline ramp");

        // Paint 1x1 slope with auto-fill dirt at (5, 5) - connects to solid ground on right
        doc.paint_slope(5, 5, SlopeSize::Slope1x1, true, 0, 0, false);
        expect(doc.is_slope(5, 5), "cell (5,5) is slope");
        const MapCell& c55 = doc.get_cell(5, 5);
        expect(c55.mode == TileMode::Slope, "cell (5,5) mode is Slope");
        expect(c55.atlas_x == 0 && c55.atlas_y == 4, "FloorIncline1x1 maps to tile (0, 4)");

        // Check auto-fill dirt placed at (5, 6)
        const MapCell& c56 = doc.get_cell(5, 6);
        expect(c56.mode == TileMode::Terrain, "dirt placed at (5,6) beneath floor slope");

        // E. Paint 2x1 slope at (8, 5)
        doc.paint_slope(8, 5, SlopeSize::Slope2x1, false, 1, -1, false);
        expect(doc.is_slope(8, 5), "(8,5) is slope");
        expect(doc.is_slope(9, 5), "(9,5) is partner slope");
        const MapCell& c8 = doc.get_cell(8, 5);
        const MapCell& c9 = doc.get_cell(9, 5);
        expect(c8.atlas_x == 4 && c8.atlas_y == 4, "first half of 2x1 Floor Incline is (4, 4)");
        expect(c9.atlas_x == 5 && c9.atlas_y == 4, "second half of 2x1 Floor Incline is (5, 4)");

        // Erase 2x1 slope
        doc.erase_slope(8, 5);
        expect(!doc.is_slope(8, 5), "(8,5) erased");
        expect(!doc.is_slope(9, 5), "partner (9,5) also erased");
    }

    // 3. Terrain flushness against slopes (is_solid_for_terrain)
    {
        TilemapDoc doc;
        doc.reset(10, 10, 16);
        // Paint FloorIncline (/) at (5, 5)
        doc.paint_slope_explicit(5, 5, SlopeType::FloorIncline1x1, false);

        // FloorIncline (/) is solid on bottom face: from (5, 6) looking at target (5, 5)
        expect(doc.is_solid_for_terrain(5, 5, 5, 6), "slope bottom edge is solid to neighbor below");
        // FloorIncline (/) is solid on right (high) face: from (6, 5) looking at target (5, 5)
        expect(doc.is_solid_for_terrain(5, 5, 6, 5), "slope right high edge is solid to neighbor on right");
        // FloorIncline (/) is open on top face: from (5, 4) looking at target (5, 5)
        expect(!doc.is_solid_for_terrain(5, 5, 5, 4), "slope top edge is open");
        // FloorIncline (/) is low on left face: from (4, 5) looking at target (5, 5)
        expect(!doc.is_solid_for_terrain(5, 5, 4, 5), "slope left low edge is open");
    }

    // 4. Save and load map JSON with slopes
    {
        TilemapDoc doc;
        doc.reset(10, 10, 16);
        doc.name = "SlopeMap";
        doc.paint_slope_explicit(3, 3, SlopeType::FloorIncline1x1, false);
        doc.paint_slope_explicit(6, 6, SlopeType::FloorDecline2x1, false);

        const std::string test_json_path = temp_path("test_slope_map.json");
        std::string save_err = save_map_json(doc, test_json_path);
        expect(save_err.empty(), "save_map_json succeeded");

        // Verify JSON content has "slope" mode
        std::ifstream ifs(test_json_path);
        std::string json_str((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        expect(json_str.find("\"mode\": \"slope\"") != std::string::npos || json_str.find("\"mode\":\"slope\"") != std::string::npos, "serialized JSON contains slope mode");

        TilemapDoc loaded;
        const std::string err = load_map_json(loaded, test_json_path);
        expect(err.empty(), "loading map JSON succeeded");
        expect(loaded.is_slope(3, 3), "loaded map has slope at (3, 3)");
        expect(loaded.is_slope(6, 6), "loaded map has slope at (6, 6)");
        expect(loaded.is_slope(7, 6), "loaded map has 2x1 slope partner at (7, 6)");

        std::remove(test_json_path.c_str());
    }
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
    test_buffer_and_outside_zone();
    test_empty_background_tile_10_1();
    test_tileset_terrain_and_variants();
    test_tileset_preview_scaling();
    test_viewport_zoom_pan_math();
    test_tileset_maker_integration();
    test_ui_scale_and_sync();
    test_new_map_empty_tileset();
    test_tilesetproj_import();
    test_tileset_export();
    test_zip_export();
    test_zip_reader();
    test_map_project_tmproj();
    test_slopes_and_layout();

    if (g_fails) {
        std::cerr << g_fails << " test(s) failed\n";
        return 1;
    }
    std::cout << "self-test: ok\n";
    return 0;
}
