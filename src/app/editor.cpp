#include "editor.h"
#include "settings.h"
#include "theme.h"

#include "core/collision.h"
#include "core/io.h"
#include "core/tilemap_doc.h"
#include "core/tileset.h"
#include "core/types.h"

#include "../deps/tileset-maker-thingy/src/app/tileset_editor.h"
#include "../deps/tileset-maker-thingy/src/core/convert.h"
#include "../deps/tileset-maker-thingy/src/core/io.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "nfd.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace tmm {

enum class AppView { Tilemap, TilesetMaker };
enum class Tool { Paint, Line, Erase, Rect, Fill, Select, Eyedropper };
enum class TilesetSidebarMode { Stamp, Collision, Variants };

struct EditorState {
    AppView current_view = AppView::Tilemap;
    tsm::TilesetEditor tileset_editor;

    TilemapDoc doc;
    Settings settings;

    TilesetSidebarMode tileset_mode = TilesetSidebarMode::Stamp;
    uint8_t active_collision_type = 1;
    Cell hovered_col_tile = {-1, -1};
    Cell last_col_painted = {-1, -1};

    // Tileset Terrain Variant State
    Cell selected_terrain_tile = {Tileset::kDefaultCenterCol, Tileset::kDefaultCenterRow};
    Cell hovered_terrain_tile = {-1, -1};
    float terrain_variant_prob = 0.30f;

    Tool tool = Tool::Paint;
    TileMode paint_mode = TileMode::Terrain;
    int stamp_col = 9;
    int stamp_row = 2;
    int brush_size = 1;
    bool rect_fill = true;
    bool rect_circle = false;

    // Viewport
    float zoom = 2.0f;
    ImVec2 pan = ImVec2(80.0f, 60.0f);
    bool is_panning = false;
    ImVec2 pan_drag_start = ImVec2(0, 0);
    ImVec2 pan_start_offset = ImVec2(0, 0);
    float pinch_scale = 1.0f;
    bool has_pinch = false;

    // Sidebar sizing
    float sidebar_w = 380.0f;

    // Canvas tools interaction
    bool is_drawing = false;
    Cell drag_start = {0, 0};
    Cell last_mouse_cell = {-1, -1};
    Cell last_painted_cell = {-1, -1};
    bool right_click_erasing = false;
    std::vector<ImVec2> stroke_points;
    std::vector<ImVec2> pending_mouse_moves;

    // Selection
    bool has_selection = false;
    Rect selection = {0, 0, 0, 0};
    bool select_circle = false;
    bool selection_is_circle = false;
    bool selection_lifted = false;
    Rect selection_origin = {0, 0, 0, 0};
    Clipboard floating_clip;
    Clipboard clipboard;
    bool paste_mode = false;
    Cell paste_pos = {0, 0};
    bool is_moving_selection = false;
    Rect selection_drag_origin = {0, 0, 0, 0};
    bool is_moving_paste = false;
    Cell paste_drag_origin = {0, 0};

    // Modals
    bool show_new_modal = false;
    char new_name[128] = "level_1";
    int new_w = 40;
    int new_h = 28;
    int new_tile_size = 16;

    bool show_resize_modal = false;
    int resize_w = 40;
    int resize_h = 28;
    int resize_anchor_x = -1; // -1: left, 0: center, 1: right
    int resize_anchor_y = -1; // -1: top, 0: center, 1: bottom

    bool show_export_modal = false;
    char export_folder[512] = "";
    bool export_png = true;
    bool export_col_json = true;
    bool export_col_bin = true;
    bool export_map_json = true;
    bool export_terrain = true;

    std::string status_msg = "Ready.";
    std::string current_map_path;
    Cell hovered_cell = {-1, -1};

    // GPU Texture
    SDL_Texture* tileset_texture = nullptr;
    int texture_w = 0;
    int texture_h = 0;
};

static EditorState g_ed;

static void apply_moved_selection();
static void deselect();
static void cancel_or_deselect();
static void commit_paste();
static void cancel_paste();

static void update_tileset_texture(SDL_Renderer* renderer) {
    if (g_ed.tileset_texture) {
        SDL_DestroyTexture(g_ed.tileset_texture);
        g_ed.tileset_texture = nullptr;
    }
    if (!g_ed.doc.tileset.is_valid()) {
        return;
    }

    const int w = g_ed.doc.tileset.image_width();
    const int h = g_ed.doc.tileset.image_height();
    if (w <= 0 || h <= 0) return;

    g_ed.tileset_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!g_ed.tileset_texture) return;

    SDL_SetTextureScaleMode(g_ed.tileset_texture, SDL_SCALEMODE_NEAREST);

    // Build RGBA pixel buffer
    std::vector<uint32_t> rgba(static_cast<size_t>(w * h), 0);
    const auto& pal = g_ed.doc.tileset.palette;
    const auto& px = g_ed.doc.tileset.pixels;

    for (size_t i = 0; i < px.size() && i < rgba.size(); ++i) {
        const uint8_t idx = px[i];
        if (idx < pal.size()) {
            const Rgb c = pal[idx];
            // 0xAABBGGRR
            rgba[i] = 0xFF000000u | (static_cast<uint32_t>(c.b) << 16) | (static_cast<uint32_t>(c.g) << 8) | static_cast<uint32_t>(c.r);
        } else {
            rgba[i] = 0xFF000000u;
        }
    }

    SDL_UpdateTexture(g_ed.tileset_texture, nullptr, rgba.data(), w * sizeof(uint32_t));
    g_ed.texture_w = w;
    g_ed.texture_h = h;
}

static bool file_exists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream f(path.c_str(), std::ios::binary);
    return f.good();
}

static void sync_atlas_to_tileset(const tsm::AtlasDoc& atlas, Tileset& tileset) {
    tileset.cols = atlas.cols;
    tileset.rows = tsm::AtlasDoc::kRows;
    tileset.tile_size = atlas.tile_size;
    tileset.palette.clear();
    for (const auto& c : atlas.palette) {
        tileset.palette.push_back(Rgb{c.r, c.g, c.b});
    }
    const int img_w = tileset.cols * tileset.tile_size;
    const int img_h = tileset.rows * tileset.tile_size;
    tileset.pixels.assign(static_cast<size_t>(img_w * img_h), 0);
    for (int row = 0; row < tileset.rows; ++row) {
        for (int col = 0; col < tileset.cols; ++col) {
            const auto tile = atlas.get_tile(col, row);
            for (int y = 0; y < tileset.tile_size; ++y) {
                for (int x = 0; x < tileset.tile_size; ++x) {
                    const int dest = (row * tileset.tile_size + y) * img_w + (col * tileset.tile_size + x);
                    const int src = y * tileset.tile_size + x;
                    tileset.pixels[static_cast<size_t>(dest)] = (src < static_cast<int>(tile.size())) ? tile[static_cast<size_t>(src)] : 0;
                }
            }
        }
    }
    tileset.variants.clear();
    for (const auto& b : atlas.bindings) {
        VariantBinding vb;
        vb.x = b.x;
        vb.y = b.y;
        vb.root_x = b.root_x;
        vb.root_y = b.root_y;
        vb.probability = b.probability;
        tileset.variants.push_back(vb);
    }
    if (tileset.tile_collisions.size() != static_cast<size_t>(tileset.cols * tileset.rows)) {
        tileset.init_tile_collisions(1);
    }
}

static void sync_tileset_to_atlas(const Tileset& tileset, tsm::AtlasDoc& atlas) {
    atlas.reset(tileset.tile_size, tileset.cols);
    std::vector<tsm::Rgb> pal;
    for (const auto& c : tileset.palette) {
        pal.push_back(tsm::Rgb{c.r, c.g, c.b});
    }
    atlas.apply_palette(pal);
    const int img_w = tileset.image_width();
    for (int row = 0; row < tileset.rows; ++row) {
        for (int col = 0; col < tileset.cols; ++col) {
            std::vector<uint8_t> tile(static_cast<size_t>(tileset.tile_size * tileset.tile_size), 0);
            for (int y = 0; y < tileset.tile_size; ++y) {
                for (int x = 0; x < tileset.tile_size; ++x) {
                    const int src = (row * tileset.tile_size + y) * img_w + (col * tileset.tile_size + x);
                    const int dest = y * tileset.tile_size + x;
                    if (src < static_cast<int>(tileset.pixels.size())) {
                        tile[static_cast<size_t>(dest)] = tileset.pixels[static_cast<size_t>(src)];
                    }
                }
            }
            atlas.set_tile(col, row, tile);
        }
    }
    atlas.bindings.clear();
    for (const auto& b : tileset.variants) {
        tsm::VariantBinding vb;
        vb.x = b.x;
        vb.y = b.y;
        vb.root_x = b.root_x;
        vb.root_y = b.root_y;
        vb.probability = b.probability;
        atlas.bindings.push_back(vb);
    }
    atlas.painted = true;
}

static void switch_to_view(AppView target, SDL_Renderer* renderer) {
    if (g_ed.current_view == target) return;

    if (target == AppView::TilesetMaker) {
        g_ed.tileset_editor.embedded = true;
        g_ed.tileset_editor.settings.dark = g_ed.settings.dark;
        g_ed.tileset_editor.settings.scale = g_ed.settings.scale;

        // If no current tileset, start tileset maker thingy as a new tileset at step 1
        if (!g_ed.doc.tileset.is_valid()) {
            g_ed.tileset_editor.reset_new("tileset", g_ed.doc.tile_size);
            g_ed.tileset_editor.step = tsm::Step::Center;
            g_ed.tileset_editor.configure_view();
            g_ed.status_msg = "Tileset Maker: started new tileset at Step 1 (Center tile).";
        } else {
            // If there is an existing tileset, import the PNG or terrain (which automatically finds companions)
            bool imported = false;
            std::string path_to_import;
            if (!g_ed.doc.tileset.terrain_path.empty() && file_exists(g_ed.doc.tileset.terrain_path)) {
                path_to_import = g_ed.doc.tileset.terrain_path;
            } else if (!g_ed.doc.tileset.png_path.empty() && file_exists(g_ed.doc.tileset.png_path)) {
                path_to_import = g_ed.doc.tileset.png_path;
            }

            if (!path_to_import.empty()) {
                imported = g_ed.tileset_editor.import_12x4(path_to_import);
            }

            if (!imported) {
                // In-memory fallback if file doesn't exist on disk or import returned false
                sync_tileset_to_atlas(g_ed.doc.tileset, g_ed.tileset_editor.atlas);
                tsm::convert_atlas_to_tileset(g_ed.tileset_editor.atlas, g_ed.tileset_editor.doc);
                g_ed.tileset_editor.step = tsm::Step::Variants;
                g_ed.tileset_editor.has_atlas = true;
                g_ed.tileset_editor.ui.project_open = true;
                g_ed.tileset_editor.configure_view();
            }
            g_ed.status_msg = "Tileset Maker: loaded tileset.";
        }
        g_ed.current_view = AppView::TilesetMaker;
    } else if (target == AppView::Tilemap) {
        // Returning to tilemap view
        std::string ensure_err = g_ed.tileset_editor.ensure_atlas();
        if (ensure_err.empty()) {
            const int new_ts = g_ed.tileset_editor.doc.tile_size;
            if (g_ed.doc.tile_size != new_ts && (new_ts == 8 || new_ts == 16)) {
                const int w8 = g_ed.doc.width_8px();
                const int h8 = g_ed.doc.height_8px();
                g_ed.doc.reset_8px(w8, h8, new_ts);
            }
            sync_atlas_to_tileset(g_ed.tileset_editor.atlas, g_ed.doc.tileset);
            if (!g_ed.tileset_editor.last_exported_png_path.empty()) {
                g_ed.doc.tileset.png_path = g_ed.tileset_editor.last_exported_png_path;
            }
            if (!g_ed.tileset_editor.last_exported_terrain_path.empty()) {
                g_ed.doc.tileset.terrain_path = g_ed.tileset_editor.last_exported_terrain_path;
            }
            update_tileset_texture(renderer);
            g_ed.doc.solve_all_autotiles();
            g_ed.status_msg = "Returned to Tilemap Maker (tileset synced).";
        } else {
            g_ed.status_msg = "Returned to Tilemap Maker.";
        }
        g_ed.current_view = AppView::Tilemap;
    }
}

static void apply_app_theme(bool dark) {
    tmm::apply_theme(dark, g_ed.settings.scale);
    g_ed.tileset_editor.settings.dark = dark;
    g_ed.tileset_editor.settings.scale = g_ed.settings.scale;
}

static SDL_Window* s_window = nullptr;

static bool window_rect_visible(int x, int y, int w, int h) {
    int n = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&n);
    if (!displays || n <= 0) {
        return false;
    }
    bool ok = false;
    for (int i = 0; i < n; ++i) {
        SDL_Rect b{};
        if (!SDL_GetDisplayUsableBounds(displays[i], &b)) {
            continue;
        }
        const int probe_x = x + w / 2;
        const int probe_y = y + 16;
        if (probe_x >= b.x && probe_x < b.x + b.w && probe_y >= b.y && probe_y < b.y + b.h) {
            ok = true;
            break;
        }
    }
    SDL_free(displays);
    return ok;
}

static void persist_settings(SDL_Window* window = nullptr) {
    SDL_Window* target_win = window ? window : s_window;
    if (target_win) {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(target_win);
        if (!(flags & SDL_WINDOW_MINIMIZED)) {
            g_ed.settings.window_placed = true;
            g_ed.settings.window_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
            if (!g_ed.settings.window_maximized) {
                SDL_GetWindowPosition(target_win, &g_ed.settings.window_x, &g_ed.settings.window_y);
                SDL_GetWindowSize(target_win, &g_ed.settings.window_w, &g_ed.settings.window_h);
            }
        }
    }
    g_ed.settings.sidebar_w = g_ed.sidebar_w;
    g_ed.settings.zoom = g_ed.zoom;
    g_ed.settings.brush_size = 1;
    g_ed.settings.paint_mode = 0; // Default to Terrain mode
    if (!g_ed.current_map_path.empty()) {
        g_ed.settings.last_map_path = g_ed.current_map_path;
    }
    if (std::strlen(g_ed.export_folder) > 0) {
        g_ed.settings.last_export_dir = g_ed.export_folder;
    }
    ensure_config_dir();
    save_settings_file(g_ed.settings, settings_path());
}

static void open_tileset_dialog(SDL_Renderer* renderer) {
    nfdu8filteritem_t filters[3] = {
        {"Tileset Files (*.png, *.terrain)", "png,terrain"},
        {"PNG Images (*.png)", "png"},
        {"Terrain Files (*.terrain)", "terrain"}
    };
    nfdu8char_t* out_path = nullptr;
    nfdresult_t res = NFD_OpenDialogU8(&out_path, filters, 3, nullptr);
    if (res == NFD_OKAY && out_path) {
        if (g_ed.doc.tileset.load_from_file(out_path)) {
            const int old_ts = g_ed.doc.tile_size;
            const int new_ts = g_ed.doc.tileset.tile_size;
            if (old_ts != new_ts) {
                const int w8 = g_ed.doc.width_8px();
                const int h8 = g_ed.doc.height_8px();
                g_ed.doc.tile_size = new_ts;
                g_ed.doc.resize_8px(w8, h8);
            }
            g_ed.doc.solve_all_autotiles();
            update_tileset_texture(renderer);
            if (!g_ed.doc.tileset.variants.empty()) {
                g_ed.status_msg = "Loaded tileset (" + std::to_string(g_ed.doc.tileset.variants.size()) +
                                  " variants): " + std::string(out_path);
            } else {
                g_ed.status_msg = "Loaded tileset: " + std::string(out_path);
            }
            g_ed.settings.last_tileset_path = out_path;
            persist_settings();
        } else {
            g_ed.status_msg = "Error loading tileset: " + g_ed.doc.tileset.error;
        }
        NFD_FreePathU8(out_path);
    }
}

static void open_map_dialog(SDL_Renderer* renderer) {
    nfdu8filteritem_t filters[2] = {{"Map JSON", "json"}, {"All Files", "*"}};
    nfdu8char_t* out_path = nullptr;
    nfdresult_t res = NFD_OpenDialogU8(&out_path, filters, 2, nullptr);
    if (res == NFD_OKAY && out_path) {
        std::string err = load_map_json(g_ed.doc, out_path);
        if (err.empty()) {
            g_ed.current_map_path = out_path;
            g_ed.settings.last_map_path = out_path;
            persist_settings();
            g_ed.status_msg = "Opened map: " + std::string(out_path);
            update_tileset_texture(renderer);
        } else {
            g_ed.status_msg = "Error opening map: " + err;
        }
        NFD_FreePathU8(out_path);
    }
}

static void save_map_dialog() {
    if (g_ed.selection_lifted) apply_moved_selection();
    if (g_ed.paste_mode) commit_paste();

    if (!g_ed.current_map_path.empty()) {
        std::string err = save_map_json(g_ed.doc, g_ed.current_map_path);
        if (err.empty()) {
            g_ed.status_msg = "Map saved.";
            g_ed.doc.clear_dirty();
            g_ed.settings.last_map_path = g_ed.current_map_path;
            persist_settings();
            return;
        }
    }
    nfdu8filteritem_t filters[2] = {{"Map JSON", "json"}, {"All Files", "*"}};
    nfdu8char_t* out_path = nullptr;
    nfdresult_t res = NFD_SaveDialogU8(&out_path, filters, 2, nullptr, (g_ed.doc.name + ".json").c_str());
    if (res == NFD_OKAY && out_path) {
        std::string path = out_path;
        if (path.size() < 5 || path.substr(path.size() - 5) != ".json") {
            path += ".json";
        }
        std::string err = save_map_json(g_ed.doc, path);
        if (err.empty()) {
            g_ed.current_map_path = path;
            g_ed.settings.last_map_path = path;
            persist_settings();
            g_ed.status_msg = "Saved map to " + path;
            g_ed.doc.clear_dirty();
        } else {
            g_ed.status_msg = "Error saving map: " + err;
        }
        NFD_FreePathU8(out_path);
    }
}

static void execute_export() {
    if (g_ed.selection_lifted) apply_moved_selection();
    if (g_ed.paste_mode) commit_paste();

    const char* default_dir = (std::strlen(g_ed.export_folder) > 0)
        ? g_ed.export_folder
        : (!g_ed.settings.last_export_dir.empty() ? g_ed.settings.last_export_dir.c_str() : nullptr);

    nfdu8filteritem_t filters[2] = {{"PNG Image", "png"}, {"All Files", "*"}};
    nfdu8char_t* out_path = nullptr;
    const std::string def_name = g_ed.doc.name.empty() ? "map.png" : (g_ed.doc.name + ".png");

    nfdresult_t res = NFD_SaveDialogU8(&out_path, filters, 2, default_dir, def_name.c_str());
    if (res != NFD_OKAY || !out_path) {
        if (res == NFD_ERROR) {
            g_ed.status_msg = "Export dialog error: " + std::string(NFD_GetError());
        }
        return;
    }

    std::string chosen_png = out_path;
    NFD_FreePathU8(out_path);

    if (chosen_png.size() < 4 || (chosen_png.substr(chosen_png.size() - 4) != ".png" && chosen_png.substr(chosen_png.size() - 4) != ".PNG")) {
        chosen_png += ".png";
    }

    std::string dir;
    std::string stem;
    const size_t last_slash = chosen_png.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        dir = chosen_png.substr(0, last_slash);
        const std::string filename = chosen_png.substr(last_slash + 1);
        stem = filename.substr(0, filename.size() - 4);
    } else {
        dir = ".";
        stem = chosen_png.substr(0, chosen_png.size() - 4);
    }

    std::snprintf(g_ed.export_folder, sizeof(g_ed.export_folder), "%s", dir.c_str());
    g_ed.settings.last_export_dir = dir;
    persist_settings();
    if (!stem.empty()) {
        g_ed.doc.name = stem;
    }

    const std::string prefix = dir + "/" + stem;
    std::vector<std::string> saved_files;

    // Composite PNG is always exported
    {
        const std::string p = prefix + ".png";
        std::string err = export_composite_png(g_ed.doc, p);
        if (!err.empty()) {
            g_ed.status_msg = "PNG export failed: " + err;
            return;
        }
        saved_files.push_back(stem + ".png");
    }
    if (g_ed.export_col_json) {
        const std::string p = prefix + "_collisions.json";
        std::string err = export_mde_collision_json(g_ed.doc, p);
        if (!err.empty()) {
            g_ed.status_msg = "Collision JSON export failed: " + err;
            return;
        }
        saved_files.push_back(stem + "_collisions.json");
    }
    if (g_ed.export_col_bin) {
        const CollisionGrid grid = g_ed.doc.build_collision_grid();
        if (grid.count_types_used() <= 1) {
            const std::string p = prefix + "_col.bin";
            std::string err = export_collision_bin(g_ed.doc, p);
            if (!err.empty()) {
                g_ed.status_msg = "Collision BIN export failed: " + err;
                return;
            }
            saved_files.push_back(stem + "_col.bin");
        }
    }
    if (g_ed.export_map_json) {
        const std::string p = prefix + ".json";
        std::string err = save_map_json(g_ed.doc, p);
        if (!err.empty()) {
            g_ed.status_msg = "Map JSON export failed: " + err;
            return;
        }
        saved_files.push_back(stem + ".json");
    }
    if (g_ed.export_terrain) {
        const std::string p = prefix + ".terrain";
        std::string err = export_tileset_terrain(g_ed.doc.tileset, p);
        if (!err.empty()) {
            g_ed.status_msg = "Terrain export failed: " + err;
            return;
        }
        saved_files.push_back(stem + ".terrain");
    }

    std::string msg = "Export complete: ";
    for (size_t i = 0; i < saved_files.size(); ++i) {
        if (i > 0) msg += ", ";
        msg += saved_files[i];
    }
    g_ed.status_msg = msg;
    g_ed.show_export_modal = false;
}

static bool is_cell_in_ellipse(int x, int y, const Rect& rect) {
    if (rect.w <= 0 || rect.h <= 0) return false;
    const float cx = static_cast<float>(rect.x) + static_cast<float>(rect.w) * 0.5f;
    const float cy = static_cast<float>(rect.y) + static_cast<float>(rect.h) * 0.5f;
    const float rx = std::max(0.5f, static_cast<float>(rect.w) * 0.5f);
    const float ry = std::max(0.5f, static_cast<float>(rect.h) * 0.5f);
    const float px = static_cast<float>(x) + 0.5f;
    const float py = static_cast<float>(y) + 0.5f;
    const float dx = (px - cx) / rx;
    const float dy = (py - cy) / ry;
    return (dx * dx + dy * dy) <= 1.0f;
}

static bool is_cell_in_outline_ellipse(int x, int y, const Rect& rect, int brush_size) {
    if (!is_cell_in_ellipse(x, y, rect)) return false;
    brush_size = std::max(brush_size, 1);
    const int inner_w = rect.w - 2 * brush_size;
    const int inner_h = rect.h - 2 * brush_size;
    if (inner_w <= 0 || inner_h <= 0) return true;
    const Rect inner_rect{rect.x + brush_size, rect.y + brush_size, inner_w, inner_h};
    return !is_cell_in_ellipse(x, y, inner_rect);
}

static Rect compute_drag_rect(int start_x, int start_y, int curr_x, int curr_y, bool square) {
    const int min_x = -g_ed.doc.buffer;
    const int min_y = -g_ed.doc.buffer;
    const int max_x = g_ed.doc.width + g_ed.doc.buffer - 1;
    const int max_y = g_ed.doc.height + g_ed.doc.buffer - 1;
    start_x = std::clamp(start_x, min_x, max_x);
    start_y = std::clamp(start_y, min_y, max_y);
    const int dx = curr_x - start_x;
    const int dy = curr_y - start_y;
    const int sx = (dx >= 0) ? 1 : -1;
    const int sy = (dy >= 0) ? 1 : -1;
    if (square) {
        const int max_side_x = (sx >= 0) ? (max_x - start_x) : (start_x - min_x);
        const int max_side_y = (sy >= 0) ? (max_y - start_y) : (start_y - min_y);
        int side = std::max(std::abs(dx), std::abs(dy));
        side = std::min(side, std::min(max_side_x, max_side_y));
        const int target_x = start_x + sx * side;
        const int target_y = start_y + sy * side;
        const int rx = std::min(start_x, target_x);
        const int ry = std::min(start_y, target_y);
        return {rx, ry, side + 1, side + 1};
    } else {
        const int cx_clamped = std::clamp(curr_x, min_x, max_x);
        const int cy_clamped = std::clamp(curr_y, min_y, max_y);
        const int rx = std::min(start_x, cx_clamped);
        const int ry = std::min(start_y, cy_clamped);
        const int rx2 = std::max(start_x, cx_clamped);
        const int ry2 = std::max(start_y, cy_clamped);
        return {rx, ry, rx2 - rx + 1, ry2 - ry + 1};
    }
}

static void apply_moved_selection() {
    if (!g_ed.has_selection) return;
    if (g_ed.selection_lifted) {
        g_ed.doc.paste_clipboard(g_ed.selection.x, g_ed.selection.y, g_ed.floating_clip);
        g_ed.doc.end_stroke();
        g_ed.selection_lifted = false;
        g_ed.floating_clip.clear();
        g_ed.is_moving_selection = false;
        g_ed.status_msg = "Applied moved selection.";
    }
}

static void cancel_paste() {
    if (!g_ed.paste_mode) return;
    g_ed.paste_mode = false;
    g_ed.has_selection = false;
    g_ed.selection_is_circle = false;
    g_ed.doc.set_clip_rect(nullptr);
    g_ed.is_moving_paste = false;
    g_ed.status_msg = "Paste cancelled.";
}

static void commit_paste() {
    if (!g_ed.paste_mode || g_ed.clipboard.is_empty()) return;
    g_ed.doc.paste_clipboard(g_ed.paste_pos.x, g_ed.paste_pos.y, g_ed.clipboard);
    g_ed.paste_mode = false;
    g_ed.has_selection = true;
    g_ed.selection_is_circle = false;
    g_ed.selection = {g_ed.paste_pos.x, g_ed.paste_pos.y, g_ed.clipboard.w, g_ed.clipboard.h};
    g_ed.doc.set_clip_rect(&g_ed.selection, TilemapDoc::ClipShape::Rect);
    g_ed.is_moving_paste = false;
    g_ed.status_msg = "Pasted clipboard contents.";
}

static void deselect() {
    if (g_ed.paste_mode) {
        commit_paste();
    }
    if (g_ed.has_selection) {
        apply_moved_selection();
        g_ed.has_selection = false;
        g_ed.selection_is_circle = false;
        g_ed.selection_lifted = false;
        g_ed.is_moving_selection = false;
        g_ed.floating_clip.clear();
        g_ed.doc.set_clip_rect(nullptr);
        g_ed.status_msg = "Deselected.";
    }
}

static void cancel_or_deselect() {
    if (g_ed.paste_mode) {
        cancel_paste();
        return;
    }
    if (g_ed.selection_lifted) {
        g_ed.doc.cancel_stroke();
        g_ed.selection = g_ed.selection_origin;
        g_ed.selection_lifted = false;
        g_ed.floating_clip.clear();
        g_ed.is_moving_selection = false;
        g_ed.has_selection = false;
        g_ed.selection_is_circle = false;
        g_ed.doc.set_clip_rect(nullptr);
        g_ed.status_msg = "Move cancelled.";
    } else if (g_ed.has_selection) {
        g_ed.has_selection = false;
        g_ed.selection_is_circle = false;
        g_ed.doc.set_clip_rect(nullptr);
        g_ed.status_msg = "Deselected.";
    }
}

static void cut_selection() {
    if (!g_ed.has_selection || g_ed.paste_mode) return;
    if (g_ed.selection_lifted) {
        g_ed.clipboard = g_ed.floating_clip;
        g_ed.floating_clip.clear();
        g_ed.doc.end_stroke();
        g_ed.selection_lifted = false;
        g_ed.has_selection = false;
        g_ed.selection_is_circle = false;
        g_ed.doc.set_clip_rect(nullptr);
    } else if (g_ed.selection_is_circle) {
        g_ed.doc.cut_ellipse(g_ed.selection, g_ed.clipboard);
        g_ed.has_selection = false;
        g_ed.selection_is_circle = false;
        g_ed.doc.set_clip_rect(nullptr);
    } else {
        g_ed.doc.cut_rect(g_ed.selection, g_ed.clipboard);
        g_ed.has_selection = false;
        g_ed.selection_is_circle = false;
        g_ed.doc.set_clip_rect(nullptr);
    }
    g_ed.status_msg = "Cut selection.";
}

static void copy_selection() {
    if (!g_ed.has_selection || g_ed.paste_mode) return;
    if (g_ed.selection_lifted) {
        g_ed.clipboard = g_ed.floating_clip;
    } else if (g_ed.selection_is_circle) {
        g_ed.clipboard = g_ed.doc.copy_ellipse(g_ed.selection);
    } else {
        g_ed.clipboard = g_ed.doc.copy_rect(g_ed.selection);
    }
    g_ed.status_msg = "Copied selection.";
}

static void delete_selection() {
    if (!g_ed.has_selection || g_ed.paste_mode) return;
    if (g_ed.selection_lifted) {
        g_ed.doc.end_stroke();
        g_ed.selection_lifted = false;
        g_ed.floating_clip.clear();
        g_ed.is_moving_selection = false;
        g_ed.has_selection = false;
        g_ed.selection_is_circle = false;
        g_ed.doc.set_clip_rect(nullptr);
        g_ed.status_msg = "Deleted selected tiles.";
    } else if (g_ed.selection_is_circle) {
        g_ed.doc.erase_ellipse(g_ed.selection);
        g_ed.has_selection = false;
        g_ed.selection_is_circle = false;
        g_ed.doc.set_clip_rect(nullptr);
        g_ed.status_msg = "Cleared selection.";
    } else {
        g_ed.doc.erase_rect(g_ed.selection);
        g_ed.has_selection = false;
        g_ed.selection_is_circle = false;
        g_ed.doc.set_clip_rect(nullptr);
        g_ed.status_msg = "Cleared selection.";
    }
}

static void start_paste() {
    if (g_ed.clipboard.is_empty()) {
        g_ed.status_msg = "Clipboard is empty.";
        return;
    }
    if (g_ed.selection_lifted) {
        apply_moved_selection();
    }
    g_ed.paste_mode = true;
    int px = 0;
    int py = 0;
    if (g_ed.hovered_cell.x >= 0 && g_ed.hovered_cell.y >= 0) {
        px = g_ed.hovered_cell.x;
        py = g_ed.hovered_cell.y;
    } else if (g_ed.has_selection) {
        px = g_ed.selection.x;
        py = g_ed.selection.y;
    } else {
        px = std::max(0, (g_ed.doc.width - g_ed.clipboard.w) / 2);
        py = std::max(0, (g_ed.doc.height - g_ed.clipboard.h) / 2);
    }
    const int max_x = std::max(0, g_ed.doc.width - g_ed.clipboard.w);
    const int max_y = std::max(0, g_ed.doc.height - g_ed.clipboard.h);
    px = std::clamp(px, 0, max_x);
    py = std::clamp(py, 0, max_y);
    g_ed.paste_pos = {px, py};
    g_ed.selection = {px, py, g_ed.clipboard.w, g_ed.clipboard.h};
    g_ed.has_selection = true;
    g_ed.selection_is_circle = false;
    g_ed.selection_lifted = false;
    g_ed.doc.set_clip_rect(&g_ed.selection, TilemapDoc::ClipShape::Rect);
    g_ed.is_moving_paste = false;
    g_ed.status_msg = "Pasting: Drag or use Arrows to move. Enter or click outside to commit, Esc to cancel.";
}

struct ScopedStyleColor {
    int count = 0;
    ScopedStyleColor(ImGuiCol idx, const ImVec4& col, bool condition = true) {
        if (condition) {
            ImGui::PushStyleColor(idx, col);
            count = 1;
            if (idx == ImGuiCol_Button) {
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
                count = 3;
            }
        }
    }
    ~ScopedStyleColor() {
        if (count > 0) {
            ImGui::PopStyleColor(count);
        }
    }
    ScopedStyleColor(const ScopedStyleColor&) = delete;
    ScopedStyleColor& operator=(const ScopedStyleColor&) = delete;
};

static void draw_top_nav_and_view_row(SDL_Renderer* renderer) {
    // View Switcher Buttons
    const bool is_map = (g_ed.current_view == AppView::Tilemap);
    const bool is_ts = (g_ed.current_view == AppView::TilesetMaker);

    if (is_map) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.88f, 1.0f));
    }
    if (ImGui::Button("Map Editor", ImVec2(100, 24))) {
        if (!is_map) switch_to_view(AppView::Tilemap, renderer);
    }
    if (is_map) ImGui::PopStyleColor();

    ImGui::SameLine(0, 4);

    if (is_ts) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.88f, 1.0f));
    }
    if (ImGui::Button("Tileset Maker", ImVec2(105, 24))) {
        if (!is_ts) switch_to_view(AppView::TilesetMaker, renderer);
    }
    if (is_ts) ImGui::PopStyleColor();

    ImGui::SameLine(0, 12);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 12);

    if (is_map) {
        // Global View Controls (Grid & Collision checkboxes)
        if (ImGui::Checkbox("Grid", &g_ed.settings.grid_lines)) {
            persist_settings();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Collision", &g_ed.settings.collision_overlay)) {
            persist_settings();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Zoom Controls
        if (ImGui::Button("-##ZoomOut")) {
            g_ed.zoom = std::max(0.25f, g_ed.zoom / 1.25f);
        }
        ImGui::SameLine();
        ImGui::Text("%.0f%%", g_ed.zoom * 100.0f);
        ImGui::SameLine();
        if (ImGui::Button("+##ZoomIn")) {
            g_ed.zoom = std::min(16.0f, g_ed.zoom * 1.25f);
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit View")) {
            g_ed.zoom = 2.0f;
            g_ed.pan = ImVec2(60, 40);
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "TILESET MAKER MODE");
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("Create autotile terrains and pixel art for your maps");
        ImGui::SameLine(ImGui::GetWindowWidth() - 210);
        if (ImGui::Button("Apply & Return to Map", ImVec2(190, 24))) {
            switch_to_view(AppView::Tilemap, renderer);
        }
    }
}

static void draw_tool_selection_row() {
    ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "TOOLS:");
    ImGui::SameLine();

    auto tool_button = [](const char* label, Tool t, const char* shortcut, const char* tooltip) {
        const bool is_active = (g_ed.tool == t);
        const ImVec4 active_col(0.20f, 0.50f, 0.88f, 1.0f);
        if (is_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, active_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, active_col);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.9f));
        }
        char title[64];
        std::snprintf(title, sizeof(title), "%s (%s)", label, shortcut);
        if (ImGui::Button(title)) {
            g_ed.tool = t;
            g_ed.paste_mode = false;
        }
        if (is_active) {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
        }
        if (ImGui::IsItemHovered() && tooltip && *tooltip) {
            ImGui::SetTooltip("%s", tooltip);
        }
        ImGui::SameLine();
    };

    tool_button("Pencil", Tool::Paint, "1", "Freehand drawing tool");
    tool_button("Eraser", Tool::Erase, "2", "Eraser tool");
    tool_button("Line", Tool::Line, "3", "Straight line drawing tool");
    tool_button("Square", Tool::Rect, "4", "Square / Circle tool");
    tool_button("Fill", Tool::Fill, "5", "Flood fill contiguous tiles");
    tool_button("Select", Tool::Select, "6", "Rectangular selection & move tool");
    tool_button("Eyedropper", Tool::Eyedropper, "7", "Pick tile from map into stamp");
}

static void draw_tool_options_row() {
    // Helper for rendering brush thickness preset buttons
    auto draw_thickness_controls = [](const char* label_prefix, const char* id_suffix) {
        ImGui::Text("Thickness:");
        ImGui::SameLine();
        for (int s = 1; s <= 4; ++s) {
            ScopedStyleColor bcol(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.80f, 1.0f), g_ed.brush_size == s);
            char btn_lbl[32];
            std::snprintf(btn_lbl, sizeof(btn_lbl), "%d##%sSz%d%s", s, label_prefix, s, id_suffix);
            if (ImGui::Button(btn_lbl, ImVec2(24, 0))) {
                g_ed.brush_size = s;
                persist_settings();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Brush thickness: %d tile%s", s, s > 1 ? "s" : "");
            }
            ImGui::SameLine();
        }
    };

    // Helper for rendering tile source toggle (Terrain vs Stamp)
    auto draw_source_controls = [](const char* id_suffix) {
        ImGui::Text("Mode:");
        ImGui::SameLine();
        {
            ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.35f, 1.0f), g_ed.paint_mode == TileMode::Terrain);
            char t_btn[48];
            std::snprintf(t_btn, sizeof(t_btn), "Terrain Autotile##%s", id_suffix);
            if (ImGui::Button(t_btn)) {
                g_ed.paint_mode = TileMode::Terrain;
                persist_settings();
            }
        }
        ImGui::SameLine();
        {
            ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.35f, 1.0f), g_ed.paint_mode == TileMode::Stamp);
            char s_btn[48];
            std::snprintf(s_btn, sizeof(s_btn), "Stamp Tile##%s", id_suffix);
            if (ImGui::Button(s_btn)) {
                g_ed.paint_mode = TileMode::Stamp;
                persist_settings();
            }
        }
        ImGui::SameLine();
        if (g_ed.paint_mode == TileMode::Terrain) {
            char r_btn[48];
            std::snprintf(r_btn, sizeof(r_btn), "Reroll Variants##%s", id_suffix);
            if (ImGui::Button(r_btn)) {
                g_ed.doc.reroll_variants();
                g_ed.status_msg = "Rerolled terrain variants.";
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Rerolls variant chances across all terrain autotiles on the map");
            }
            ImGui::SameLine();
        } else {
            ImGui::TextDisabled("Stamp: (%d, %d)", g_ed.stamp_col, g_ed.stamp_row);
            ImGui::SameLine();
        }
    };

    // Helper for rendering contextual hints only when there is sufficient room
    auto draw_hint = [](const char* hint) {
        if (ImGui::GetContentRegionAvail().x > 160.0f) {
            ImGui::TextDisabled("|  %s", hint);
        }
    };

    switch (g_ed.tool) {
        case Tool::Paint: {
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "PENCIL OPTIONS:");
            ImGui::SameLine();
            draw_thickness_controls("Pencil", "Opt");
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            draw_source_controls("PencilOpt");
            draw_hint("Left-drag: Paint  |  Right-drag: Erase");
            break;
        }
        case Tool::Line: {
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "LINE OPTIONS:");
            ImGui::SameLine();
            draw_thickness_controls("Line", "Opt");
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            draw_source_controls("LineOpt");
            draw_hint("Left-drag: Draw Line  |  Right-drag: Erase Line");
            break;
        }
        case Tool::Erase: {
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "ERASER OPTIONS:");
            ImGui::SameLine();
            draw_thickness_controls("Eraser", "Opt");
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (ImGui::Button("Clear Entire Map##EraseAll")) {
                g_ed.doc.clear_cells();
                g_ed.status_msg = "Cleared map.";
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Erase all tiles on the entire map");
            }
            ImGui::SameLine();
            draw_hint("Click & drag to erase tiles");
            break;
        }
        case Tool::Rect: {
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "SQUARE OPTIONS:");
            ImGui::SameLine();
            ImGui::Checkbox("Circle Mode##RectCircle", &g_ed.rect_circle);
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            {
                ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.80f, 1.0f), g_ed.rect_fill);
                if (ImGui::Button("Fill##RectOptFill")) {
                    g_ed.rect_fill = true;
                }
            }
            ImGui::SameLine();
            {
                ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.80f, 1.0f), !g_ed.rect_fill);
                if (ImGui::Button("Outline##RectOptOutline")) {
                    g_ed.rect_fill = false;
                }
            }
            ImGui::SameLine();
            if (!g_ed.rect_fill) {
                draw_thickness_controls("Rect", "Opt");
            }
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            draw_source_controls("RectOpt");
            draw_hint("Left-drag: Draw  |  Right-drag: Erase  |  Hold Shift: 1:1");
            break;
        }
        case Tool::Fill: {
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "FILL OPTIONS:");
            ImGui::SameLine();
            draw_source_controls("FillOpt");
            draw_hint("Scope: Contiguous matching tiles  |  Left-click: Fill  |  Right-click: Erase contiguous");
            break;
        }
        case Tool::Select: {
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "SELECTION OPTIONS:");
            ImGui::SameLine();
            ImGui::Checkbox("Circle Mode##SelectCircle", &g_ed.select_circle);
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (g_ed.paste_mode) {
                ImGui::Text("Pasting: %dx%d at (%d, %d)", g_ed.clipboard.w, g_ed.clipboard.h, g_ed.paste_pos.x, g_ed.paste_pos.y);
            } else if (g_ed.has_selection) {
                ImGui::Text("%s: %dx%d at (%d, %d)", g_ed.selection_is_circle ? "Circle Selected" : "Selected", g_ed.selection.w, g_ed.selection.h, g_ed.selection.x, g_ed.selection.y);
            } else {
                ImGui::TextDisabled("No active selection");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            const bool can_cut_copy = g_ed.has_selection && !g_ed.paste_mode;
            if (!can_cut_copy) ImGui::BeginDisabled(true);
            if (ImGui::Button("Cut##SelCut")) {
                cut_selection();
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy##SelCopy")) {
                copy_selection();
            }
            if (!can_cut_copy) ImGui::EndDisabled();
            ImGui::SameLine();

            const bool can_paste = !g_ed.clipboard.is_empty();
            if (!can_paste) ImGui::BeginDisabled(true);
            if (ImGui::Button("Paste##SelPaste")) {
                start_paste();
            }
            if (!can_paste) ImGui::EndDisabled();
            ImGui::SameLine();

            if (!can_cut_copy) ImGui::BeginDisabled(true);
            if (ImGui::Button("Delete##SelDel")) {
                delete_selection();
            }
            if (!can_cut_copy) ImGui::EndDisabled();
            ImGui::SameLine();

            const bool can_desel = g_ed.has_selection || g_ed.paste_mode;
            if (!can_desel) ImGui::BeginDisabled(true);
            if (ImGui::Button("Deselect##SelDeselect")) {
                deselect();
            }
            if (!can_desel) ImGui::EndDisabled();
            ImGui::SameLine();

            draw_hint("Drag: Select  |  Hold Shift: 1:1  |  Drag inside to move");
            break;
        }
        case Tool::Eyedropper: {
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "EYEDROPPER OPTIONS:");
            ImGui::SameLine();
            ImGui::Text("Sampled Stamp Tile: (%d, %d)", g_ed.stamp_col, g_ed.stamp_row);
            ImGui::SameLine();
            if (ImGui::Button("Switch to Pencil##EyeToPencil")) {
                g_ed.tool = Tool::Paint;
                g_ed.status_msg = "Switched to Pencil tool.";
            }
            ImGui::SameLine();
            draw_hint("Click any tile on the canvas to sample it into Stamp mode");
            break;
        }
    }
}

static void draw_canvas_viewport_content() {
    const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 30.0f || canvas_sz.y < 30.0f) {
        return;
    }
    const ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(canvas_p0, canvas_p1, true);

    // Background checkerboard
    const ImU32 canvas_bg_col = g_ed.settings.dark ? IM_COL32(18, 20, 24, 255) : IM_COL32(215, 218, 224, 255);
    draw_list->AddRectFilled(canvas_p0, canvas_p1, canvas_bg_col);

    const ImGuiIO& io = ImGui::GetIO();
    const bool is_hovered = ImGui::IsWindowHovered();

    // Pan with middle mouse or space+drag
    const bool space_down = ImGui::IsKeyDown(ImGuiKey_Space);
    if (is_hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (space_down && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
        g_ed.is_panning = true;
        g_ed.pan_drag_start = io.MousePos;
        g_ed.pan_start_offset = g_ed.pan;
    }
    if (g_ed.is_panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (space_down && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
            g_ed.pan.x = g_ed.pan_start_offset.x + (io.MousePos.x - g_ed.pan_drag_start.x);
            g_ed.pan.y = g_ed.pan_start_offset.y + (io.MousePos.y - g_ed.pan_drag_start.y);
        } else {
            g_ed.is_panning = false;
        }
    }

    // 1. Pinch gesture zoom (e.g. macOS trackpad pinch-to-zoom)
    if (g_ed.has_pinch && g_ed.pinch_scale > 0.0f) {
        const float old_zoom = g_ed.zoom;
        g_ed.zoom = std::clamp(g_ed.zoom * g_ed.pinch_scale, 0.25f, 16.0f);
        float mx = io.MousePos.x - canvas_p0.x;
        float my = io.MousePos.y - canvas_p0.y;
        if (mx < 0.0f || mx > canvas_sz.x || my < 0.0f || my > canvas_sz.y) {
            mx = canvas_sz.x * 0.5f;
            my = canvas_sz.y * 0.5f;
        }
        g_ed.pan.x = mx - (mx - g_ed.pan.x) * (g_ed.zoom / old_zoom);
        g_ed.pan.y = my - (my - g_ed.pan.y) * (g_ed.zoom / old_zoom);
        g_ed.has_pinch = false;
        g_ed.pinch_scale = 1.0f;
    }

    // 2. Trackpad & Mouse Wheel navigation (scroll to pan, Cmd/Ctrl+scroll to zoom)
    if (is_hovered && !g_ed.is_drawing) {
        const bool cmd_or_ctrl = io.KeyCtrl || io.KeySuper;
        if (cmd_or_ctrl) {
            // Cmd/Ctrl + Wheel: Zoom around mouse cursor
            if (io.MouseWheel != 0.0f) {
                const float old_zoom = g_ed.zoom;
                const float factor = (io.MouseWheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
                g_ed.zoom = std::clamp(g_ed.zoom * factor, 0.25f, 16.0f);

                const float mx = io.MousePos.x - canvas_p0.x;
                const float my = io.MousePos.y - canvas_p0.y;
                g_ed.pan.x = mx - (mx - g_ed.pan.x) * (g_ed.zoom / old_zoom);
                g_ed.pan.y = my - (my - g_ed.pan.y) * (g_ed.zoom / old_zoom);
            }
        } else {
            // Normal Scroll: Pan canvas horizontally and vertically
            float scroll_dx = io.MouseWheelH;
            float scroll_dy = io.MouseWheel;
            if (io.MouseWheelRequestAxisSwap || (io.KeyShift && scroll_dx == 0.0f)) {
                scroll_dx = scroll_dy;
                scroll_dy = 0.0f;
            }
            if (scroll_dx != 0.0f || scroll_dy != 0.0f) {
                const float scroll_speed = 28.0f;
                g_ed.pan.x += scroll_dx * scroll_speed;
                g_ed.pan.y += scroll_dy * scroll_speed;
            }
        }
    }

    const float tile_px = static_cast<float>(g_ed.doc.tile_size) * g_ed.zoom;
    const float origin_x = std::floor(canvas_p0.x + g_ed.pan.x);
    const float origin_y = std::floor(canvas_p0.y + g_ed.pan.y);

    auto cell_to_screen_x = [origin_x, tile_px](int c) -> float {
        return std::floor(origin_x + static_cast<float>(c) * tile_px);
    };
    auto cell_to_screen_y = [origin_y, tile_px](int r) -> float {
        return std::floor(origin_y + static_cast<float>(r) * tile_px);
    };

    const float map_x0 = cell_to_screen_x(0);
    const float map_y0 = cell_to_screen_y(0);
    const float map_x1 = cell_to_screen_x(g_ed.doc.width);
    const float map_y1 = cell_to_screen_y(g_ed.doc.height);

    const float buf_x0 = cell_to_screen_x(-g_ed.doc.buffer);
    const float buf_y0 = cell_to_screen_y(-g_ed.doc.buffer);
    const float buf_x1 = cell_to_screen_x(g_ed.doc.width + g_ed.doc.buffer);
    const float buf_y1 = cell_to_screen_y(g_ed.doc.height + g_ed.doc.buffer);

    // Draw canvas backgrounds: outer buffer zone (outside zone) in dark gray and active map
    const ImU32 buf_bg_col = g_ed.settings.dark ? IM_COL32(16, 18, 22, 255) : IM_COL32(48, 52, 60, 255);
    const ImU32 map_bg_col = g_ed.settings.dark ? IM_COL32(30, 33, 40, 255) : IM_COL32(245, 246, 250, 255);
    draw_list->AddRectFilled(ImVec2(buf_x0, buf_y0), ImVec2(buf_x1, buf_y1), buf_bg_col);
    draw_list->AddRectFilled(ImVec2(map_x0, map_y0), ImVec2(map_x1, map_y1), map_bg_col);

    // Render placed tiles (across map and buffer) with nearest-neighbor point sampling
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (platform_io.DrawCallback_SetSamplerNearest != nullptr) {
        draw_list->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);
    }

    const bool has_texture = (g_ed.tileset_texture != nullptr && g_ed.texture_w > 0 && g_ed.texture_h > 0);
    const float inv_tex_w = has_texture ? (1.0f / static_cast<float>(g_ed.texture_w)) : 1.0f;
    const float inv_tex_h = has_texture ? (1.0f / static_cast<float>(g_ed.texture_h)) : 1.0f;
    const int ts = g_ed.doc.tile_size;

    for (int cy = -g_ed.doc.buffer; cy < g_ed.doc.height + g_ed.doc.buffer; ++cy) {
        for (int cx = -g_ed.doc.buffer; cx < g_ed.doc.width + g_ed.doc.buffer; ++cx) {
            const MapCell& cell = g_ed.doc.get_cell(cx, cy);

            const float x0 = cell_to_screen_x(cx);
            const float y0 = cell_to_screen_y(cy);
            const float x1 = cell_to_screen_x(cx + 1);
            const float y1 = cell_to_screen_y(cy + 1);

            if (x1 < canvas_p0.x || y1 < canvas_p0.y || x0 > canvas_p1.x || y0 > canvas_p1.y) {
                continue; // Frustum cull
            }

            int ax = cell.atlas_x;
            int ay = cell.atlas_y;
            if (cell.mode == TileMode::Empty || ax < 0 || ay < 0) {
                ax = 10;
                ay = 1;
            }

            if (has_texture && g_ed.doc.tileset.in_bounds(ax, ay)) {
                const float u0 = static_cast<float>(ax * ts) * inv_tex_w;
                const float v0 = static_cast<float>(ay * ts) * inv_tex_h;
                const float u1 = static_cast<float>((ax + 1) * ts) * inv_tex_w;
                const float v1 = static_cast<float>((ay + 1) * ts) * inv_tex_h;
                draw_list->AddImage(reinterpret_cast<ImTextureID>(g_ed.tileset_texture), ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(u0, v0), ImVec2(u1, v1));
            } else if (!cell.is_empty()) {
                draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(80, 120, 180, 255));
            }
        }
    }

    // Dark gray tint overlay over buffer area (drawn tiles in the buffer appear underneath this tint)
    if (g_ed.doc.buffer > 0) {
        const ImU32 buf_tint_col = IM_COL32(12, 14, 18, 160);
        // Top buffer strip
        draw_list->AddRectFilled(ImVec2(buf_x0, buf_y0), ImVec2(buf_x1, map_y0), buf_tint_col);
        // Bottom buffer strip
        draw_list->AddRectFilled(ImVec2(buf_x0, map_y1), ImVec2(buf_x1, buf_y1), buf_tint_col);
        // Left buffer strip
        draw_list->AddRectFilled(ImVec2(buf_x0, map_y0), ImVec2(map_x0, map_y1), buf_tint_col);
        // Right buffer strip
        draw_list->AddRectFilled(ImVec2(map_x1, map_y0), ImVec2(buf_x1, map_y1), buf_tint_col);
    }

    // Draw grid lines
    if (g_ed.settings.grid_lines && tile_px >= 4.0f) {
        const ImU32 grid_col = g_ed.settings.dark ? IM_COL32(60, 65, 75, 140) : IM_COL32(150, 155, 165, 160);
        for (int x = -g_ed.doc.buffer; x <= g_ed.doc.width + g_ed.doc.buffer; ++x) {
            const float gx = cell_to_screen_x(x);
            draw_list->AddLine(ImVec2(gx, buf_y0), ImVec2(gx, buf_y1), grid_col);
        }
        for (int y = -g_ed.doc.buffer; y <= g_ed.doc.height + g_ed.doc.buffer; ++y) {
            const float gy = cell_to_screen_y(y);
            draw_list->AddLine(ImVec2(buf_x0, gy), ImVec2(buf_x1, gy), grid_col);
        }
    }

    // Draw Collision 8x8 Overlay
    if (g_ed.settings.collision_overlay) {
        const CollisionGrid col_grid = g_ed.doc.build_collision_grid();
        const float col_step = tile_px * (8.0f / static_cast<float>(g_ed.doc.tile_size));

        const int min_cx = std::max(0, static_cast<int>(std::floor((canvas_p0.x - origin_x) / col_step)));
        const int max_cx = std::min(col_grid.width, static_cast<int>(std::ceil((canvas_p1.x - origin_x) / col_step)));
        const int min_cy = std::max(0, static_cast<int>(std::floor((canvas_p0.y - origin_y) / col_step)));
        const int max_cy = std::min(col_grid.height, static_cast<int>(std::ceil((canvas_p1.y - origin_y) / col_step)));

        for (int cy = min_cy; cy < max_cy; ++cy) {
            for (int cx = min_cx; cx < max_cx; ++cx) {
                const uint8_t type_id = col_grid.get_type(cx, cy);
                if (type_id != 0) {
                    const CollisionType* ct = g_ed.doc.get_collision_type(type_id);
                    const Rgb col_rgb = ct ? ct->color : Rgb{235, 60, 50};
                    const ImU32 col_solid_color = IM_COL32(col_rgb.r, col_rgb.g, col_rgb.b, 90);
                    const ImU32 col_border_color = IM_COL32(col_rgb.r, col_rgb.g, col_rgb.b, 180);

                    const float cx0 = std::floor(origin_x + static_cast<float>(cx) * col_step);
                    const float cy0 = std::floor(origin_y + static_cast<float>(cy) * col_step);
                    const float cx1 = std::floor(origin_x + static_cast<float>(cx + 1) * col_step);
                    const float cy1 = std::floor(origin_y + static_cast<float>(cy + 1) * col_step);
                    draw_list->AddRectFilled(ImVec2(cx0, cy0), ImVec2(cx1, cy1), col_solid_color);
                    draw_list->AddRect(ImVec2(cx0, cy0), ImVec2(cx1, cy1), col_border_color);
                }
            }
        }
    }

    // Outer buffer border (subtle)
    draw_list->AddRect(ImVec2(buf_x0, buf_y0), ImVec2(buf_x1, buf_y1), IM_COL32(70, 75, 85, 200), 0.0f, 0, 1.0f);

    // Active export map boundary border (prominent blue)
    draw_list->AddRect(ImVec2(map_x0, map_y0), ImVec2(map_x1, map_y1), IM_COL32(90, 140, 230, 255), 0.0f, 0, 2.0f);

    // Mouse coordinates in map cell units
    const float rel_x = (io.MousePos.x - origin_x) / tile_px;
    const float rel_y = (io.MousePos.y - origin_y) / tile_px;
    const int cell_x = static_cast<int>(std::floor(rel_x));
    const int cell_y = static_cast<int>(std::floor(rel_y));
    const bool in_map = g_ed.doc.in_bounds(cell_x, cell_y);

    if (in_map) {
        g_ed.hovered_cell = {cell_x, cell_y};
    } else {
        g_ed.hovered_cell = {-999999, -999999};
    }

    // Draw floating preview (for paste or moving selection)
    const bool show_floating_paste = (g_ed.paste_mode && !g_ed.clipboard.is_empty());
    const bool show_floating_selection = (g_ed.selection_lifted && !g_ed.floating_clip.is_empty());
    if (show_floating_paste || show_floating_selection) {
        const auto& clip_cells = show_floating_paste ? g_ed.clipboard.cells : g_ed.floating_clip.cells;
        const int base_x = show_floating_paste ? g_ed.paste_pos.x : g_ed.selection.x;
        const int base_y = show_floating_paste ? g_ed.paste_pos.y : g_ed.selection.y;

        for (const auto& item : clip_cells) {
            const int cx = base_x + item.first.x;
            const int cy = base_y + item.first.y;
            const MapCell& cell = item.second;
            if (cell.is_empty()) continue;

            const float x0 = cell_to_screen_x(cx);
            const float y0 = cell_to_screen_y(cy);
            const float x1 = cell_to_screen_x(cx + 1);
            const float y1 = cell_to_screen_y(cy + 1);

            if (x1 < canvas_p0.x || y1 < canvas_p0.y || x0 > canvas_p1.x || y0 > canvas_p1.y) {
                continue;
            }

            if (has_texture && cell.atlas_x >= 0 && cell.atlas_y >= 0) {
                const float u0 = static_cast<float>(cell.atlas_x * ts) * inv_tex_w;
                const float v0 = static_cast<float>(cell.atlas_y * ts) * inv_tex_h;
                const float u1 = static_cast<float>((cell.atlas_x + 1) * ts) * inv_tex_w;
                const float v1 = static_cast<float>((cell.atlas_y + 1) * ts) * inv_tex_h;
                draw_list->AddImage(reinterpret_cast<ImTextureID>(g_ed.tileset_texture),
                                    ImVec2(x0, y0), ImVec2(x1, y1),
                                    ImVec2(u0, v0), ImVec2(u1, v1),
                                    IM_COL32(255, 255, 255, 255));
            } else {
                draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(100, 200, 255, 180));
            }
        }
    }

    // Brush / Tool Preview
    if (is_hovered && !space_down && in_map && !g_ed.paste_mode) {
        const int bs = (g_ed.tool == Tool::Paint || g_ed.tool == Tool::Line || g_ed.tool == Tool::Erase || (g_ed.tool == Tool::Rect && !g_ed.rect_fill)) ? g_ed.brush_size : 1;
        const float bx0 = cell_to_screen_x(cell_x);
        const float by0 = cell_to_screen_y(cell_y);
        const float bx1 = cell_to_screen_x(cell_x + bs);
        const float by1 = cell_to_screen_y(cell_y + bs);

        if (g_ed.tool == Tool::Erase) {
            draw_list->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(230, 50, 50, 80));
            draw_list->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(255, 80, 80, 220), 0.0f, 0, 1.5f);
        } else if (g_ed.tool == Tool::Paint || g_ed.tool == Tool::Line || (g_ed.tool == Tool::Rect && !g_ed.rect_fill)) {
            if (has_texture && g_ed.paint_mode == TileMode::Stamp) {
                const float u0 = static_cast<float>(g_ed.stamp_col * ts) * inv_tex_w;
                const float v0 = static_cast<float>(g_ed.stamp_row * ts) * inv_tex_h;
                const float u1 = static_cast<float>((g_ed.stamp_col + 1) * ts) * inv_tex_w;
                const float v1 = static_cast<float>((g_ed.stamp_row + 1) * ts) * inv_tex_h;
                for (int dy = 0; dy < bs; ++dy) {
                    for (int dx = 0; dx < bs; ++dx) {
                        const float tx0 = cell_to_screen_x(cell_x + dx);
                        const float ty0 = cell_to_screen_y(cell_y + dy);
                        const float tx1 = cell_to_screen_x(cell_x + dx + 1);
                        const float ty1 = cell_to_screen_y(cell_y + dy + 1);
                        draw_list->AddImage(reinterpret_cast<ImTextureID>(g_ed.tileset_texture),
                                            ImVec2(tx0, ty0), ImVec2(tx1, ty1),
                                            ImVec2(u0, v0), ImVec2(u1, v1), IM_COL32(255, 255, 255, 180));
                    }
                }
            } else {
                draw_list->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(80, 200, 120, 80));
            }
            draw_list->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), IM_COL32(100, 230, 140, 220), 0.0f, 0, 1.5f);
        }
    }

    // Restore linear sampler for UI elements
    if (platform_io.DrawCallback_SetSamplerLinear != nullptr) {
        draw_list->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
    }

    const bool in_paste_rect = g_ed.paste_mode &&
        (cell_x >= g_ed.paste_pos.x && cell_x < g_ed.paste_pos.x + g_ed.clipboard.w &&
         cell_y >= g_ed.paste_pos.y && cell_y < g_ed.paste_pos.y + g_ed.clipboard.h);
    const bool in_sel_rect = g_ed.has_selection && !g_ed.paste_mode &&
        (g_ed.selection_is_circle ?
            is_cell_in_ellipse(cell_x, cell_y, g_ed.selection) :
            (cell_x >= g_ed.selection.x && cell_x < g_ed.selection.right() &&
             cell_y >= g_ed.selection.y && cell_y < g_ed.selection.bottom()));

    if (is_hovered && !space_down) {
        if (in_paste_rect || (in_sel_rect && g_ed.tool == Tool::Select)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }

    // Interactive Drawing on Canvas
    const bool left_clicked = is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !space_down;
    const bool right_clicked = is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !space_down;

    if (g_ed.paste_mode) {
        if (left_clicked) {
            if (in_paste_rect) {
                g_ed.is_drawing = true;
                g_ed.is_moving_paste = true;
                g_ed.paste_drag_origin = g_ed.paste_pos;
                g_ed.drag_start = {cell_x, cell_y};
            } else {
                commit_paste();
            }
        } else if (right_clicked) {
            cancel_paste();
        }
    } else if (right_clicked && g_ed.tool == Tool::Select) {
        deselect();
    } else if (left_clicked || right_clicked) {
        if (g_ed.selection_lifted && g_ed.tool != Tool::Select) {
            apply_moved_selection();
        }
        g_ed.is_drawing = true;
        g_ed.drag_start = {cell_x, cell_y};
        g_ed.last_mouse_cell = {cell_x, cell_y};
        g_ed.last_painted_cell = {cell_x, cell_y};
        g_ed.stroke_points.clear();
        g_ed.stroke_points.push_back(ImVec2(rel_x, rel_y));
        g_ed.right_click_erasing = right_clicked;

        if (g_ed.tool == Tool::Select && !g_ed.right_click_erasing) {
            if (in_sel_rect) {
                if (!g_ed.selection_lifted) {
                    g_ed.selection_origin = g_ed.selection;
                    if (g_ed.selection_is_circle) {
                        g_ed.floating_clip = g_ed.doc.copy_ellipse(g_ed.selection);
                        g_ed.doc.begin_stroke("Move Selection");
                        g_ed.doc.erase_ellipse(g_ed.selection);
                    } else {
                        g_ed.floating_clip = g_ed.doc.copy_rect(g_ed.selection);
                        g_ed.doc.begin_stroke("Move Selection");
                        g_ed.doc.erase_rect(g_ed.selection);
                    }
                    g_ed.selection_lifted = true;
                }
                g_ed.is_moving_selection = true;
                g_ed.selection_drag_origin = g_ed.selection;
                g_ed.drag_start = {cell_x, cell_y};
            } else {
                if (g_ed.selection_lifted) {
                    apply_moved_selection();
                }
                g_ed.is_moving_selection = false;
            }
        } else if (g_ed.tool == Tool::Paint && !g_ed.right_click_erasing) {
            g_ed.is_moving_selection = false;
            g_ed.doc.begin_stroke(g_ed.paint_mode == TileMode::Terrain ? "Paint Terrain" : "Paint Stamp");
            g_ed.doc.paint_cell(cell_x, cell_y, g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row, g_ed.brush_size);
        } else if ((g_ed.tool == Tool::Erase || g_ed.right_click_erasing) && g_ed.tool != Tool::Line && g_ed.tool != Tool::Rect && g_ed.tool != Tool::Select) {
            g_ed.is_moving_selection = false;
            g_ed.doc.begin_stroke("Erase");
            g_ed.doc.erase_cell(cell_x, cell_y, g_ed.brush_size);
        } else if (g_ed.tool == Tool::Fill) {
            g_ed.is_moving_selection = false;
            if (g_ed.right_click_erasing) {
                g_ed.doc.flood_fill(cell_x, cell_y, TileMode::Empty);
            } else {
                g_ed.doc.flood_fill(cell_x, cell_y, g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row);
            }
            g_ed.is_drawing = false;
        } else if (g_ed.tool == Tool::Eyedropper) {
            g_ed.is_moving_selection = false;
            const MapCell& mc = g_ed.doc.get_cell(cell_x, cell_y);
            const int px = (mc.atlas_x >= 0) ? mc.atlas_x : 10;
            const int py = (mc.atlas_y >= 0) ? mc.atlas_y : 1;
            g_ed.stamp_col = px;
            g_ed.stamp_row = py;
            g_ed.paint_mode = TileMode::Stamp;
            g_ed.status_msg = "Sampled tile (" + std::to_string(px) + ", " + std::to_string(py) + ")";
            g_ed.is_drawing = false;
        }
    }

    if (g_ed.is_drawing) {
        if (g_ed.is_moving_paste) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const int dx = cell_x - g_ed.drag_start.x;
                const int dy = cell_y - g_ed.drag_start.y;
                const int min_x = -g_ed.doc.buffer;
                const int min_y = -g_ed.doc.buffer;
                const int max_x = g_ed.doc.width + g_ed.doc.buffer - g_ed.clipboard.w;
                const int max_y = g_ed.doc.height + g_ed.doc.buffer - g_ed.clipboard.h;
                g_ed.paste_pos.x = std::clamp(g_ed.paste_drag_origin.x + dx, min_x, std::max(min_x, max_x));
                g_ed.paste_pos.y = std::clamp(g_ed.paste_drag_origin.y + dy, min_y, std::max(min_y, max_y));
                g_ed.selection.x = g_ed.paste_pos.x;
                g_ed.selection.y = g_ed.paste_pos.y;
                g_ed.doc.set_clip_rect(&g_ed.selection);
            } else {
                g_ed.is_moving_paste = false;
                g_ed.is_drawing = false;
            }
        } else if (g_ed.is_moving_selection) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const int dx = cell_x - g_ed.drag_start.x;
                const int dy = cell_y - g_ed.drag_start.y;
                const int min_x = -g_ed.doc.buffer;
                const int min_y = -g_ed.doc.buffer;
                const int max_x = g_ed.doc.width + g_ed.doc.buffer - g_ed.selection.w;
                const int max_y = g_ed.doc.height + g_ed.doc.buffer - g_ed.selection.h;
                g_ed.selection.x = std::clamp(g_ed.selection_drag_origin.x + dx, min_x, std::max(min_x, max_x));
                g_ed.selection.y = std::clamp(g_ed.selection_drag_origin.y + dy, min_y, std::max(min_y, max_y));
                g_ed.doc.set_clip_rect(&g_ed.selection, g_ed.selection_is_circle ? TilemapDoc::ClipShape::Ellipse : TilemapDoc::ClipShape::Rect);
            } else {
                g_ed.is_moving_selection = false;
                g_ed.is_drawing = false;
                g_ed.doc.set_clip_rect(&g_ed.selection, g_ed.selection_is_circle ? TilemapDoc::ClipShape::Ellipse : TilemapDoc::ClipShape::Rect);
                g_ed.status_msg = "Moved selection (floating). Press Ctrl+D or right-click to apply.";
            }
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (g_ed.tool == Tool::Paint || g_ed.tool == Tool::Erase || (g_ed.right_click_erasing && g_ed.tool != Tool::Line && g_ed.tool != Tool::Rect && g_ed.tool != Tool::Select)) {
                auto paint_or_erase_cell = [&](int cx, int cy) {
                    if (cx == g_ed.last_painted_cell.x && cy == g_ed.last_painted_cell.y) {
                        return;
                    }
                    if (g_ed.right_click_erasing || g_ed.tool == Tool::Erase) {
                        g_ed.doc.erase_cell(cx, cy, g_ed.brush_size);
                    } else {
                        g_ed.doc.paint_cell(cx, cy, g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row, g_ed.brush_size);
                    }
                    g_ed.last_painted_cell = {cx, cy};
                    g_ed.last_mouse_cell = {cx, cy};
                };

                auto catmull_rom = [](const ImVec2& p0, const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, float t) -> ImVec2 {
                    const float t2 = t * t;
                    const float t3 = t2 * t;
                    const float f0 = -0.5f * t3 + t2 - 0.5f * t;
                    const float f1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
                    const float f2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
                    const float f3 = 0.5f * t3 - 0.5f * t2;
                    return ImVec2(
                        p0.x * f0 + p1.x * f1 + p2.x * f2 + p3.x * f3,
                        p0.y * f0 + p1.y * f1 + p2.y * f2 + p3.y * f3
                    );
                };

                auto add_and_interpolate_stroke = [&](float fx, float fy) {
                    const ImVec2 new_pt(fx, fy);
                    if (g_ed.stroke_points.empty()) {
                        g_ed.stroke_points.push_back(new_pt);
                        paint_or_erase_cell(static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fy)));
                        return;
                    }

                    const ImVec2 prev_pt = g_ed.stroke_points.back();
                    const float dist = std::hypot(new_pt.x - prev_pt.x, new_pt.y - prev_pt.y);
                    if (dist < 0.05f) {
                        return;
                    }

                    g_ed.stroke_points.push_back(new_pt);
                    const size_t n = g_ed.stroke_points.size();
                    const ImVec2 p1 = prev_pt;
                    const ImVec2 p2 = new_pt;
                    const ImVec2 p0 = (n >= 3) ? g_ed.stroke_points[n - 3] : ImVec2(2.0f * p1.x - p2.x, 2.0f * p1.y - p2.y);
                    const ImVec2 p3 = ImVec2(2.0f * p2.x - p1.x, 2.0f * p2.y - p1.y);

                    const int steps = std::min(500, std::max(1, static_cast<int>(std::ceil(dist / 0.2f))));
                    for (int s = 1; s <= steps; ++s) {
                        const float t = static_cast<float>(s) / static_cast<float>(steps);
                        const ImVec2 pos = catmull_rom(p0, p1, p2, p3, t);
                        const int cx = static_cast<int>(std::floor(pos.x));
                        const int cy = static_cast<int>(std::floor(pos.y));
                        paint_or_erase_cell(cx, cy);
                    }

                    if (g_ed.stroke_points.size() > 64) {
                        g_ed.stroke_points.erase(g_ed.stroke_points.begin(), g_ed.stroke_points.begin() + 32);
                    }
                };

                std::vector<ImVec2> pts = g_ed.pending_mouse_moves;
                if (pts.empty() || pts.back().x != io.MousePos.x || pts.back().y != io.MousePos.y) {
                    pts.push_back(io.MousePos);
                }

                for (const auto& sp : pts) {
                    const float fx = (sp.x - origin_x) / tile_px;
                    const float fy = (sp.y - origin_y) / tile_px;
                    add_and_interpolate_stroke(fx, fy);
                }
                g_ed.pending_mouse_moves.clear();
            } else if (g_ed.tool == Tool::Line) {
                const int x0 = g_ed.drag_start.x;
                const int y0 = g_ed.drag_start.y;
                const int x1 = cell_x;
                const int y1 = cell_y;
                const int dx = std::abs(x1 - x0);
                const int dy = -std::abs(y1 - y0);
                const int sx = (x0 < x1) ? 1 : -1;
                const int sy = (y0 < y1) ? 1 : -1;
                int err = dx + dy;
                int x = x0;
                int y = y0;
                const int bs = g_ed.brush_size;

                const ImU32 fill_col = g_ed.right_click_erasing ? IM_COL32(220, 50, 50, 80) : IM_COL32(80, 200, 120, 90);
                const ImU32 border_col = g_ed.right_click_erasing ? IM_COL32(255, 80, 80, 200) : IM_COL32(100, 230, 140, 200);

                while (true) {
                    const float px0 = cell_to_screen_x(x);
                    const float py0 = cell_to_screen_y(y);
                    const float px1 = cell_to_screen_x(x + bs);
                    const float py1 = cell_to_screen_y(y + bs);
                    draw_list->AddRectFilled(ImVec2(px0, py0), ImVec2(px1, py1), fill_col);
                    draw_list->AddRect(ImVec2(px0, py0), ImVec2(px1, py1), border_col, 0.0f, 0, 1.0f);

                    if (x == x1 && y == y1) break;
                    const int e2 = 2 * err;
                    if (e2 >= dy) { err += dy; x += sx; }
                    if (e2 <= dx) { err += dx; y += sy; }
                }

                const float start_center_x = cell_to_screen_x(x0) + (bs * tile_px * 0.5f);
                const float start_center_y = cell_to_screen_y(y0) + (bs * tile_px * 0.5f);
                const float curr_center_x = cell_to_screen_x(x1) + (bs * tile_px * 0.5f);
                const float curr_center_y = cell_to_screen_y(y1) + (bs * tile_px * 0.5f);
                draw_list->AddLine(ImVec2(start_center_x, start_center_y), ImVec2(curr_center_x, curr_center_y),
                                   g_ed.right_click_erasing ? IM_COL32(255, 100, 100, 220) : IM_COL32(120, 255, 160, 220), 1.5f);
            } else if (g_ed.tool == Tool::Rect || g_ed.tool == Tool::Select) {
                const Rect r = compute_drag_rect(g_ed.drag_start.x, g_ed.drag_start.y, cell_x, cell_y, io.KeyShift);
                const float rpx0 = cell_to_screen_x(r.x);
                const float rpy0 = cell_to_screen_y(r.y);
                const float rpx1 = cell_to_screen_x(r.x + r.w);
                const float rpy1 = cell_to_screen_y(r.y + r.h);

                const ImU32 fill_col = g_ed.right_click_erasing ? IM_COL32(220, 50, 50, 70) : IM_COL32(60, 160, 240, 70);
                const ImU32 border_col = g_ed.right_click_erasing ? IM_COL32(255, 80, 80, 240) : IM_COL32(80, 180, 255, 240);

                const bool is_circle = (g_ed.tool == Tool::Rect) ? g_ed.rect_circle : g_ed.select_circle;

                if (is_circle) {
                    const ImVec2 center((rpx0 + rpx1) * 0.5f, (rpy0 + rpy1) * 0.5f);
                    const ImVec2 radius((rpx1 - rpx0) * 0.5f, (rpy1 - rpy0) * 0.5f);

                    if (g_ed.tool == Tool::Select || g_ed.rect_fill) {
                        draw_list->AddEllipseFilled(center, radius, fill_col);
                        draw_list->AddEllipse(center, radius, border_col, 0.0f, 0, 2.0f);
                    } else {
                        const int bs = std::min(g_ed.brush_size, std::min(r.w, r.h));
                        if (bs * 2 >= r.w || bs * 2 >= r.h) {
                            draw_list->AddEllipseFilled(center, radius, fill_col);
                        } else {
                            for (int y = r.y; y < r.bottom(); ++y) {
                                for (int x = r.x; x < r.right(); ++x) {
                                    if (is_cell_in_outline_ellipse(x, y, r, bs)) {
                                        const float cx0 = cell_to_screen_x(x);
                                        const float cy0 = cell_to_screen_y(y);
                                        const float cx1 = cell_to_screen_x(x + 1);
                                        const float cy1 = cell_to_screen_y(y + 1);
                                        draw_list->AddRectFilled(ImVec2(cx0, cy0), ImVec2(cx1, cy1), fill_col);
                                    }
                                }
                            }
                            const ImVec2 inner_radius(std::max(0.0f, radius.x - static_cast<float>(bs) * tile_px),
                                                      std::max(0.0f, radius.y - static_cast<float>(bs) * tile_px));
                            draw_list->AddEllipse(center, inner_radius, border_col, 0.0f, 0, 1.0f);
                        }
                        draw_list->AddEllipse(center, radius, border_col, 0.0f, 0, 2.0f);
                    }
                } else {
                    if (g_ed.tool == Tool::Select || g_ed.rect_fill) {
                        draw_list->AddRectFilled(ImVec2(rpx0, rpy0), ImVec2(rpx1, rpy1), fill_col);
                        draw_list->AddRect(ImVec2(rpx0, rpy0), ImVec2(rpx1, rpy1), border_col, 0.0f, 0, 2.0f);
                    } else {
                        const int bs = std::min(g_ed.brush_size, std::min(r.w, r.h));
                        const float inner_x0 = cell_to_screen_x(r.x + bs);
                        const float inner_y0 = cell_to_screen_y(r.y + bs);
                        const float inner_x1 = cell_to_screen_x(r.x + r.w - bs);
                        const float inner_y1 = cell_to_screen_y(r.y + r.h - bs);

                        if (bs * 2 >= r.w || bs * 2 >= r.h) {
                            draw_list->AddRectFilled(ImVec2(rpx0, rpy0), ImVec2(rpx1, rpy1), fill_col);
                        } else {
                            draw_list->AddRectFilled(ImVec2(rpx0, rpy0), ImVec2(rpx1, inner_y0), fill_col); // Top
                            draw_list->AddRectFilled(ImVec2(rpx0, inner_y1), ImVec2(rpx1, rpy1), fill_col); // Bottom
                            draw_list->AddRectFilled(ImVec2(rpx0, inner_y0), ImVec2(inner_x0, inner_y1), fill_col); // Left
                            draw_list->AddRectFilled(ImVec2(inner_x1, inner_y0), ImVec2(rpx1, inner_y1), fill_col); // Right
                            draw_list->AddRect(ImVec2(inner_x0, inner_y0), ImVec2(inner_x1, inner_y1), border_col, 0.0f, 0, 1.0f);
                        }
                        draw_list->AddRect(ImVec2(rpx0, rpy0), ImVec2(rpx1, rpy1), border_col, 0.0f, 0, 2.0f);
                    }
                }
            }
        } else {
            if (g_ed.tool == Tool::Paint || g_ed.tool == Tool::Erase || (g_ed.right_click_erasing && g_ed.tool != Tool::Line && g_ed.tool != Tool::Rect && g_ed.tool != Tool::Select)) {
                g_ed.doc.end_stroke();
                g_ed.stroke_points.clear();
                g_ed.last_painted_cell = {-1, -1};
            } else if (g_ed.tool == Tool::Line) {
                if (g_ed.right_click_erasing) {
                    g_ed.doc.erase_line(g_ed.drag_start.x, g_ed.drag_start.y, cell_x, cell_y, g_ed.brush_size);
                } else {
                    g_ed.doc.draw_line(g_ed.drag_start.x, g_ed.drag_start.y, cell_x, cell_y,
                                       g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row, g_ed.brush_size);
                }
            } else if (g_ed.tool == Tool::Rect) {
                const Rect r = compute_drag_rect(g_ed.drag_start.x, g_ed.drag_start.y, cell_x, cell_y, io.KeyShift);
                if (g_ed.rect_circle) {
                    if (g_ed.rect_fill) {
                        if (g_ed.right_click_erasing) {
                            g_ed.doc.erase_ellipse(r);
                        } else {
                            g_ed.doc.fill_ellipse(r, g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row);
                        }
                    } else {
                        if (g_ed.right_click_erasing) {
                            g_ed.doc.erase_outline_ellipse(r, g_ed.brush_size);
                        } else {
                            g_ed.doc.outline_ellipse(r, g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row, g_ed.brush_size);
                        }
                    }
                } else {
                    if (g_ed.rect_fill) {
                        if (g_ed.right_click_erasing) {
                            g_ed.doc.erase_rect(r);
                        } else {
                            g_ed.doc.fill_rect(r, g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row);
                        }
                    } else {
                        if (g_ed.right_click_erasing) {
                            g_ed.doc.erase_outline_rect(r, g_ed.brush_size);
                        } else {
                            g_ed.doc.outline_rect(r, g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row, g_ed.brush_size);
                        }
                    }
                }
            } else if (g_ed.tool == Tool::Select) {
                const Rect r = compute_drag_rect(g_ed.drag_start.x, g_ed.drag_start.y, cell_x, cell_y, io.KeyShift);
                g_ed.selection = r;
                g_ed.has_selection = true;
                g_ed.selection_lifted = false;
                g_ed.selection_is_circle = g_ed.select_circle;
                g_ed.floating_clip.clear();
                g_ed.doc.set_clip_rect(&g_ed.selection, g_ed.selection_is_circle ? TilemapDoc::ClipShape::Ellipse : TilemapDoc::ClipShape::Rect);
            }
            g_ed.is_drawing = false;
        }
    }

    // Draw active selection box
    if (g_ed.has_selection) {
        const float spx0 = cell_to_screen_x(g_ed.selection.x);
        const float spy0 = cell_to_screen_y(g_ed.selection.y);
        const float spx1 = cell_to_screen_x(g_ed.selection.x + g_ed.selection.w);
        const float spy1 = cell_to_screen_y(g_ed.selection.y + g_ed.selection.h);
        const ImU32 box_col = g_ed.paste_mode ? IM_COL32(80, 220, 255, 255) : (g_ed.selection_lifted ? IM_COL32(255, 210, 50, 255) : IM_COL32(255, 230, 80, 255));
        if (g_ed.selection_is_circle && !g_ed.paste_mode) {
            const ImVec2 scenter((spx0 + spx1) * 0.5f, (spy0 + spy1) * 0.5f);
            const ImVec2 sradius((spx1 - spx0) * 0.5f, (spy1 - spy0) * 0.5f);
            draw_list->AddEllipse(scenter, sradius, box_col, 0.0f, 0, 2.0f);
            if (g_ed.selection_lifted) {
                draw_list->AddEllipse(scenter, ImVec2(sradius.x + 1.0f, sradius.y + 1.0f), IM_COL32(0, 0, 0, 160), 0.0f, 0, 1.0f);
            }
        } else {
            draw_list->AddRect(ImVec2(spx0, spy0), ImVec2(spx1, spy1), box_col, 0.0f, 0, 2.0f);
            if (g_ed.paste_mode) {
                draw_list->AddRectFilled(ImVec2(spx0, spy0), ImVec2(spx1, spy1), IM_COL32(60, 180, 255, 40));
            } else if (g_ed.selection_lifted) {
                draw_list->AddRect(ImVec2(spx0 - 1, spy0 - 1), ImVec2(spx1 + 1, spy1 + 1), IM_COL32(0, 0, 0, 160), 0.0f, 0, 1.0f);
            }
        }
    }

    draw_list->PopClipRect();
}

static void draw_sidebar_content(SDL_Renderer* renderer) {
    const ImVec4 sec_hdr_col = g_ed.settings.dark ? ImVec4(0.4f, 0.75f, 1.0f, 1.0f) : ImVec4(0.12f, 0.45f, 0.85f, 1.0f);

    // 1. Tileset Section
    ImGui::TextColored(sec_hdr_col, "TILESET");
    if (ImGui::Button("Import Tileset...", ImVec2(-1, 28))) {
        open_tileset_dialog(renderer);
    }
    if (!g_ed.doc.tileset.is_valid()) {
        if (ImGui::Button("Create in Tileset Maker...", ImVec2(-1, 26))) {
            switch_to_view(AppView::TilesetMaker, renderer);
        }
    } else {
        if (ImGui::Button("Edit in Tileset Maker...", ImVec2(-1, 26))) {
            switch_to_view(AppView::TilesetMaker, renderer);
        }
    }

    if (g_ed.doc.tileset.is_valid()) {
        auto short_name = [](const std::string& path) -> std::string {
            const auto slash = path.find_last_of("/\\");
            return (slash == std::string::npos) ? path : path.substr(slash + 1);
        };
        std::string label = short_name(g_ed.doc.tileset.png_path);
        if (!g_ed.doc.tileset.terrain_path.empty()) {
            label += " (+" + short_name(g_ed.doc.tileset.terrain_path) + ")";
        }
        ImGui::Text("File: %s", label.c_str());
        ImGui::Text("Tileset: %dx%d (%dpx tiles)", g_ed.doc.tileset.cols, g_ed.doc.tileset.rows, g_ed.doc.tileset.tile_size);
        if (!g_ed.doc.tileset.variants.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "Variants: %d active", static_cast<int>(g_ed.doc.tileset.variants.size()));
        }

        // Mode Switcher: Stamp | Collision | Variants
        ImGui::Spacing();
        {
            const float avail_w = ImGui::GetContentRegionAvail().x;
            const float btn_w = std::floor((avail_w - 8.0f) / 3.0f);
            {
                ScopedStyleColor active_col(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.88f, 1.0f), g_ed.tileset_mode == TilesetSidebarMode::Stamp);
                if (ImGui::Button("Stamp", ImVec2(btn_w, 26))) {
                    g_ed.tileset_mode = TilesetSidebarMode::Stamp;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Select tiles to paint onto the tilemap");
            }
            ImGui::SameLine(0, 4);
            {
                ScopedStyleColor active_col(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.88f, 1.0f), g_ed.tileset_mode == TilesetSidebarMode::Collision);
                if (ImGui::Button("Collision", ImVec2(btn_w, 26))) {
                    g_ed.tileset_mode = TilesetSidebarMode::Collision;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Paint or clear collision types on tileset tiles");
            }
            ImGui::SameLine(0, 4);
            {
                ScopedStyleColor active_col(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.88f, 1.0f), g_ed.tileset_mode == TilesetSidebarMode::Variants);
                if (ImGui::Button("Variants", ImVec2(btn_w, 26))) {
                    g_ed.tileset_mode = TilesetSidebarMode::Variants;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Configure autotile terrain variants and spawn probabilities");
            }
        }

        // Interactive visual palette
        ImGui::Spacing();
        if (g_ed.tileset_texture && g_ed.doc.tileset.cols > 0 && g_ed.doc.tileset.rows > 0) {
            const int cols = g_ed.doc.tileset.cols;
            const int rows = g_ed.doc.tileset.rows;
            const float spacing = 1.0f;
            const ImGuiStyle& style = ImGui::GetStyle();
            const ImVec2 child_pad(4.0f, 4.0f);
            const float border_size = style.ChildBorderSize;

            const float total_gaps_x = (cols > 1) ? static_cast<float>(cols - 1) * spacing : 0.0f;
            const float total_gaps_y = (rows > 1) ? static_cast<float>(rows - 1) * spacing : 0.0f;
            const float outer_w = ImGui::GetContentRegionAvail().x;
            const float extra_controls_h = (g_ed.tileset_mode == TilesetSidebarMode::Stamp) ? 70.0f : 270.0f;
            const float avail_sidebar_h = std::max(100.0f, ImGui::GetContentRegionAvail().y - extra_controls_h);
            const TilesetPreviewLayout layout = compute_tileset_preview_layout(
                outer_w, avail_sidebar_h, cols, rows, spacing,
                child_pad.x, child_pad.y, border_size, style.ScrollbarSize);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, child_pad);
            ImGuiWindowFlags child_flags = ImGuiWindowFlags_None;
            if (!layout.needs_vscroll) {
                child_flags |= ImGuiWindowFlags_NoScrollbar;
            }

            ImGui::BeginChild("AtlasScroll##Grid", ImVec2(0, layout.child_h), ImGuiChildFlags_Borders, child_flags);

            // Re-read exact inner width inside the child window
            const float actual_inner_w = ImGui::GetContentRegionAvail().x;
            const float tile_ui_size = std::max(1.0f, (actual_inner_w - total_gaps_x) / static_cast<float>(cols));
            const float actual_total_w = actual_inner_w;
            const float total_h = static_cast<float>(rows) * tile_ui_size + total_gaps_y;

            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(actual_total_w, total_h));

            const bool grid_hovered = ImGui::IsItemHovered();
            const ImGuiIO& io = ImGui::GetIO();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
            if (platform_io.DrawCallback_SetSamplerNearest != nullptr) {
                dl->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);
            }

            const float sel_thick = std::clamp(tile_ui_size * 0.08f, 1.5f, 2.5f);

            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    const float x0 = p0.x + c * (tile_ui_size + spacing);
                    const float y0 = p0.y + r * (tile_ui_size + spacing);
                    const float x1 = x0 + tile_ui_size;
                    const float y1 = y0 + tile_ui_size;

                    const float u0 = static_cast<float>(c * g_ed.doc.tileset.tile_size) / static_cast<float>(g_ed.texture_w);
                    const float v0 = static_cast<float>(r * g_ed.doc.tileset.tile_size) / static_cast<float>(g_ed.texture_h);
                    const float u1 = static_cast<float>((c + 1) * g_ed.doc.tileset.tile_size) / static_cast<float>(g_ed.texture_w);
                    const float v1 = static_cast<float>((r + 1) * g_ed.doc.tileset.tile_size) / static_cast<float>(g_ed.texture_h);

                    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), g_ed.settings.dark ? IM_COL32(30, 32, 36, 255) : IM_COL32(230, 233, 238, 255));
                    dl->AddImage(reinterpret_cast<ImTextureID>(g_ed.tileset_texture),
                                 ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(u0, v0), ImVec2(u1, v1));

                    if (g_ed.tileset_mode == TilesetSidebarMode::Stamp) {
                        const bool is_stamp_sel = (g_ed.stamp_col == c && g_ed.stamp_row == r);
                        if (is_stamp_sel) {
                            dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(255, 220, 40, 255), 0, 0, sel_thick);
                        } else if (g_ed.doc.tileset.is_extra(c, r)) {
                            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(140, 90, 220, 160), 0, 0, 1.0f);
                        }
                        const uint8_t tile_col = g_ed.doc.tileset.get_tile_collision(c, r);
                        if (tile_col != 0) {
                            const CollisionType* ct = g_ed.doc.get_collision_type(tile_col);
                            const Rgb cr = ct ? ct->color : Rgb{235, 60, 50};
                            const float dot_sz = std::clamp(std::floor(tile_ui_size * 0.25f), 2.0f, 6.0f);
                            const float dot_pad = std::clamp(std::floor(tile_ui_size * 0.05f), 1.0f, 2.0f);
                            dl->AddRectFilled(ImVec2(x1 - dot_sz - dot_pad, y1 - dot_sz - dot_pad),
                                              ImVec2(x1 - dot_pad, y1 - dot_pad),
                                              IM_COL32(cr.r, cr.g, cr.b, 220));
                        }
                    } else if (g_ed.tileset_mode == TilesetSidebarMode::Collision) {
                        if (c == 10 && r == 1) {
                            // Protected empty background tile
                            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(160, 160, 160, 140), 1.0f);
                            dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y0), IM_COL32(160, 160, 160, 140), 1.0f);
                        } else {
                            const uint8_t tile_col = g_ed.doc.tileset.get_tile_collision(c, r);
                            if (tile_col != 0) {
                                const CollisionType* ct = g_ed.doc.get_collision_type(tile_col);
                                const Rgb cr = ct ? ct->color : Rgb{235, 60, 50};
                                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(cr.r, cr.g, cr.b, 90));
                                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(cr.r, cr.g, cr.b, 200), 0, 0, 1.0f);
                                const float dot_sz = std::clamp(std::floor(tile_ui_size * 0.28f), 3.0f, 8.0f);
                                dl->AddRectFilled(ImVec2(x1 - dot_sz - 1, y1 - dot_sz - 1),
                                                  ImVec2(x1 - 1, y1 - 1),
                                                  IM_COL32(cr.r, cr.g, cr.b, 240));
                            }
                        }
                    } else if (g_ed.tileset_mode == TilesetSidebarMode::Variants) {
                        const Cell sel = g_ed.selected_terrain_tile;
                        const bool is_sel = (sel.x == c && sel.y == r);

                        if (Tileset::is_base_origin_tile(c, r)) {
                            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(60, 180, 255, 120), 0, 0, 1.0f);
                        } else if (Tileset::is_variant_tile(c, r)) {
                            const VariantBinding* vb = g_ed.doc.tileset.find_variant(c, r);
                            if (vb) {
                                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(180, 100, 255, 200), 0, 0, 1.5f);
                            } else {
                                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(100, 100, 100, 90), 0, 0, 1.0f);
                            }
                        }

                        if (is_sel) {
                            dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(255, 220, 40, 255), 0, 0, sel_thick + 0.5f);
                        } else if (Tileset::is_base_origin_tile(sel.x, sel.y)) {
                            // If an origin is selected, highlight all its connected variants
                            const VariantBinding* vb = g_ed.doc.tileset.find_variant(c, r);
                            if (vb && vb->root_x == sel.x && vb->root_y == sel.y) {
                                dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(50, 220, 120, 230), 0, 0, sel_thick);
                            }
                        } else if (Tileset::is_variant_tile(sel.x, sel.y)) {
                            // If a variant is selected, highlight its origin
                            const VariantBinding* vb = g_ed.doc.tileset.find_variant(sel.x, sel.y);
                            if (vb && vb->root_x == c && vb->root_y == r) {
                                dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(50, 220, 120, 230), 0, 0, sel_thick);
                            }
                        }
                    }
                }
            }

            if (platform_io.DrawCallback_SetSamplerLinear != nullptr) {
                dl->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
            }

            auto apply_col_line = [&](int x0, int y0, int x1, int y1, uint8_t t) {
                const int dx = std::abs(x1 - x0);
                const int dy = -std::abs(y1 - y0);
                const int sx = x0 < x1 ? 1 : -1;
                const int sy = y0 < y1 ? 1 : -1;
                int err = dx + dy;
                int cx = x0, cy = y0;
                while (true) {
                    if (cx >= 0 && cy >= 0 && cx < cols && cy < rows) {
                        if (g_ed.doc.tileset.get_tile_collision(cx, cy) != t) {
                            g_ed.doc.tileset.set_tile_collision(cx, cy, t);
                            g_ed.doc.mark_dirty();
                        }
                    }
                    if (cx == x1 && cy == y1) break;
                    const int e2 = 2 * err;
                    if (e2 >= dy) { err += dy; cx += sx; }
                    if (e2 <= dx) { err += dx; cy += sy; }
                }
            };

            if (grid_hovered) {
                int hover_c = static_cast<int>((io.MousePos.x - p0.x) / (tile_ui_size + spacing));
                int hover_r = static_cast<int>((io.MousePos.y - p0.y) / (tile_ui_size + spacing));
                hover_c = std::clamp(hover_c, 0, cols - 1);
                hover_r = std::clamp(hover_r, 0, rows - 1);

                const float hx0 = p0.x + hover_c * (tile_ui_size + spacing);
                const float hy0 = p0.y + hover_r * (tile_ui_size + spacing);
                const float hov_thick = std::clamp(tile_ui_size * 0.06f, 1.0f, 2.0f);
                dl->AddRect(ImVec2(hx0, hy0), ImVec2(hx0 + tile_ui_size, hy0 + tile_ui_size),
                            IM_COL32(255, 255, 255, 180), 0, 0, hov_thick);

                if (g_ed.tileset_mode == TilesetSidebarMode::Stamp) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_ed.stamp_col = hover_c;
                        g_ed.stamp_row = hover_r;
                        g_ed.paint_mode = TileMode::Stamp;
                        g_ed.status_msg = "Selected stamp tile (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ")";
                    }
                    const uint8_t tc = g_ed.doc.tileset.get_tile_collision(hover_c, hover_r);
                    const CollisionType* ct = g_ed.doc.get_collision_type(tc);
                    const std::string col_str = (tc == 0) ? "None" : (ct ? ct->name : "Type " + std::to_string(tc));
                    if (hover_c == 10 && hover_r == 1) {
                        ImGui::SetTooltip("Tile (10, 1) [Empty / Background]\nCollision: None (Always empty / no collision)");
                    } else {
                        ImGui::SetTooltip("Tile (%d, %d)%s [Col: %s]", hover_c, hover_r,
                                          g_ed.doc.tileset.is_extra(hover_c, hover_r) ? " [Variant]" : "",
                                          col_str.c_str());
                    }
                } else if (g_ed.tileset_mode == TilesetSidebarMode::Collision) {
                    const bool left_down = io.MouseDown[ImGuiMouseButton_Left];
                    const bool right_down = io.MouseDown[ImGuiMouseButton_Right];
                    if (left_down || right_down) {
                        const uint8_t paint_col_val = left_down ? g_ed.active_collision_type : 0;
                        const int prev_x = (g_ed.last_col_painted.x >= 0) ? g_ed.last_col_painted.x : hover_c;
                        const int prev_y = (g_ed.last_col_painted.y >= 0) ? g_ed.last_col_painted.y : hover_r;
                        apply_col_line(prev_x, prev_y, hover_c, hover_r, paint_col_val);
                        g_ed.last_col_painted = {hover_c, hover_r};
                    } else {
                        g_ed.last_col_painted = {-1, -1};
                    }

                    const uint8_t tc = g_ed.doc.tileset.get_tile_collision(hover_c, hover_r);
                    const CollisionType* ct = g_ed.doc.get_collision_type(tc);
                    const std::string cur_name = (tc == 0) ? "None" : (ct ? ct->name : "Type " + std::to_string(tc));
                    const CollisionType* act = g_ed.doc.get_collision_type(g_ed.active_collision_type);
                    const std::string act_name = (g_ed.active_collision_type == 0) ? "None" : (act ? act->name : "Type " + std::to_string(g_ed.active_collision_type));

                    if (hover_c == 10 && hover_r == 1) {
                        ImGui::SetTooltip("Tile (10, 1) [Empty / Background]\nCollision: None (Protected)");
                    } else {
                        ImGui::SetTooltip("Tile (%d, %d)\nCollision: %s\nLeft-drag: Set to %s\nRight-drag: Clear (None)",
                                          hover_c, hover_r, cur_name.c_str(), act_name.c_str());
                    }
                } else if (g_ed.tileset_mode == TilesetSidebarMode::Variants) {
                    const bool is_shift_down = io.KeyShift || (io.KeyMods & ImGuiMod_Shift);
                    const Cell sel = g_ed.selected_terrain_tile;

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (is_shift_down) {
                            if (Tileset::is_base_origin_tile(sel.x, sel.y)) {
                                if (Tileset::is_variant_tile(hover_c, hover_r)) {
                                    const VariantBinding* cur_vb = g_ed.doc.tileset.find_variant(hover_c, hover_r);
                                    if (cur_vb && cur_vb->root_x == sel.x && cur_vb->root_y == sel.y) {
                                        g_ed.doc.tileset.remove_variant(hover_c, hover_r);
                                        g_ed.doc.mark_dirty();
                                        g_ed.doc.solve_all_autotiles();
                                        g_ed.status_msg = "Disconnected variant (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ") from Origin (" + std::to_string(sel.x) + ", " + std::to_string(sel.y) + ").";
                                    } else {
                                        g_ed.doc.tileset.set_variant(hover_c, hover_r, sel.x, sel.y, g_ed.terrain_variant_prob);
                                        g_ed.doc.mark_dirty();
                                        g_ed.doc.solve_all_autotiles();
                                        g_ed.status_msg = "Attached variant (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ") to Origin (" + std::to_string(sel.x) + ", " + std::to_string(sel.y) + ").";
                                    }
                                } else if (hover_c == sel.x && hover_r == sel.y) {
                                    const int n = g_ed.doc.tileset.count_variants_for_root(sel.x, sel.y);
                                    if (n > 0) {
                                        g_ed.doc.tileset.remove_variants_for_root(sel.x, sel.y);
                                        g_ed.doc.mark_dirty();
                                        g_ed.doc.solve_all_autotiles();
                                        g_ed.status_msg = "Cleared all " + std::to_string(n) + " variants for Origin (" + std::to_string(sel.x) + ", " + std::to_string(sel.y) + ").";
                                    }
                                } else if (Tileset::is_base_origin_tile(hover_c, hover_r)) {
                                    g_ed.selected_terrain_tile = {hover_c, hover_r};
                                }
                            } else if (Tileset::is_variant_tile(sel.x, sel.y)) {
                                if (Tileset::is_base_origin_tile(hover_c, hover_r)) {
                                    const VariantBinding* cur_vb = g_ed.doc.tileset.find_variant(sel.x, sel.y);
                                    if (cur_vb && cur_vb->root_x == hover_c && cur_vb->root_y == hover_r) {
                                        g_ed.doc.tileset.remove_variant(sel.x, sel.y);
                                        g_ed.doc.mark_dirty();
                                        g_ed.doc.solve_all_autotiles();
                                        g_ed.status_msg = "Disconnected variant (" + std::to_string(sel.x) + ", " + std::to_string(sel.y) + ") from Origin (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ").";
                                    } else {
                                        g_ed.doc.tileset.set_variant(sel.x, sel.y, hover_c, hover_r, g_ed.terrain_variant_prob);
                                        g_ed.doc.mark_dirty();
                                        g_ed.doc.solve_all_autotiles();
                                        g_ed.status_msg = "Attached variant (" + std::to_string(sel.x) + ", " + std::to_string(sel.y) + ") to Origin (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ").";
                                    }
                                } else if (Tileset::is_variant_tile(hover_c, hover_r)) {
                                    g_ed.selected_terrain_tile = {hover_c, hover_r};
                                }
                            }
                        } else {
                            g_ed.selected_terrain_tile = {hover_c, hover_r};
                            g_ed.status_msg = "Selected tile (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ")";
                        }
                    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        if (Tileset::is_variant_tile(hover_c, hover_r)) {
                            if (g_ed.doc.tileset.find_variant(hover_c, hover_r)) {
                                g_ed.doc.tileset.remove_variant(hover_c, hover_r);
                                g_ed.doc.mark_dirty();
                                g_ed.doc.solve_all_autotiles();
                                g_ed.status_msg = "Removed variant (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ").";
                            }
                        } else if (Tileset::is_base_origin_tile(hover_c, hover_r)) {
                            const int cnt = g_ed.doc.tileset.count_variants_for_root(hover_c, hover_r);
                            if (cnt > 0) {
                                g_ed.doc.tileset.remove_variants_for_root(hover_c, hover_r);
                                g_ed.doc.mark_dirty();
                                g_ed.doc.solve_all_autotiles();
                                g_ed.status_msg = "Cleared variants for Origin (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ").";
                            }
                        }
                    }

                    if (is_shift_down) {
                        if (Tileset::is_base_origin_tile(sel.x, sel.y)) {
                            if (Tileset::is_variant_tile(hover_c, hover_r)) {
                                const VariantBinding* cur_vb = g_ed.doc.tileset.find_variant(hover_c, hover_r);
                                if (cur_vb && cur_vb->root_x == sel.x && cur_vb->root_y == sel.y) {
                                    ImGui::SetTooltip("SHIFT + Click: Disconnect variant (%d, %d) from Origin (%d, %d)", hover_c, hover_r, sel.x, sel.y);
                                } else {
                                    ImGui::SetTooltip("SHIFT + Click: Attach variant (%d, %d) to Origin (%d, %d)", hover_c, hover_r, sel.x, sel.y);
                                }
                            } else if (hover_c == sel.x && hover_r == sel.y) {
                                const int n = g_ed.doc.tileset.count_variants_for_root(sel.x, sel.y);
                                ImGui::SetTooltip("SHIFT + Click: Clear all %d variant(s) for Origin (%d, %d)", n, sel.x, sel.y);
                            } else {
                                ImGui::SetTooltip("Click: Select Origin (%d, %d)", hover_c, hover_r);
                            }
                        } else if (Tileset::is_variant_tile(sel.x, sel.y)) {
                            if (Tileset::is_base_origin_tile(hover_c, hover_r)) {
                                const VariantBinding* cur_vb = g_ed.doc.tileset.find_variant(sel.x, sel.y);
                                if (cur_vb && cur_vb->root_x == hover_c && cur_vb->root_y == hover_r) {
                                    ImGui::SetTooltip("SHIFT + Click: Disconnect variant (%d, %d) from Origin (%d, %d)", sel.x, sel.y, hover_c, hover_r);
                                } else {
                                    ImGui::SetTooltip("SHIFT + Click: Attach variant (%d, %d) to Origin (%d, %d)", sel.x, sel.y, hover_c, hover_r);
                                }
                            } else {
                                ImGui::SetTooltip("Click: Select tile (%d, %d)", hover_c, hover_r);
                            }
                        } else {
                            ImGui::SetTooltip("Tile (%d, %d) (Click to select)", hover_c, hover_r);
                        }
                    } else {
                        if (Tileset::is_base_origin_tile(hover_c, hover_r)) {
                            const int cnt = g_ed.doc.tileset.count_variants_for_root(hover_c, hover_r);
                            ImGui::SetTooltip("Origin (%d, %d) [%d variant%s]\nClick to select (Shift+Click to connect/disconnect)",
                                              hover_c, hover_r, cnt, cnt == 1 ? "" : "s");
                        } else if (Tileset::is_variant_tile(hover_c, hover_r)) {
                            const VariantBinding* vb = g_ed.doc.tileset.find_variant(hover_c, hover_r);
                            if (vb) {
                                ImGui::SetTooltip("Variant (%d, %d) -> Origin (%d, %d) [%d%%]\nClick to select | R-click to disconnect",
                                                  hover_c, hover_r, vb->root_x, vb->root_y, static_cast<int>(std::round(vb->probability * 100.0f)));
                            } else {
                                ImGui::SetTooltip("Extra Tile (%d, %d) [Unassigned]\nClick to select | Shift+Click with Origin to attach",
                                                  hover_c, hover_r);
                            }
                        }
                    }
                }
            } else {
                if (!io.MouseDown[ImGuiMouseButton_Left] && !io.MouseDown[ImGuiMouseButton_Right]) {
                    g_ed.last_col_painted = {-1, -1};
                }
            }

            ImGui::EndChild();
            ImGui::PopStyleVar();
        } else {
            ImGui::TextDisabled("Tileset preview unavailable.");
        }

        // Sub-panels below tileset preview based on tileset_mode
        if (g_ed.tileset_mode == TilesetSidebarMode::Stamp) {
            ImGui::Spacing();
            ImGui::Text("Selected Stamp: (%d, %d)", g_ed.stamp_col, g_ed.stamp_row);
            ImGui::SameLine();
            if (ImGui::SmallButton("Set Paint to Stamp")) {
                g_ed.paint_mode = TileMode::Stamp;
            }
        } else if (g_ed.tileset_mode == TilesetSidebarMode::Collision) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(sec_hdr_col, "COLLISION PALETTE");

            // Palette buttons: 0: None, and each collision type
            {
                const bool is_none_active = (g_ed.active_collision_type == 0);
                ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.45f, 0.48f, 0.52f, 1.0f), is_none_active);
                if (ImGui::Button("0: None")) {
                    g_ed.active_collision_type = 0;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Clear collision (None / 0)");
                }
            }
            ImGui::SameLine();

            int btn_count = 1;
            for (const auto& ct : g_ed.doc.collision_types) {
                if (btn_count % 4 == 0) {
                    ImGui::NewLine();
                } else {
                    ImGui::SameLine();
                }
                btn_count++;

                const bool is_active = (g_ed.active_collision_type == ct.id);
                const ImVec4 btn_col(ct.color.r / 255.0f, ct.color.g / 255.0f, ct.color.b / 255.0f, is_active ? 1.0f : 0.65f);

                ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, is_active ? btn_col : ImVec4(btn_col.x, btn_col.y, btn_col.z, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, btn_col);
                if (is_active) {
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                }

                const std::string btn_label = std::to_string(ct.id) + ": " + ct.name + "##ColBtn" + std::to_string(ct.id);
                if (ImGui::Button(btn_label.c_str())) {
                    g_ed.active_collision_type = ct.id;
                }

                if (is_active) {
                    ImGui::PopStyleColor();
                    ImGui::PopStyleVar();
                }
                ImGui::PopStyleColor(3);
            }

            if (g_ed.doc.collision_types.size() < 13) {
                ImGui::SameLine();
                if (ImGui::Button("+ Add")) {
                    const uint8_t new_id = g_ed.doc.add_collision_type();
                    if (new_id > 0) {
                        g_ed.active_collision_type = new_id;
                        const CollisionType* added_ct = g_ed.doc.get_collision_type(new_id);
                        g_ed.status_msg = "Added collision type: " + (added_ct ? added_ct->name : ("Type " + std::to_string(new_id)));
                    }
                }
            }

            // Edit active collision type
            CollisionType* active_ct = (g_ed.active_collision_type > 0) ? g_ed.doc.get_collision_type(g_ed.active_collision_type) : nullptr;
            if (active_ct) {
                ImGui::Spacing();
                ImGui::Text("Active Type %d: %s", active_ct->id, active_ct->name.c_str());

                char name_buf[64];
                std::snprintf(name_buf, sizeof(name_buf), "%s", active_ct->name.c_str());
                ImGui::SetNextItemWidth(140);
                if (ImGui::InputText("Name##ColName", name_buf, sizeof(name_buf))) {
                    active_ct->name = name_buf;
                    g_ed.doc.mark_dirty();
                }

                ImGui::SameLine();
                float col_arr[3] = {active_ct->color.r / 255.0f, active_ct->color.g / 255.0f, active_ct->color.b / 255.0f};
                ImGui::SetNextItemWidth(80);
                if (ImGui::ColorEdit3("Color##ColColor", col_arr, ImGuiColorEditFlags_NoInputs)) {
                    active_ct->color.r = static_cast<uint8_t>(std::clamp(col_arr[0] * 255.0f, 0.0f, 255.0f));
                    active_ct->color.g = static_cast<uint8_t>(std::clamp(col_arr[1] * 255.0f, 0.0f, 255.0f));
                    active_ct->color.b = static_cast<uint8_t>(std::clamp(col_arr[2] * 255.0f, 0.0f, 255.0f));
                    g_ed.doc.mark_dirty();
                }
            } else {
                ImGui::Spacing();
                ImGui::TextDisabled("Active Type: 0 (None / Clear)");
            }

            // Batch tools
            ImGui::Spacing();
            const float half_btn_w = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
            if (ImGui::Button("Fill All Tiles", ImVec2(half_btn_w, 24))) {
                for (int r = 0; r < g_ed.doc.tileset.rows; ++r) {
                    for (int c = 0; c < g_ed.doc.tileset.cols; ++c) {
                        g_ed.doc.tileset.set_tile_collision(c, r, g_ed.active_collision_type);
                    }
                }
                g_ed.doc.mark_dirty();
                g_ed.status_msg = "Filled all tiles with collision type " + std::to_string(g_ed.active_collision_type);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Set all tiles to active collision type (tile 10,1 remains None)");
            }

            ImGui::SameLine(0, 4);
            if (ImGui::Button("Clear All Tiles", ImVec2(half_btn_w, 24))) {
                for (int r = 0; r < g_ed.doc.tileset.rows; ++r) {
                    for (int c = 0; c < g_ed.doc.tileset.cols; ++c) {
                        g_ed.doc.tileset.set_tile_collision(c, r, 0);
                    }
                }
                g_ed.doc.mark_dirty();
                g_ed.status_msg = "Cleared collision on all tiles";
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Reset all tiles to None (0)");
            }
        } else if (g_ed.tileset_mode == TilesetSidebarMode::Variants) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(sec_hdr_col, "VARIANTS & TERRAIN MAPPING");

            if (ImGui::Button("Save .terrain File", ImVec2(-1, 26))) {
                std::string save_p = g_ed.doc.tileset.default_terrain_path();
                std::string err = save_terrain_file(g_ed.doc.tileset, save_p);
                if (err.empty()) {
                    g_ed.status_msg = "Saved terrain to " + save_p;
                } else {
                    g_ed.status_msg = "Save terrain failed: " + err;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Save variant mappings to %s", g_ed.doc.tileset.default_terrain_path().c_str());
            }

            const int sx = g_ed.selected_terrain_tile.x;
            const int sy = g_ed.selected_terrain_tile.y;
            if (sx >= 0 && sy >= 0 && g_ed.doc.tileset.in_bounds(sx, sy)) {
                ImGui::Spacing();
                if (Tileset::is_base_origin_tile(sx, sy)) {
                    const int root_vars = g_ed.doc.tileset.count_variants_for_root(sx, sy);
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Selected Origin: (%d, %d)", sx, sy);
                    ImGui::Text("Attached Variants: %d", root_vars);

                    int def_pct = static_cast<int>(std::round(g_ed.terrain_variant_prob * 100.0f));
                    ImGui::SetNextItemWidth(120);
                    if (ImGui::SliderInt("Default Prob##DefProb", &def_pct, 5, 100, "%d%%")) {
                        g_ed.terrain_variant_prob = std::clamp(static_cast<float>(def_pct) / 100.0f, 0.05f, 1.0f);
                    }

                    if (ImGui::Button("Auto-bind Extra Cols (12+)##Origin", ImVec2(-1, 24))) {
                        g_ed.doc.tileset.auto_bind_extra_columns(sx, sy, g_ed.terrain_variant_prob);
                        g_ed.doc.mark_dirty();
                        g_ed.doc.solve_all_autotiles();
                        g_ed.status_msg = "Auto-bound extra columns to Origin (" + std::to_string(sx) + ", " + std::to_string(sy) + ").";
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Automatically bind matching row columns 12+ as variants of this origin tile");
                    }

                    if (root_vars > 0) {
                        if (ImGui::Button("Disconnect All Variants##Origin", ImVec2(-1, 24))) {
                            g_ed.doc.tileset.remove_variants_for_root(sx, sy);
                            g_ed.doc.mark_dirty();
                            g_ed.doc.solve_all_autotiles();
                            g_ed.status_msg = "Cleared variants for Origin (" + std::to_string(sx) + ", " + std::to_string(sy) + ").";
                        }
                    }
                    ImGui::TextDisabled("Hold SHIFT + click extra tile (col 12+) to attach/disconnect.");
                } else if (Tileset::is_variant_tile(sx, sy)) {
                    VariantBinding* vb = g_ed.doc.tileset.find_variant(sx, sy);
                    if (vb) {
                        ImGui::TextColored(ImVec4(0.75f, 0.5f, 1.0f, 1.0f), "Selected Variant: (%d, %d)", sx, sy);
                        ImGui::Text("Origin: (%d, %d)", vb->root_x, vb->root_y);

                        int pct = static_cast<int>(std::round(vb->probability * 100.0f));
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::SliderInt("Probability##VarProb", &pct, 5, 100, "%d%%")) {
                            vb->probability = std::clamp(static_cast<float>(pct) / 100.0f, 0.05f, 1.0f);
                            g_ed.doc.mark_dirty();
                            g_ed.doc.solve_all_autotiles();
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Spawn probability for this variant relative to its origin");
                        }

                        if (ImGui::Button("Disconnect Variant##Var", ImVec2(-1, 24))) {
                            g_ed.doc.tileset.remove_variant(sx, sy);
                            g_ed.doc.mark_dirty();
                            g_ed.doc.solve_all_autotiles();
                            g_ed.status_msg = "Disconnected variant (" + std::to_string(sx) + ", " + std::to_string(sy) + ").";
                        }
                    } else {
                        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Selected Extra Tile: (%d, %d)", sx, sy);
                        ImGui::TextDisabled("Role: Unassigned");
                        ImGui::TextWrapped("Hold SHIFT and click any 12x4 Origin above to attach.");
                    }
                }
            } else {
                ImGui::Spacing();
                ImGui::TextDisabled("Click a tile in the preview above to inspect.");
            }

            // Active Variants List
            ImGui::Spacing();
            ImGui::Text("Active Variants (%zu):", g_ed.doc.tileset.variants.size());
            ImGui::BeginChild("VariantListChild##Sidebar", ImVec2(0, 140), ImGuiChildFlags_Borders);
            if (g_ed.doc.tileset.variants.empty()) {
                ImGui::TextDisabled("No variants configured.");
            } else {
                for (size_t i = 0; i < g_ed.doc.tileset.variants.size(); ++i) {
                    const auto& vb = g_ed.doc.tileset.variants[i];
                    char vlabel[64];
                    std::snprintf(vlabel, sizeof(vlabel), "(%d,%d) -> (%d,%d) %d%%",
                                  vb.x, vb.y, vb.root_x, vb.root_y,
                                  static_cast<int>(std::round(vb.probability * 100.0f)));
                    const bool is_selected = (g_ed.selected_terrain_tile.x == vb.x && g_ed.selected_terrain_tile.y == vb.y);
                    if (ImGui::Selectable(vlabel, is_selected)) {
                        g_ed.selected_terrain_tile = {vb.x, vb.y};
                    }
                    ImGui::SameLine(ImGui::GetWindowWidth() - 35);
                    char del_id[32];
                    std::snprintf(del_id, sizeof(del_id), "X##delvar_%zu", i);
                    if (ImGui::SmallButton(del_id)) {
                        g_ed.doc.tileset.remove_variant(vb.x, vb.y);
                        g_ed.doc.mark_dirty();
                        g_ed.doc.solve_all_autotiles();
                        break;
                    }
                }
            }
            ImGui::EndChild();
        }
    } else {
        ImGui::TextDisabled("No tileset loaded.");
    }

    ImGui::Separator();
    // 2. Map Properties Section
    ImGui::TextColored(sec_hdr_col, "MAP PROPERTIES");
    char name_buf[128];
    std::snprintf(name_buf, sizeof(name_buf), "%s", g_ed.doc.name.c_str());
    if (ImGui::InputText("Level Name", name_buf, sizeof(name_buf))) {
        g_ed.doc.name = name_buf;
        g_ed.doc.mark_dirty();
    }
    ImGui::Text("Size: %dx%d - %dx%d px",
                g_ed.doc.width_8px(), g_ed.doc.height_8px(),
                g_ed.doc.pixel_width(), g_ed.doc.pixel_height());

    if (ImGui::Button("Resize Canvas...", ImVec2(-1, 26))) {
        g_ed.resize_w = g_ed.doc.width_8px();
        g_ed.resize_h = g_ed.doc.height_8px();
        g_ed.show_resize_modal = true;
    }
    if (ImGui::Button("Clear Map", ImVec2(-1, 26))) {
        g_ed.doc.clear_cells();
        g_ed.status_msg = "Cleared map.";
    }

    ImGui::Separator();
    // 3. Export Section
    ImGui::TextColored(sec_hdr_col, "EXPORT");
    if (ImGui::Button("Export Destination...", ImVec2(-1, 26))) {
        nfdu8char_t* out_dir = nullptr;
        const char* def_dir = (std::strlen(g_ed.export_folder) > 0)
            ? g_ed.export_folder
            : (!g_ed.settings.last_export_dir.empty() ? g_ed.settings.last_export_dir.c_str() : nullptr);
        nfdresult_t res = NFD_PickFolderU8(&out_dir, def_dir);
        if (res == NFD_OKAY && out_dir) {
            std::snprintf(g_ed.export_folder, sizeof(g_ed.export_folder), "%s", out_dir);
            g_ed.settings.last_export_dir = out_dir;
            persist_settings();
            NFD_FreePathU8(out_dir);
        }
    }
    if (std::strlen(g_ed.export_folder) > 0) {
        ImGui::TextWrapped("Folder: %s", g_ed.export_folder);
    } else {
        ImGui::TextDisabled("No folder selected (prompts on Export).");
    }
    ImGui::Spacing();

    ImGui::Checkbox("MDE Collision JSON", &g_ed.export_col_json);
    const int types_used = g_ed.doc.build_collision_grid().count_types_used();
    if (types_used > 1) {
        ImGui::BeginDisabled(true);
        bool dummy_bin = false;
        ImGui::Checkbox("Collision BIN (Disabled)", &dummy_bin);
        ImGui::EndDisabled();
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "BIN disabled: >1 collision types used (%d)", types_used);
    } else {
        ImGui::Checkbox("Collision BIN", &g_ed.export_col_bin);
    }
    ImGui::Checkbox("Map JSON", &g_ed.export_map_json);
    ImGui::Checkbox("Tileset Terrain (.terrain)", &g_ed.export_terrain);

    ImGui::Spacing();
    {
        ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.2f, 0.58f, 0.35f, 1.0f));
        if (ImGui::Button("EXPORT", ImVec2(-1, 36))) {
            execute_export();
        }
    }
}

static void draw_modals() {
    if (g_ed.show_resize_modal) {
        ImGui::OpenPopup("Resize Canvas##Modal");
    }
    if (ImGui::BeginPopupModal("Resize Canvas##Modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter new map size in 8x8 tiles:");
        const int f = g_ed.doc.factor();
        ImGui::InputInt("Width", &g_ed.resize_w, f);
        ImGui::InputInt("Height", &g_ed.resize_h, f);
        g_ed.resize_w = clampi(g_ed.resize_w, f, 4096);
        g_ed.resize_h = clampi(g_ed.resize_h, f, 4096);

        const int eff_w8 = ((g_ed.resize_w + f - 1) / f) * f;
        const int eff_h8 = ((g_ed.resize_h + f - 1) / f) * f;
        ImGui::Text("Current: %dx%d - %dx%d px",
                    g_ed.doc.width_8px(), g_ed.doc.height_8px(),
                    g_ed.doc.pixel_width(), g_ed.doc.pixel_height());
        ImGui::Text("New:     %dx%d - %dx%d px",
                    eff_w8, eff_h8, eff_w8 * 8, eff_h8 * 8);

        ImGui::Separator();
        ImGui::Text("Anchor position:");

        const char* anchors[3][3] = {
            {"TL", "T", "TR"},
            {"L",  "C", "R"},
            {"BL", "B", "BR"}
        };
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (x > -1) ImGui::SameLine();
                ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.9f, 1.0f),
                                     g_ed.resize_anchor_x == x && g_ed.resize_anchor_y == y);
                char bid[16];
                std::snprintf(bid, sizeof(bid), "%s##anch_%d_%d", anchors[y + 1][x + 1], x, y);
                if (ImGui::Button(bid, ImVec2(36, 28))) {
                    g_ed.resize_anchor_x = x;
                    g_ed.resize_anchor_y = y;
                }
            }
        }

        const bool losing = g_ed.doc.would_lose_tiles_8px(g_ed.resize_w, g_ed.resize_h, g_ed.resize_anchor_x, g_ed.resize_anchor_y);
        if (losing) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Warning: Shrinking will cut off placed tiles!");
        }

        ImGui::Spacing();
        if (ImGui::Button("Apply", ImVec2(100, 28))) {
            g_ed.doc.resize_8px(g_ed.resize_w, g_ed.resize_h, g_ed.resize_anchor_x, g_ed.resize_anchor_y);
            g_ed.show_resize_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 28))) {
            g_ed.show_resize_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_ed.show_new_modal) {
        ImGui::OpenPopup("New Map##Modal");
    }
    if (ImGui::BeginPopupModal("New Map##Modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Map Name", g_ed.new_name, sizeof(g_ed.new_name));
        const int f = (g_ed.new_tile_size == 16) ? 2 : 1;
        ImGui::InputInt("Width (8x8 tiles)", &g_ed.new_w, f);
        ImGui::InputInt("Height (8x8 tiles)", &g_ed.new_h, f);
        g_ed.new_w = clampi(g_ed.new_w, f, 4096);
        g_ed.new_h = clampi(g_ed.new_h, f, 4096);

        ImGui::Text("Tile Size:");
        ImGui::RadioButton("8x8", &g_ed.new_tile_size, 8);
        ImGui::SameLine();
        ImGui::RadioButton("16x16", &g_ed.new_tile_size, 16);

        const int eff_w8 = ((g_ed.new_w + f - 1) / f) * f;
        const int eff_h8 = ((g_ed.new_h + f - 1) / f) * f;
        ImGui::Spacing();
        ImGui::Text("Size: %dx%d - %dx%d px", eff_w8, eff_h8, eff_w8 * 8, eff_h8 * 8);

        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(100, 28))) {
            g_ed.doc.reset_8px(g_ed.new_w, g_ed.new_h, g_ed.new_tile_size);
            g_ed.doc.name = g_ed.new_name;
            g_ed.current_map_path.clear();
            g_ed.show_new_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 28))) {
            g_ed.show_new_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

int run_editor() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    NFD_Init();
    ensure_config_dir();
    load_settings_file(g_ed.settings, settings_path());

    if (!g_ed.settings.last_export_dir.empty()) {
        std::snprintf(g_ed.export_folder, sizeof(g_ed.export_folder), "%s", g_ed.settings.last_export_dir.c_str());
    }
    if (g_ed.settings.sidebar_w >= 200.0f) {
        g_ed.sidebar_w = g_ed.settings.sidebar_w;
    }
    if (g_ed.settings.zoom >= 0.25f && g_ed.settings.zoom <= 16.0f) {
        g_ed.zoom = g_ed.settings.zoom;
    }
    // Tool options shift back to clean defaults on app startup
    g_ed.tool = Tool::Paint;
    g_ed.paint_mode = TileMode::Terrain;
    g_ed.brush_size = 1;
    g_ed.stamp_col = 9;
    g_ed.stamp_row = 2;
    g_ed.rect_fill = true;
    g_ed.rect_circle = false;
    g_ed.tileset_mode = TilesetSidebarMode::Stamp;

    const int win_w = (g_ed.settings.window_w >= 640) ? g_ed.settings.window_w : 1280;
    const int win_h = (g_ed.settings.window_h >= 480) ? g_ed.settings.window_h : 800;

    SDL_Window* window = SDL_CreateWindow("Tilemap Maker", win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    s_window = window;

    if (g_ed.settings.window_placed &&
        window_rect_visible(g_ed.settings.window_x, g_ed.settings.window_y, win_w, win_h)) {
        SDL_SetWindowPosition(window, g_ed.settings.window_x, g_ed.settings.window_y);
    }
    if (g_ed.settings.window_maximized) {
        SDL_MaximizeWindow(window);
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    if (!g_ed.settings.last_tileset_path.empty()) {
        if (g_ed.doc.tileset.load_from_file(g_ed.settings.last_tileset_path)) {
            const int old_ts = g_ed.doc.tile_size;
            const int new_ts = g_ed.doc.tileset.tile_size;
            if (old_ts != new_ts) {
                const int w8 = g_ed.doc.width_8px();
                const int h8 = g_ed.doc.height_8px();
                g_ed.doc.reset_8px(w8, h8, new_ts);
            }
            update_tileset_texture(renderer);
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Disable floating docking windows and disable ini file so everything stays fixed inside the main window
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    apply_app_theme(g_ed.settings.dark);
    g_ed.tileset_editor.embedded = true;
    g_ed.tileset_editor.settings.dark = g_ed.settings.dark;
    g_ed.tileset_editor.settings.scale = g_ed.settings.scale;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    bool running = true;
    while (running) {
        g_ed.has_pinch = false;
        g_ed.pinch_scale = 1.0f;
        g_ed.pending_mouse_moves.clear();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                g_ed.pending_mouse_moves.push_back(ImVec2(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y)));
            }
            if (event.type == SDL_EVENT_PINCH_UPDATE && event.pinch.scale > 0.0f) {
                g_ed.pinch_scale *= event.pinch.scale;
                g_ed.has_pinch = true;
            }
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            }
        }

        const SDL_WindowFlags win_flags = SDL_GetWindowFlags(window);
        if (win_flags & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(20);
            continue;
        }
        if (!(win_flags & (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS))) {
            SDL_Delay(32);
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Synchronize clip rect with active selection
        g_ed.doc.set_clip_rect(g_ed.has_selection ? &g_ed.selection : nullptr, g_ed.selection_is_circle ? TilemapDoc::ClipShape::Ellipse : TilemapDoc::ClipShape::Rect);

        // Global shortcuts (processed after NewFrame so input events are current)
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
                switch_to_view(AppView::Tilemap, renderer);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                switch_to_view(AppView::TilesetMaker, renderer);
            }
        }

        if (g_ed.current_view == AppView::Tilemap) {
            if (!io.WantTextInput) {
                const bool cmd = io.KeyCtrl || io.KeySuper;

                if (cmd && ImGui::IsKeyPressed(ImGuiKey_D)) {
                    deselect();
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_0)) {
                    g_ed.zoom = 1.0f;
                }
                if (cmd && (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))) {
                    g_ed.zoom = std::min(16.0f, g_ed.zoom * 1.25f);
                }
                if (cmd && (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))) {
                    g_ed.zoom = std::max(0.25f, g_ed.zoom / 1.25f);
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_Z)) {
                    if (g_ed.selection_lifted) {
                        cancel_or_deselect();
                    } else if (io.KeyShift) {
                        g_ed.doc.redo();
                    } else {
                        g_ed.doc.undo();
                    }
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_Y)) {
                    if (!g_ed.selection_lifted) {
                        g_ed.doc.redo();
                    }
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_S)) {
                    save_map_dialog();
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_O)) {
                    open_map_dialog(renderer);
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_N)) {
                    g_ed.show_new_modal = true;
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_E)) {
                    execute_export();
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_C) && g_ed.has_selection && !g_ed.paste_mode) {
                    copy_selection();
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_X) && g_ed.has_selection && !g_ed.paste_mode) {
                    cut_selection();
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_V) && !g_ed.clipboard.is_empty()) {
                    start_paste();
                }

                // Keyboard navigation for floating paste or selection
                if (g_ed.paste_mode) {
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                        commit_paste();
                    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                        cancel_paste();
                    } else {
                        int dx = 0;
                        int dy = 0;
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  dx -= 1;
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) dx += 1;
                        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))    dy -= 1;
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))  dy += 1;
                        if (dx != 0 || dy != 0) {
                            const int min_x = -g_ed.doc.buffer;
                            const int min_y = -g_ed.doc.buffer;
                            const int max_x = g_ed.doc.width + g_ed.doc.buffer - g_ed.clipboard.w;
                            const int max_y = g_ed.doc.height + g_ed.doc.buffer - g_ed.clipboard.h;
                            g_ed.paste_pos.x = std::clamp(g_ed.paste_pos.x + dx, min_x, std::max(min_x, max_x));
                            g_ed.paste_pos.y = std::clamp(g_ed.paste_pos.y + dy, min_y, std::max(min_y, max_y));
                            g_ed.selection.x = g_ed.paste_pos.x;
                            g_ed.selection.y = g_ed.paste_pos.y;
                            g_ed.doc.set_clip_rect(&g_ed.selection, TilemapDoc::ClipShape::Rect);
                        }
                    }
                } else {
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                        cancel_or_deselect();
                    }
                    if (g_ed.selection_lifted && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
                        apply_moved_selection();
                    }
                    if ((ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) && g_ed.has_selection) {
                        delete_selection();
                    }
                    if (g_ed.has_selection && !g_ed.is_moving_selection) {
                        int dx = 0;
                        int dy = 0;
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  dx -= 1;
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) dx += 1;
                        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))    dy -= 1;
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))  dy += 1;
                        if (dx != 0 || dy != 0) {
                            const int min_x = -g_ed.doc.buffer;
                            const int min_y = -g_ed.doc.buffer;
                            const int max_x = g_ed.doc.width + g_ed.doc.buffer - g_ed.selection.w;
                            const int max_y = g_ed.doc.height + g_ed.doc.buffer - g_ed.selection.h;
                            const int target_x = std::clamp(g_ed.selection.x + dx, min_x, std::max(min_x, max_x));
                            const int target_y = std::clamp(g_ed.selection.y + dy, min_y, std::max(min_y, max_y));
                            if (target_x != g_ed.selection.x || target_y != g_ed.selection.y) {
                                if (!g_ed.selection_lifted) {
                                    g_ed.selection_origin = g_ed.selection;
                                    if (g_ed.selection_is_circle) {
                                        g_ed.floating_clip = g_ed.doc.copy_ellipse(g_ed.selection);
                                        g_ed.doc.begin_stroke("Move Selection");
                                        g_ed.doc.erase_ellipse(g_ed.selection);
                                    } else {
                                        g_ed.floating_clip = g_ed.doc.copy_rect(g_ed.selection);
                                        g_ed.doc.begin_stroke("Move Selection");
                                        g_ed.doc.erase_rect(g_ed.selection);
                                    }
                                    g_ed.selection_lifted = true;
                                }
                                g_ed.selection.x = target_x;
                                g_ed.selection.y = target_y;
                                g_ed.doc.set_clip_rect(&g_ed.selection, g_ed.selection_is_circle ? TilemapDoc::ClipShape::Ellipse : TilemapDoc::ClipShape::Rect);
                                g_ed.status_msg = "Moved selection (floating). Press Ctrl+D or right-click to apply.";
                            }
                        }
                    }
                }

                if (!cmd) {
                    if (ImGui::IsKeyPressed(ImGuiKey_1)) g_ed.tool = Tool::Paint;
                    if (ImGui::IsKeyPressed(ImGuiKey_2)) g_ed.tool = Tool::Erase;
                    if (ImGui::IsKeyPressed(ImGuiKey_3)) g_ed.tool = Tool::Line;
                    if (ImGui::IsKeyPressed(ImGuiKey_4)) g_ed.tool = Tool::Rect;
                    if (ImGui::IsKeyPressed(ImGuiKey_5)) g_ed.tool = Tool::Fill;
                    if (ImGui::IsKeyPressed(ImGuiKey_6)) g_ed.tool = Tool::Select;
                    if (ImGui::IsKeyPressed(ImGuiKey_7)) g_ed.tool = Tool::Eyedropper;
                }
            }
        } else {
            // Tileset Maker View shortcuts
            g_ed.tileset_editor.handle_shortcuts(io);
            if (!io.WantTextInput) {
                const bool cmd = io.KeyCtrl || io.KeySuper;
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_S)) {
                    if (io.KeyShift) g_ed.tileset_editor.save_project(true);
                    else g_ed.tileset_editor.save_project(false);
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_O)) {
                    g_ed.tileset_editor.try_open_project_dialog();
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_N)) {
                    g_ed.tileset_editor.ui.show_new = true;
                    g_ed.tileset_editor.ui.new_focus_name = true;
                }
                if (cmd && ImGui::IsKeyPressed(ImGuiKey_E)) {
                    g_ed.tileset_editor.export_all();
                }
            }
        }

        // Main Menu Bar
        if (ImGui::BeginMainMenuBar()) {
            if (g_ed.current_view == AppView::Tilemap) {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("New Map...", "Ctrl+N")) {
                        g_ed.show_new_modal = true;
                    }
                    if (ImGui::MenuItem("Open Map...", "Ctrl+O")) {
                        open_map_dialog(renderer);
                    }
                    if (ImGui::MenuItem("Save Map", "Ctrl+S")) {
                        save_map_dialog();
                    }
                    if (ImGui::MenuItem("Import Tileset...", "Ctrl+I")) {
                        open_tileset_dialog(renderer);
                    }
                    if (ImGui::MenuItem("Edit in Tileset Maker...", "F2")) {
                        switch_to_view(AppView::TilesetMaker, renderer);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Export", "Ctrl+E")) {
                        execute_export();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Quit", "Cmd+Q")) {
                        running = false;
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Edit")) {
                    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, g_ed.selection_lifted || g_ed.doc.can_undo())) {
                        if (g_ed.selection_lifted) cancel_or_deselect();
                        else g_ed.doc.undo();
                    }
                    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !g_ed.selection_lifted && g_ed.doc.can_redo())) {
                        g_ed.doc.redo();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Cut", "Ctrl+X", false, g_ed.has_selection && !g_ed.paste_mode)) {
                        cut_selection();
                    }
                    if (ImGui::MenuItem("Copy", "Ctrl+C", false, g_ed.has_selection && !g_ed.paste_mode)) {
                        copy_selection();
                    }
                    if (ImGui::MenuItem("Paste", "Ctrl+V", false, !g_ed.clipboard.is_empty())) {
                        start_paste();
                    }
                    if (ImGui::MenuItem("Clear Selection", "Del", false, g_ed.has_selection && !g_ed.paste_mode)) {
                        delete_selection();
                    }
                    if (ImGui::MenuItem("Deselect", "Ctrl+D", false, g_ed.has_selection || g_ed.paste_mode)) {
                        deselect();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Resize Canvas...")) {
                        g_ed.resize_w = g_ed.doc.width_8px();
                        g_ed.resize_h = g_ed.doc.height_8px();
                        g_ed.show_resize_modal = true;
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View")) {
                    if (ImGui::MenuItem("Map Editor", "F1", true)) {
                        switch_to_view(AppView::Tilemap, renderer);
                    }
                    if (ImGui::MenuItem("Tileset Maker", "F2", false)) {
                        switch_to_view(AppView::TilesetMaker, renderer);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Grid Lines", nullptr, g_ed.settings.grid_lines)) {
                        g_ed.settings.grid_lines = !g_ed.settings.grid_lines;
                        persist_settings();
                    }
                    if (ImGui::MenuItem("Collision Overlay", nullptr, g_ed.settings.collision_overlay)) {
                        g_ed.settings.collision_overlay = !g_ed.settings.collision_overlay;
                        persist_settings();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Dark Theme", nullptr, g_ed.settings.dark)) {
                        g_ed.settings.dark = true;
                        apply_app_theme(true);
                        persist_settings();
                    }
                    if (ImGui::MenuItem("Light Theme", nullptr, !g_ed.settings.dark)) {
                        g_ed.settings.dark = false;
                        apply_app_theme(false);
                        persist_settings();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Reset Zoom", "100%")) {
                        g_ed.zoom = 1.0f;
                        persist_settings();
                    }
                    if (ImGui::MenuItem("Fit Map in View")) {
                        g_ed.zoom = 2.0f;
                        g_ed.pan = ImVec2(60, 40);
                        persist_settings();
                    }
                    ImGui::EndMenu();
                }
            } else {
                if (ImGui::BeginMenu("File")) {
                    if (ImGui::MenuItem("Apply & Return to Map", "F1")) {
                        switch_to_view(AppView::Tilemap, renderer);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("New Project...", "Ctrl+N")) {
                        g_ed.tileset_editor.ui.show_new = true;
                        g_ed.tileset_editor.ui.new_focus_name = true;
                    }
                    if (ImGui::MenuItem("Open Project...", "Ctrl+O")) {
                        g_ed.tileset_editor.try_open_project_dialog();
                    }
                    if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
                        g_ed.tileset_editor.save_project();
                    }
                    if (ImGui::MenuItem("Save Project As...", "Shift+Ctrl+S")) {
                        g_ed.tileset_editor.save_project(true);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Import 12x4 Tileset...")) {
                        g_ed.tileset_editor.try_import_12x4_dialog();
                    }
                    if (ImGui::MenuItem("Import 5x3 Tileset...")) {
                        g_ed.tileset_editor.try_import_5x3_dialog();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Export All...", "Ctrl+E")) {
                        g_ed.tileset_editor.export_all();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Quit", "Cmd+Q")) {
                        running = false;
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Edit")) {
                    if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
                        if (g_ed.tileset_editor.floating) g_ed.tileset_editor.cancel_floating();
                        else g_ed.tileset_editor.do_undo();
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Cut", "Ctrl+X", false, g_ed.tileset_editor.has_selection())) {
                        g_ed.tileset_editor.start_selection_move();
                    }
                    if (ImGui::MenuItem("Copy", "Ctrl+C", false, g_ed.tileset_editor.has_selection())) {
                        g_ed.tileset_editor.copy_selection();
                    }
                    if (ImGui::MenuItem("Paste", "Ctrl+V", false, g_ed.tileset_editor.clipboard.valid())) {
                        g_ed.tileset_editor.start_paste();
                    }
                    if (ImGui::MenuItem("Clear Selection", "Del", false, g_ed.tileset_editor.has_selection())) {
                        g_ed.tileset_editor.clear_selection();
                    }
                    if (ImGui::MenuItem("Deselect", "Ctrl+D", false, g_ed.tileset_editor.has_selection())) {
                        g_ed.tileset_editor.clear_selection();
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View")) {
                    if (ImGui::MenuItem("Map Editor", "F1", false)) {
                        switch_to_view(AppView::Tilemap, renderer);
                    }
                    if (ImGui::MenuItem("Tileset Maker", "F2", true)) {
                        switch_to_view(AppView::TilesetMaker, renderer);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Pixel Grid", nullptr, g_ed.tileset_editor.settings.pixel_grid)) {
                        g_ed.tileset_editor.settings.pixel_grid = !g_ed.tileset_editor.settings.pixel_grid;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Dark Theme", nullptr, g_ed.settings.dark)) {
                        g_ed.settings.dark = true;
                        apply_app_theme(true);
                        persist_settings();
                    }
                    if (ImGui::MenuItem("Light Theme", nullptr, !g_ed.settings.dark)) {
                        g_ed.settings.dark = false;
                        apply_app_theme(false);
                        persist_settings();
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndMainMenuBar();
        }

        // Single Fullscreen Parent Window: locks the entire UI into one clean layout
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("MainLayout##Window", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

        // 1. Top row: Mode switcher, view toggles, zoom
        draw_top_nav_and_view_row(renderer);
        ImGui::Separator();

        if (g_ed.current_view == AppView::Tilemap) {
            // 2. Second row: All tools
            draw_tool_selection_row();
            ImGui::Separator();

            // 3. Third row: Contextual tool options
            draw_tool_options_row();
            ImGui::Separator();

            // 4. Body: Left Canvas & Right Sidebar
            const float splitter_w = 6.0f;
            const float status_bar_h = 24.0f;
            const float avail_w = ImGui::GetContentRegionAvail().x;
            const float avail_h = ImGui::GetContentRegionAvail().y - status_bar_h;

            const float min_sidebar = 300.0f;
            const float min_canvas = 300.0f;
            g_ed.sidebar_w = std::clamp(g_ed.sidebar_w, min_sidebar, std::max(min_sidebar, avail_w - min_canvas - splitter_w));
            const float canvas_w = std::max(min_canvas, avail_w - g_ed.sidebar_w - splitter_w);

            // Canvas Panel
            ImGui::BeginChild("CanvasChildPanel", ImVec2(canvas_w, avail_h), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            draw_canvas_viewport_content();
            ImGui::EndChild();

            // Splitter
            ImGui::SameLine(0, 0);
            ImGui::InvisibleButton("vsplit", ImVec2(splitter_w, avail_h));
            if (ImGui::IsItemActive()) {
                g_ed.sidebar_w -= io.MouseDelta.x;
                g_ed.settings.sidebar_w = g_ed.sidebar_w;
            }
            if (ImGui::IsItemDeactivated()) {
                persist_settings();
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            const ImU32 split_col = g_ed.settings.dark ? IM_COL32(50, 54, 62, 255) : IM_COL32(195, 200, 210, 255);
            ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), split_col);

            // Sidebar Panel
            ImGui::SameLine(0, 0);
            ImGui::BeginChild("SidebarChildPanel", ImVec2(g_ed.sidebar_w, avail_h), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            draw_sidebar_content(renderer);
            ImGui::EndChild();

            // 5. Status bar at bottom
            ImGui::Separator();
            ImGui::Text("%s", g_ed.status_msg.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 360);
            if (g_ed.doc.in_bounds(g_ed.hovered_cell.x, g_ed.hovered_cell.y)) {
                if (g_ed.doc.in_active_bounds(g_ed.hovered_cell.x, g_ed.hovered_cell.y)) {
                    ImGui::Text("Cell: (%d, %d) | Map: %dx%d - %dx%d px", g_ed.hovered_cell.x, g_ed.hovered_cell.y,
                                g_ed.doc.width_8px(), g_ed.doc.height_8px(),
                                g_ed.doc.pixel_width(), g_ed.doc.pixel_height());
                } else {
                    ImGui::Text("Cell: (%d, %d) [Outside Buffer] | Map: %dx%d - %dx%d px", g_ed.hovered_cell.x, g_ed.hovered_cell.y,
                                g_ed.doc.width_8px(), g_ed.doc.height_8px(),
                                g_ed.doc.pixel_width(), g_ed.doc.pixel_height());
                }
            } else {
                ImGui::Text("Map: %dx%d - %dx%d px",
                            g_ed.doc.width_8px(), g_ed.doc.height_8px(),
                            g_ed.doc.pixel_width(), g_ed.doc.pixel_height());
            }
        } else {
            // Tileset Maker View
            const float status_bar_h = 24.0f;
            const float avail_h = ImGui::GetContentRegionAvail().y - status_bar_h;
            g_ed.tileset_editor.draw_content(renderer, window, avail_h);

            // Check if return was requested from inside TilesetEditor
            if (g_ed.tileset_editor.request_return_to_map) {
                g_ed.tileset_editor.request_return_to_map = false;
                switch_to_view(AppView::Tilemap, renderer);
            }

            // Status bar at bottom
            ImGui::Separator();
            ImGui::Text("%s", g_ed.tileset_editor.status.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 360);
            ImGui::Text("Tileset: %s (%dpx)", g_ed.tileset_editor.project_name, g_ed.tileset_editor.doc.tile_size);
        }

        ImGui::End(); // MainLayout##Window

        if (g_ed.current_view == AppView::Tilemap) {
            draw_modals();
        } else {
            g_ed.tileset_editor.draw_modals(running);
        }

        // Render Frame
        ImGui::Render();
        const Rgb bg = background_clear_color(g_ed.settings.dark);
        SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    persist_settings();
    s_window = nullptr;

    if (g_ed.tileset_texture) {
        SDL_DestroyTexture(g_ed.tileset_texture);
        g_ed.tileset_texture = nullptr;
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    NFD_Quit();
    SDL_Quit();
    return 0;
}

} // namespace tmm
