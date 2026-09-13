#include "editor.h"
#include "settings.h"
#include "theme.h"

#include "core/collision.h"
#include "core/io.h"
#include "core/tilemap_doc.h"
#include "core/tileset.h"
#include "core/types.h"

#include "../deps/tileset-maker-thingy/src/app/tileset_editor.h"
#include "../deps/tileset-maker-thingy/src/app/settings.h"
#include "../deps/tileset-maker-thingy/src/core/convert.h"
#include "../deps/tileset-maker-thingy/src/core/io.h"
#include "../deps/tileset-maker-thingy/src/core/project.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "nfd.h"

#include <SDL3/SDL.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include "wasm/web_file_io.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace tmm {

enum class AppView { Tilemap, TilesetMaker };
enum class Tool { Paint, Line, Erase, Rect, Fill, Select, Eyedropper };
enum class TilesetSidebarMode { Stamp, Collision, Variants };
enum class SidebarPage { Tileset = 0, MapProperties = 1, Export = 2 };

struct EditorState {
    AppView current_view = AppView::Tilemap;
    tsm::TilesetEditor tileset_editor;

    TilemapDoc doc;
    Settings settings;

    SidebarPage sidebar_page = SidebarPage::Tileset;
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
    ImVec2 pinch_center = ImVec2(-1, -1);
    bool has_pinch_center = false;

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
    bool new_empty_tileset = false;

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
    bool export_tileset_png = true;
    bool export_tileset_proj = true;

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
        g_ed.texture_w = 0;
        g_ed.texture_h = 0;
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

static void restore_project_data_into_editor(const tsm::ProjectData& data, const std::string& path) {
    g_ed.tileset_editor.doc.restore(data.tileset);
    if (data.has_atlas) {
        g_ed.tileset_editor.atlas.restore(data.atlas);
    } else {
        g_ed.tileset_editor.atlas.reset(data.tileset.tile_size);
    }
    g_ed.tileset_editor.has_atlas = data.has_atlas;
    g_ed.tileset_editor.art_rev = data.art_rev;
    g_ed.tileset_editor.atlas_rev = data.has_atlas ? data.art_rev : data.atlas_rev;
    switch (data.step) {
        case tsm::ProjectStep::Edges: g_ed.tileset_editor.step = tsm::Step::Edges; break;
        case tsm::ProjectStep::Specialty: g_ed.tileset_editor.step = tsm::Step::Specialty; break;
        case tsm::ProjectStep::Variants: g_ed.tileset_editor.step = tsm::Step::Variants; break;
        case tsm::ProjectStep::Center:
        default: g_ed.tileset_editor.step = tsm::Step::Center; break;
    }
    g_ed.tileset_editor.seeded = data.seeded;
    g_ed.tileset_editor.stamped = data.stamped;
    g_ed.tileset_editor.specialty = data.specialty;
    g_ed.tileset_editor.atlas_cell = data.atlas_cell;
    g_ed.tileset_editor.preview_sel = data.preview_sel;
    g_ed.tileset_editor.tile_mode = data.tile_mode;
    g_ed.tileset_editor.export_header = data.export_header;
    g_ed.tileset_editor.export_terrain = data.export_terrain;
    g_ed.tileset_editor.export_5x3 = data.export_5x3;
    std::snprintf(g_ed.tileset_editor.project_name, sizeof(g_ed.tileset_editor.project_name), "%s", data.name.c_str());
    g_ed.tileset_editor.project_path = path;
    g_ed.tileset_editor.status = "Loaded project: " + path;
    g_ed.tileset_editor.dirty = false;
    g_ed.tileset_editor.ui.project_open = true;
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
            // If there is an existing tileset, import the project, PNG or terrain (which automatically finds companions)
            bool imported = false;
            if (g_ed.doc.tileset.png_path.size() >= 12 &&
                g_ed.doc.tileset.png_path.substr(g_ed.doc.tileset.png_path.size() - 12) == ".tilesetproj") {
                if (g_ed.tileset_editor.ui.project_open && g_ed.tileset_editor.project_path == g_ed.doc.tileset.png_path) {
                    g_ed.tileset_editor.configure_view();
                    imported = true;
                } else if (file_exists(g_ed.doc.tileset.png_path)) {
                    tsm::ProjectData data;
                    if (tsm::load_project(data, g_ed.doc.tileset.png_path).empty()) {
                        restore_project_data_into_editor(data, g_ed.doc.tileset.png_path);
                        g_ed.tileset_editor.configure_view();
                        imported = true;
                    }
                }
            }

            if (!imported) {
                std::string path_to_import;
                if (!g_ed.doc.tileset.terrain_path.empty() && file_exists(g_ed.doc.tileset.terrain_path)) {
                    path_to_import = g_ed.doc.tileset.terrain_path;
                } else if (!g_ed.doc.tileset.png_path.empty() && file_exists(g_ed.doc.tileset.png_path)) {
                    path_to_import = g_ed.doc.tileset.png_path;
                }

                if (!path_to_import.empty()) {
                    imported = g_ed.tileset_editor.import_12x4(path_to_import);
                }
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
        g_ed.settings.scale = g_ed.tileset_editor.settings.scale;
        g_ed.settings.dark = g_ed.tileset_editor.settings.dark;
        g_ed.current_view = AppView::Tilemap;
    }
}

static float s_applied_scale = -1.0f;
static int s_applied_dark = -1;

static void persist_settings(SDL_Window* window = nullptr);

static void apply_app_theme(bool dark) {
    tmm::apply_theme(dark, g_ed.settings.scale);
    g_ed.tileset_editor.settings.dark = dark;
    g_ed.tileset_editor.settings.scale = g_ed.settings.scale;
    s_applied_scale = g_ed.settings.scale;
    s_applied_dark = dark ? 1 : 0;
}

static void set_ui_scale(float scale) {
    scale = std::clamp(scale, 0.75f, 2.0f);
    if (std::abs(g_ed.settings.scale - scale) > 0.0001f) {
        g_ed.settings.scale = scale;
        g_ed.tileset_editor.settings.scale = scale;
        apply_app_theme(g_ed.settings.dark);
        persist_settings();
    }
}

static SDL_Window* s_window = nullptr;
static SDL_Renderer* s_renderer = nullptr;
static bool s_running = true;

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

static void persist_settings(SDL_Window* window) {
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
    g_ed.settings.sidebar_page = static_cast<int>(g_ed.sidebar_page);
    if (!g_ed.current_map_path.empty()) {
        g_ed.settings.last_map_path = g_ed.current_map_path;
    }
    if (std::strlen(g_ed.export_folder) > 0) {
        g_ed.settings.last_export_dir = g_ed.export_folder;
    }
    g_ed.settings.export_terrain = g_ed.export_terrain;
    g_ed.settings.export_tileset_png = g_ed.export_tileset_png;
    g_ed.settings.export_tileset_proj = g_ed.export_tileset_proj;
    ensure_config_dir();
    save_settings_file(g_ed.settings, settings_path());

    // Also keep tileset maker standalone config synchronized with current scale and theme
    tsm::Settings tsm_s;
    tsm::load_settings_file(tsm_s, tsm::settings_path());
    tsm_s.scale = g_ed.settings.scale;
    tsm_s.dark = g_ed.settings.dark;
    tsm::ensure_config_dir();
    tsm::save_settings_file(tsm_s, tsm::settings_path());
}

static bool load_tileset_from_path(const std::string& path, SDL_Renderer* renderer) {
    if (path.size() >= 12 && path.substr(path.size() - 12) == ".tilesetproj") {
        tsm::ProjectData data;
        const std::string err = tsm::load_project(data, path);
        if (!err.empty()) {
            g_ed.status_msg = "Error loading tileset project: " + err;
            return false;
        }
        restore_project_data_into_editor(data, path);

        std::string ensure_err = g_ed.tileset_editor.ensure_atlas();
        if (!ensure_err.empty()) {
            g_ed.status_msg = "Error preparing tileset atlas: " + ensure_err;
            return false;
        }
        const int new_ts = g_ed.tileset_editor.doc.tile_size;
        if (g_ed.doc.tile_size != new_ts && (new_ts == 8 || new_ts == 16)) {
            const int w8 = g_ed.doc.width_8px();
            const int h8 = g_ed.doc.height_8px();
            g_ed.doc.reset_8px(w8, h8, new_ts);
        }
        sync_atlas_to_tileset(g_ed.tileset_editor.atlas, g_ed.doc.tileset);
        g_ed.doc.tileset.png_path = path;
        g_ed.doc.tileset.terrain_path = "";
        update_tileset_texture(renderer ? renderer : s_renderer);
        g_ed.doc.solve_all_autotiles();
        if (!g_ed.doc.tileset.variants.empty()) {
            g_ed.status_msg = "Imported tileset project (" + std::to_string(g_ed.doc.tileset.variants.size()) +
                              " variants): " + path;
        } else {
            g_ed.status_msg = "Imported tileset project: " + path;
        }
        g_ed.settings.last_tileset_path = path;
        persist_settings();
        return true;
    }

    if (g_ed.doc.tileset.load_from_file(path)) {
        const int old_ts = g_ed.doc.tile_size;
        const int new_ts = g_ed.doc.tileset.tile_size;
        if (old_ts != new_ts) {
            const int w8 = g_ed.doc.width_8px();
            const int h8 = g_ed.doc.height_8px();
            g_ed.doc.tile_size = new_ts;
            g_ed.doc.resize_8px(w8, h8);
        }
        g_ed.doc.solve_all_autotiles();
        update_tileset_texture(renderer ? renderer : s_renderer);
        if (!g_ed.doc.tileset.variants.empty()) {
            g_ed.status_msg = "Loaded tileset (" + std::to_string(g_ed.doc.tileset.variants.size()) +
                              " variants): " + path;
        } else {
            g_ed.status_msg = "Loaded tileset: " + path;
        }
        g_ed.settings.last_tileset_path = path;
        persist_settings();
        return true;
    } else {
        g_ed.status_msg = "Error loading tileset: " + g_ed.doc.tileset.error;
        return false;
    }
}

static bool load_map_from_path(const std::string& path, SDL_Renderer* renderer) {
    std::string err = load_map_json(g_ed.doc, path);
    if (err.empty()) {
        g_ed.current_map_path = path;
        g_ed.settings.last_map_path = path;
        persist_settings();
        g_ed.status_msg = "Opened map: " + path;
        if (!g_ed.doc.tileset.is_valid() && !g_ed.doc.tileset.png_path.empty()) {
            load_tileset_from_path(g_ed.doc.tileset.png_path, renderer);
        }
        update_tileset_texture(renderer ? renderer : s_renderer);
        return true;
    } else {
        g_ed.status_msg = "Error opening map: " + err;
        return false;
    }
}

static void open_tileset_dialog(SDL_Renderer* renderer) {
#ifdef __EMSCRIPTEN__
    (void)renderer;
    web_trigger_file_dialog(".png,.terrain,.tilesetproj", WebFileTarget_Tileset);
#else
    nfdu8filteritem_t filters[4] = {
        {"Tileset Files (*.png, *.terrain, *.tilesetproj)", "png,terrain,tilesetproj"},
        {"Tileset Projects (*.tilesetproj)", "tilesetproj"},
        {"PNG Images (*.png)", "png"},
        {"Terrain Files (*.terrain)", "terrain"}
    };
    nfdu8char_t* out_path = nullptr;
    nfdresult_t res = NFD_OpenDialogU8(&out_path, filters, 4, nullptr);
    if (res == NFD_OKAY && out_path) {
        load_tileset_from_path(out_path, renderer);
        NFD_FreePathU8(out_path);
    }
#endif
}

static void open_map_dialog(SDL_Renderer* renderer) {
#ifdef __EMSCRIPTEN__
    (void)renderer;
    web_trigger_file_dialog(".json", WebFileTarget_MapJson);
#else
    nfdu8filteritem_t filters[2] = {{"Map JSON", "json"}, {"All Files", "*"}};
    nfdu8char_t* out_path = nullptr;
    nfdresult_t res = NFD_OpenDialogU8(&out_path, filters, 2, nullptr);
    if (res == NFD_OKAY && out_path) {
        load_map_from_path(out_path, renderer);
        NFD_FreePathU8(out_path);
    }
#endif
}

#ifdef __EMSCRIPTEN__
static std::string dirname_of(const std::string& path) {
    return fs::path(path).parent_path().string();
}
static std::string basename_of(const std::string& path) {
    return fs::path(path).stem().string();
}
static std::string filename_of(const std::string& path) {
    return fs::path(path).filename().string();
}

static std::string s_pending_terrain_path;
static AppView s_pending_terrain_view = AppView::Tilemap;

void handle_web_file_upload(const std::string& path, int target_type) {
    std::string ext = "";
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    }

    if (target_type == WebFileTarget_PendingTerrainPng) {
        if (!s_pending_terrain_path.empty()) {
            std::string t_text;
            std::string expected_ts;
            if (read_text_file(s_pending_terrain_path, t_text)) {
                auto ts_pos = t_text.find("\"tileset\"");
                if (ts_pos != std::string::npos) {
                    auto col = t_text.find(':', ts_pos);
                    if (col != std::string::npos) {
                        auto q1 = t_text.find('"', col + 1);
                        if (q1 != std::string::npos) {
                            auto q2 = t_text.find('"', q1 + 1);
                            if (q2 != std::string::npos) {
                                expected_ts = t_text.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }
                    }
                }
            }
            std::string stem = basename_of(s_pending_terrain_path);
            if (stem.size() >= 8 && stem.substr(stem.size() - 8) == ".terrain") {
                stem = stem.substr(0, stem.size() - 8);
            }
            const std::string dir = dirname_of(s_pending_terrain_path);
            std::error_code ec;
            if (!expected_ts.empty()) {
                const std::string dst = (dir.empty() ? "" : (dir + "/")) + filename_of(expected_ts);
                if (dst != path) {
                    fs::copy_file(path, dst, fs::copy_options::overwrite_existing, ec);
                }
            }
            const std::string stem_dst = (dir.empty() ? "" : (dir + "/")) + stem + ".png";
            if (stem_dst != path) {
                fs::copy_file(path, stem_dst, fs::copy_options::overwrite_existing, ec);
            }

            const std::string terrain_to_load = s_pending_terrain_path;
            const AppView view_to_load = s_pending_terrain_view;
            s_pending_terrain_path.clear();

            if (view_to_load == AppView::TilesetMaker) {
                switch_to_view(AppView::TilesetMaker, s_renderer);
                g_ed.tileset_editor.import_12x4(terrain_to_load);
            } else {
                load_tileset_from_path(terrain_to_load, s_renderer);
            }
            return;
        }
    }

    if (target_type == WebFileTarget_MapJson || (target_type == WebFileTarget_Auto && ext == ".json")) {
        load_map_from_path(path, s_renderer);
    } else if (target_type == WebFileTarget_TilesetProj || (target_type == WebFileTarget_Auto && ext == ".tilesetproj" && g_ed.current_view == AppView::TilesetMaker)) {
        switch_to_view(AppView::TilesetMaker, s_renderer);
        tsm::ProjectData data;
        const std::string err = tsm::load_project(data, path);
        if (err.empty()) {
            restore_project_data_into_editor(data, path);
            g_ed.tileset_editor.configure_view();
        } else {
            g_ed.tileset_editor.status = "Error loading project: " + err;
        }
    } else if (target_type == WebFileTarget_Import5x3) {
        switch_to_view(AppView::TilesetMaker, s_renderer);
        g_ed.tileset_editor.import_5x3(path);
    } else if (target_type == WebFileTarget_Import12x4) {
        switch_to_view(AppView::TilesetMaker, s_renderer);
        if (ext == ".terrain") {
            std::string t_text;
            std::string ts_name;
            if (read_text_file(path, t_text)) {
                auto ts_pos = t_text.find("\"tileset\"");
                if (ts_pos != std::string::npos) {
                    auto col = t_text.find(':', ts_pos);
                    if (col != std::string::npos) {
                        auto q1 = t_text.find('"', col + 1);
                        if (q1 != std::string::npos) {
                            auto q2 = t_text.find('"', q1 + 1);
                            if (q2 != std::string::npos) {
                                ts_name = t_text.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }
                    }
                }
            }
            const std::string dir = dirname_of(path);
            std::string stem = basename_of(path);
            if (stem.size() >= 8 && stem.substr(stem.size() - 8) == ".terrain") {
                stem = stem.substr(0, stem.size() - 8);
            }
            std::vector<std::string> png_candidates;
            if (!ts_name.empty()) {
                if (!dir.empty()) png_candidates.push_back(dir + "/" + ts_name);
                png_candidates.push_back(ts_name);
            }
            if (!dir.empty()) png_candidates.push_back(dir + "/" + stem + ".png");
            png_candidates.push_back(stem + ".png");

            bool png_exists = false;
            for (const auto& cand : png_candidates) {
                if (file_exists(cand)) {
                    png_exists = true;
                    break;
                }
            }
            const bool current_has_tileset = (g_ed.tileset_editor.has_atlas && !g_ed.tileset_editor.atlas.tiles.empty());
            if (!png_exists && !current_has_tileset) {
                s_pending_terrain_path = path;
                s_pending_terrain_view = AppView::TilesetMaker;
                const std::string expected_img = ts_name.empty() ? (stem + ".png") : ts_name;
                const std::string prompt = "Uploaded '" + filename_of(path) + "'. Please select the matching tileset image ('" + expected_img + "')...";
                g_ed.status_msg = prompt;
                g_ed.tileset_editor.status = prompt;
                web_trigger_file_dialog(".png", WebFileTarget_PendingTerrainPng);
                return;
            }
            if (!png_exists && current_has_tileset) {
                tsm::TerrainLoad tload = tsm::load_terrain(path);
                if (tload.error.empty()) {
                    for (const auto& v : tload.variants) {
                        if (v.x >= g_ed.tileset_editor.atlas.cols) {
                            g_ed.tileset_editor.atlas.grow_cols(v.x + 1);
                        }
                    }
                    g_ed.tileset_editor.atlas.bindings = tload.variants;
                    g_ed.tileset_editor.step = tsm::Step::Variants;
                    g_ed.tileset_editor.configure_view();
                    g_ed.tileset_editor.status = "Attached terrain variants (" + std::to_string(tload.variants.size()) + ") to current tileset.";
                    return;
                }
            }
        }
        g_ed.tileset_editor.import_12x4(path);
    } else if (target_type == WebFileTarget_Palette || (target_type == WebFileTarget_Auto && ext == ".palette")) {
        switch_to_view(AppView::TilesetMaker, s_renderer);
        const auto loaded = tsm::load_palette_file(path.c_str());
        if (loaded.error.empty()) {
            g_ed.tileset_editor.push_undo();
            if (g_ed.tileset_editor.art_step()) g_ed.tileset_editor.doc.apply_palette(loaded.colors);
            else g_ed.tileset_editor.atlas.apply_palette(loaded.colors);
            g_ed.tileset_editor.bump_art();
            g_ed.tileset_editor.status = "Loaded palette.";
        } else {
            g_ed.tileset_editor.status = loaded.error;
        }
    } else if (target_type == WebFileTarget_Tileset || ext == ".terrain" || ext == ".png" || ext == ".tilesetproj") {
        if (ext == ".tilesetproj") {
            if (g_ed.current_view == AppView::TilesetMaker) {
                switch_to_view(AppView::TilesetMaker, s_renderer);
                tsm::ProjectData data;
                const std::string err = tsm::load_project(data, path);
                if (err.empty()) {
                    restore_project_data_into_editor(data, path);
                    g_ed.tileset_editor.configure_view();
                } else {
                    g_ed.tileset_editor.status = "Error loading project: " + err;
                }
            } else {
                load_tileset_from_path(path, s_renderer);
            }
            return;
        }
        if (ext == ".terrain") {
            std::string t_text;
            std::string ts_name;
            if (read_text_file(path, t_text)) {
                auto ts_pos = t_text.find("\"tileset\"");
                if (ts_pos != std::string::npos) {
                    auto col = t_text.find(':', ts_pos);
                    if (col != std::string::npos) {
                        auto q1 = t_text.find('"', col + 1);
                        if (q1 != std::string::npos) {
                            auto q2 = t_text.find('"', q1 + 1);
                            if (q2 != std::string::npos) {
                                ts_name = t_text.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }
                    }
                }
            }
            const std::string dir = dirname_of(path);
            std::string stem = basename_of(path);
            if (stem.size() >= 8 && stem.substr(stem.size() - 8) == ".terrain") {
                stem = stem.substr(0, stem.size() - 8);
            }
            std::vector<std::string> png_candidates;
            if (!ts_name.empty()) {
                if (!dir.empty()) png_candidates.push_back(dir + "/" + ts_name);
                png_candidates.push_back(ts_name);
            }
            if (!dir.empty()) png_candidates.push_back(dir + "/" + stem + ".png");
            png_candidates.push_back(stem + ".png");

            bool png_exists = false;
            for (const auto& cand : png_candidates) {
                if (file_exists(cand)) {
                    png_exists = true;
                    break;
                }
            }

            const bool current_has_tileset = (g_ed.current_view == AppView::TilesetMaker)
                ? (g_ed.tileset_editor.has_atlas && !g_ed.tileset_editor.atlas.tiles.empty())
                : g_ed.doc.tileset.is_valid();

            if (!png_exists && !current_has_tileset) {
                s_pending_terrain_path = path;
                s_pending_terrain_view = (g_ed.current_view == AppView::TilesetMaker) ? AppView::TilesetMaker : AppView::Tilemap;
                const std::string expected_img = ts_name.empty() ? (stem + ".png") : ts_name;
                const std::string prompt = "Uploaded '" + filename_of(path) + "'. Please select the matching tileset image ('" + expected_img + "')...";
                g_ed.status_msg = prompt;
                if (g_ed.current_view == AppView::TilesetMaker) {
                    g_ed.tileset_editor.status = prompt;
                }
                web_trigger_file_dialog(".png", WebFileTarget_PendingTerrainPng);
                return;
            }

            if (g_ed.current_view == AppView::TilesetMaker && !png_exists && current_has_tileset) {
                tsm::TerrainLoad tload = tsm::load_terrain(path);
                if (tload.error.empty()) {
                    for (const auto& v : tload.variants) {
                        if (v.x >= g_ed.tileset_editor.atlas.cols) {
                            g_ed.tileset_editor.atlas.grow_cols(v.x + 1);
                        }
                    }
                    g_ed.tileset_editor.atlas.bindings = tload.variants;
                    g_ed.tileset_editor.step = tsm::Step::Variants;
                    g_ed.tileset_editor.configure_view();
                    g_ed.tileset_editor.status = "Attached terrain variants (" + std::to_string(tload.variants.size()) + ") to current tileset.";
                    return;
                }
            }
        }

        if (g_ed.current_view == AppView::TilesetMaker) {
            g_ed.tileset_editor.import_12x4(path);
        } else {
            load_tileset_from_path(path, s_renderer);
        }
    } else {
        g_ed.status_msg = "Unknown file type: " + path;
    }
}

void wasm_open_file(const char* filename) {
    if (filename) handle_web_file_upload(filename, WebFileTarget_Auto);
}
void wasm_open_map(const char* filename) {
    if (filename) handle_web_file_upload(filename, WebFileTarget_MapJson);
}
void wasm_import_tileset(const char* filename) {
    if (filename) handle_web_file_upload(filename, WebFileTarget_Tileset);
}
void wasm_open_project(const char* filename) {
    if (filename) handle_web_file_upload(filename, WebFileTarget_TilesetProj);
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void wasm_c_open_file(const char* filename) {
    wasm_open_file(filename);
}
EMSCRIPTEN_KEEPALIVE void wasm_c_open_map(const char* filename) {
    wasm_open_map(filename);
}
EMSCRIPTEN_KEEPALIVE void wasm_c_import_tileset(const char* filename) {
    wasm_import_tileset(filename);
}
EMSCRIPTEN_KEEPALIVE void wasm_c_open_project(const char* filename) {
    wasm_open_project(filename);
}
}

static float s_wasm_pinch_scale = 1.0f;
static bool s_wasm_has_pinch = false;
static float s_wasm_pinch_center_x = -1.0f;
static float s_wasm_pinch_center_y = -1.0f;
static bool s_wasm_has_pinch_center = false;

void handle_web_pinch(float factor, float center_x, float center_y) {
    if (factor > 0.0f) {
        s_wasm_pinch_scale *= factor;
        s_wasm_has_pinch = true;
        if (center_x >= 0.0f && center_y >= 0.0f) {
            s_wasm_pinch_center_x = center_x;
            s_wasm_pinch_center_y = center_y;
            s_wasm_has_pinch_center = true;
        }
    }
}
#endif

static void save_map_dialog() {
    if (g_ed.selection_lifted) apply_moved_selection();
    if (g_ed.paste_mode) commit_paste();

#ifndef __EMSCRIPTEN__
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
#endif
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

static std::string path_filename(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

static std::string save_tileset_project_to_file(const std::string& path, const std::string& name) {
    if (!g_ed.doc.tileset.is_valid()) return "No valid tileset to export";

    // If the tileset editor has open project / atlas data, sync current doc variants and colors
    if (g_ed.tileset_editor.has_atlas && !g_ed.tileset_editor.atlas.tiles.empty()) {
        sync_tileset_to_atlas(g_ed.doc.tileset, g_ed.tileset_editor.atlas);
        tsm::ProjectData data;
        data.name = !name.empty() ? name : (g_ed.tileset_editor.project_name[0] ? g_ed.tileset_editor.project_name : "tileset");
        switch (g_ed.tileset_editor.step) {
            case tsm::Step::Edges: data.step = tsm::ProjectStep::Edges; break;
            case tsm::Step::Specialty: data.step = tsm::ProjectStep::Specialty; break;
            case tsm::Step::Variants: data.step = tsm::ProjectStep::Variants; break;
            case tsm::Step::Center:
            default: data.step = tsm::ProjectStep::Center; break;
        }
        data.seeded = g_ed.tileset_editor.seeded;
        data.stamped = g_ed.tileset_editor.stamped;
        data.specialty = g_ed.tileset_editor.specialty;
        data.atlas_cell = g_ed.tileset_editor.atlas_cell;
        data.preview_sel = g_ed.tileset_editor.preview_sel;
        data.tile_mode = g_ed.tileset_editor.tile_mode;
        data.export_header = g_ed.tileset_editor.export_header;
        data.export_terrain = g_ed.tileset_editor.export_terrain;
        data.export_5x3 = g_ed.tileset_editor.export_5x3;
        data.art_rev = g_ed.tileset_editor.art_rev;
        data.atlas_rev = g_ed.tileset_editor.atlas_rev;
        data.has_atlas = true;
        data.tileset = g_ed.tileset_editor.doc.snapshot();
        data.atlas = g_ed.tileset_editor.atlas.snapshot();
        return tsm::save_project(data, path);
    }

    // Otherwise, construct atlas and tileset doc snapshot from doc.tileset
    sync_tileset_to_atlas(g_ed.doc.tileset, g_ed.tileset_editor.atlas);
    tsm::convert_atlas_to_tileset(g_ed.tileset_editor.atlas, g_ed.tileset_editor.doc);
    g_ed.tileset_editor.step = tsm::Step::Variants;
    g_ed.tileset_editor.has_atlas = true;

    tsm::ProjectData data;
    data.name = !name.empty() ? name : (g_ed.tileset_editor.project_name[0] ? g_ed.tileset_editor.project_name : "tileset");
    data.step = tsm::ProjectStep::Variants;
    data.seeded = false;
    data.stamped = false;
    data.tile_mode = true;
    data.export_header = true;
    data.export_terrain = true;
    data.export_5x3 = true;
    data.has_atlas = true;
    data.tileset = g_ed.tileset_editor.doc.snapshot();
    data.atlas = g_ed.tileset_editor.atlas.snapshot();
    return tsm::save_project(data, path);
}

static void export_tileset_dialog() {
    if (!g_ed.doc.tileset.is_valid()) {
        g_ed.status_msg = "No tileset to export.";
        return;
    }

    const char* default_dir = (std::strlen(g_ed.export_folder) > 0)
        ? g_ed.export_folder
        : (!g_ed.settings.last_export_dir.empty() ? g_ed.settings.last_export_dir.c_str() : nullptr);

    std::string def_ts_name = "tileset.png";
    if (g_ed.tileset_editor.project_name[0] &&
        std::strcmp(g_ed.tileset_editor.project_name, "untitled") != 0) {
        def_ts_name = std::string(g_ed.tileset_editor.project_name) + ".png";
    } else if (!g_ed.doc.tileset.png_path.empty()) {
        std::string fn = path_filename(g_ed.doc.tileset.png_path);
        auto dot = fn.find_last_of('.');
        if (dot != std::string::npos) fn = fn.substr(0, dot) + ".png";
        if (!fn.empty()) def_ts_name = fn;
    }

    nfdu8filteritem_t filters[3] = {
        {"PNG Image (*.png)", "png"},
        {"Tileset Project (*.tilesetproj)", "tilesetproj"},
        {"All Files (*)", "*"}
    };
    nfdu8char_t* out_path = nullptr;

    nfdresult_t res = NFD_SaveDialogU8(&out_path, filters, 3, default_dir, def_ts_name.c_str());
    if (res != NFD_OKAY || !out_path) {
        if (res == NFD_ERROR) {
            g_ed.status_msg = "Export dialog error: " + std::string(NFD_GetError());
        }
        return;
    }

    std::string chosen = out_path;
    NFD_FreePathU8(out_path);

    std::string dir;
    std::string stem;
    const size_t last_slash = chosen.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        dir = chosen.substr(0, last_slash);
        const std::string filename = chosen.substr(last_slash + 1);
        auto dot = filename.find_last_of('.');
        stem = (dot != std::string::npos) ? filename.substr(0, dot) : filename;
    } else {
        dir = ".";
        auto dot = chosen.find_last_of('.');
        stem = (dot != std::string::npos) ? chosen.substr(0, dot) : chosen;
    }

    if (stem.empty()) stem = "tileset";

    std::vector<std::string> saved;
    // 1. Export Tileset PNG
    {
        std::string png_path = dir + "/" + stem + ".png";
        std::string err = export_tileset_png(g_ed.doc.tileset, png_path);
        if (!err.empty()) {
            g_ed.status_msg = "Tileset PNG export failed: " + err;
            return;
        }
        saved.push_back(stem + ".png");
    }
    // 2. Export Tileset Project (.tilesetproj)
    {
        std::string proj_path = dir + "/" + stem + ".tilesetproj";
        std::string err = save_tileset_project_to_file(proj_path, stem);
        if (!err.empty()) {
            g_ed.status_msg = "Tileset project export failed: " + err;
            return;
        }
        saved.push_back(stem + ".tilesetproj");
    }
    // 3. Export Terrain if variants exist
    if (!g_ed.doc.tileset.variants.empty()) {
        std::string terr_path = dir + "/" + stem + ".terrain";
        std::string err = export_tileset_terrain(g_ed.doc.tileset, terr_path, stem + ".png");
        if (err.empty()) {
            saved.push_back(stem + ".terrain");
        }
    }

    std::string msg = "Tileset exported: ";
    for (size_t i = 0; i < saved.size(); ++i) {
        if (i > 0) msg += ", ";
        msg += saved[i];
    }
    g_ed.status_msg = msg;
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

    // Companion tileset stem for exported tileset files
    std::string ts_stem;
    if (g_ed.tileset_editor.project_name[0] &&
        std::strcmp(g_ed.tileset_editor.project_name, "tileset") != 0 &&
        std::strcmp(g_ed.tileset_editor.project_name, "untitled") != 0) {
        ts_stem = g_ed.tileset_editor.project_name;
    } else if (!g_ed.doc.tileset.png_path.empty()) {
        std::string base = path_filename(g_ed.doc.tileset.png_path);
        auto dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        if (!base.empty() && base != "tileset" && base != "untitled") {
            ts_stem = base;
        }
    }
    if (ts_stem.empty() || ts_stem == stem) {
        ts_stem = stem + "_tileset";
    }

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
        const std::string ts_ref = (g_ed.export_tileset_png && g_ed.doc.tileset.is_valid()) ? (ts_stem + ".png") :
                                   ((g_ed.export_tileset_proj && g_ed.doc.tileset.is_valid()) ? (ts_stem + ".tilesetproj") : "");
        std::string err = save_map_json(g_ed.doc, p, ts_ref);
        if (!err.empty()) {
            g_ed.status_msg = "Map JSON export failed: " + err;
            return;
        }
        saved_files.push_back(stem + ".json");
    }

    if (g_ed.doc.tileset.is_valid()) {
        if (g_ed.export_tileset_png) {
            const std::string p = dir + "/" + ts_stem + ".png";
            std::string err = export_tileset_png(g_ed.doc.tileset, p);
            if (!err.empty()) {
                g_ed.status_msg = "Tileset PNG export failed: " + err;
                return;
            }
            saved_files.push_back(ts_stem + ".png");
        }
        if (g_ed.export_tileset_proj) {
            const std::string p = dir + "/" + ts_stem + ".tilesetproj";
            std::string err = save_tileset_project_to_file(p, ts_stem);
            if (!err.empty()) {
                g_ed.status_msg = "Tileset project export failed: " + err;
                return;
            }
            saved_files.push_back(ts_stem + ".tilesetproj");
        }
        if (g_ed.export_terrain) {
            const std::string p = dir + "/" + ts_stem + ".terrain";
            std::string err = export_tileset_terrain(g_ed.doc.tileset, p, ts_stem + ".png");
            if (!err.empty()) {
                g_ed.status_msg = "Terrain export failed: " + err;
                return;
            }
            saved_files.push_back(ts_stem + ".terrain");
        }
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
    const float sc = g_ed.settings.scale;

    if (is_map) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.88f, 1.0f));
    }
    if (ImGui::Button("Map Editor", ImVec2(100.0f * sc, 0))) {
        if (!is_map) switch_to_view(AppView::Tilemap, renderer);
    }
    if (is_map) ImGui::PopStyleColor();

    ImGui::SameLine(0, 4);

    if (is_ts) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.88f, 1.0f));
    }
    if (ImGui::Button("Tileset Maker", ImVec2(105.0f * sc, 0))) {
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
        ImGui::SameLine(ImGui::GetWindowWidth() - 210.0f * sc);
        if (ImGui::Button("Apply & Return to Map", ImVec2(190.0f * sc, 0))) {
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
            if (ImGui::Button(btn_lbl, ImVec2(24.0f * g_ed.settings.scale, 0))) {
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

    // 1. Pinch gesture zoom (e.g. macOS trackpad pinch-to-zoom, web trackpad/touch pinch)
    if (g_ed.has_pinch && g_ed.pinch_scale > 0.0f) {
        const float old_zoom = g_ed.zoom;
        g_ed.zoom = std::clamp(g_ed.zoom * g_ed.pinch_scale, 0.25f, 16.0f);
        float mx = (g_ed.has_pinch_center && g_ed.pinch_center.x >= 0.0f) ? (g_ed.pinch_center.x - canvas_p0.x) : (io.MousePos.x - canvas_p0.x);
        float my = (g_ed.has_pinch_center && g_ed.pinch_center.y >= 0.0f) ? (g_ed.pinch_center.y - canvas_p0.y) : (io.MousePos.y - canvas_p0.y);
        if (mx < 0.0f || mx > canvas_sz.x || my < 0.0f || my > canvas_sz.y) {
            mx = canvas_sz.x * 0.5f;
            my = canvas_sz.y * 0.5f;
        }
        g_ed.pan.x = mx - (mx - g_ed.pan.x) * (g_ed.zoom / old_zoom);
        g_ed.pan.y = my - (my - g_ed.pan.y) * (g_ed.zoom / old_zoom);
        g_ed.has_pinch = false;
        g_ed.pinch_scale = 1.0f;
        g_ed.has_pinch_center = false;
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

static void draw_sidebar_tileset_page(SDL_Renderer* renderer) {
    const ImVec4 sec_hdr_col = g_ed.settings.dark ? ImVec4(0.4f, 0.75f, 1.0f, 1.0f) : ImVec4(0.12f, 0.45f, 0.85f, 1.0f);

    ImGui::TextColored(sec_hdr_col, "TILESET");
    if (ImGui::Button("Import Tileset...", ImVec2(-1, 0))) {
        open_tileset_dialog(renderer);
    }
    if (!g_ed.doc.tileset.is_valid()) {
        if (ImGui::Button("Create in Tileset Maker...", ImVec2(-1, 0))) {
            switch_to_view(AppView::TilesetMaker, renderer);
        }
    } else {
        if (ImGui::Button("Edit in Tileset Maker...", ImVec2(-1, 0))) {
            switch_to_view(AppView::TilesetMaker, renderer);
        }
        if (ImGui::Button("Export Tileset...", ImVec2(-1, 0))) {
            export_tileset_dialog();
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
                if (ImGui::Button("Stamp", ImVec2(btn_w, 0))) {
                    g_ed.tileset_mode = TilesetSidebarMode::Stamp;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Select tiles to paint onto the tilemap");
            }
            ImGui::SameLine(0, 4);
            {
                ScopedStyleColor active_col(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.88f, 1.0f), g_ed.tileset_mode == TilesetSidebarMode::Collision);
                if (ImGui::Button("Collision", ImVec2(btn_w, 0))) {
                    g_ed.tileset_mode = TilesetSidebarMode::Collision;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Paint or clear collision types on tileset tiles");
            }
            ImGui::SameLine(0, 4);
            {
                ScopedStyleColor active_col(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.88f, 1.0f), g_ed.tileset_mode == TilesetSidebarMode::Variants);
                if (ImGui::Button("Variants", ImVec2(btn_w, 0))) {
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
            const float border_size = 1.0f;
            const float pad_x = 4.0f;
            const float pad_y = 4.0f;
            const float outer_w = ImGui::GetContentRegionAvail().x;
            const float avail_sidebar_h = ImGui::GetContentRegionAvail().y;

            const TilesetPreviewLayout layout = compute_tileset_preview_layout(outer_w, avail_sidebar_h, cols, rows, spacing, pad_x, pad_y, border_size);
            const float tile_ui_size = layout.tile_ui_size;
            const float total_w = layout.total_w;
            const float total_h = layout.total_h;

            const ImGuiWindowFlags child_flags = layout.needs_vscroll ? ImGuiWindowFlags_None : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::BeginChild("TilesetPaletteChild", ImVec2(0, layout.child_h), true, child_flags);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 mouse = ImGui::GetIO().MousePos;

            Cell current_hover = {-1, -1};
            Cell current_terrain_hover = {-1, -1};
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    const float x0 = origin.x + static_cast<float>(c) * (tile_ui_size + spacing);
                    const float y0 = origin.y + static_cast<float>(r) * (tile_ui_size + spacing);
                    const float x1 = x0 + tile_ui_size;
                    const float y1 = y0 + tile_ui_size;

                    const float u0 = static_cast<float>(c * g_ed.doc.tileset.tile_size) / static_cast<float>(g_ed.texture_w);
                    const float v0 = static_cast<float>(r * g_ed.doc.tileset.tile_size) / static_cast<float>(g_ed.texture_h);
                    const float u1 = static_cast<float>((c + 1) * g_ed.doc.tileset.tile_size) / static_cast<float>(g_ed.texture_w);
                    const float v1 = static_cast<float>((r + 1) * g_ed.doc.tileset.tile_size) / static_cast<float>(g_ed.texture_h);

                    dl->AddImage(reinterpret_cast<ImTextureID>(g_ed.tileset_texture),
                                 ImVec2(x0, y0), ImVec2(x1, y1),
                                 ImVec2(u0, v0), ImVec2(u1, v1));

                    if (c == 10 && r == 1) {
                        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(18, 20, 26, 215));
                        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(230, 60, 60, 200), 1.5f);
                        dl->AddLine(ImVec2(x1, y0), ImVec2(x0, y1), IM_COL32(230, 60, 60, 200), 1.5f);
                    }

                    if (mouse.x >= x0 && mouse.x < x1 && mouse.y >= y0 && mouse.y < y1 && ImGui::IsWindowHovered()) {
                        if (g_ed.tileset_mode == TilesetSidebarMode::Stamp) {
                            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 0, 255), 0.0f, 0, 2.0f);
                        } else if (g_ed.tileset_mode == TilesetSidebarMode::Collision) {
                            current_hover = {c, r};
                        } else if (g_ed.tileset_mode == TilesetSidebarMode::Variants) {
                            current_terrain_hover = {c, r};
                        }
                    }

                    if (g_ed.tileset_mode == TilesetSidebarMode::Collision) {
                        const uint8_t col_id = g_ed.doc.tileset.get_tile_collision(c, r);
                        if (col_id != 0) {
                            const CollisionType* ct = g_ed.doc.get_collision_type(col_id);
                            const Rgb rgb = ct ? ct->color : Rgb{235, 60, 50};
                            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(rgb.r, rgb.g, rgb.b, 90));
                            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(rgb.r, rgb.g, rgb.b, 160), 0.0f, 0, 1.0f);
                        }
                    } else if (g_ed.tileset_mode == TilesetSidebarMode::Variants) {
                        const Cell sel = g_ed.selected_terrain_tile;
                        const bool is_sel = (c == sel.x && r == sel.y);
                        const bool is_orig = (c < 12 && r < 4);
                        const bool is_extra = (c >= 12);
                        const bool is_bound_var = g_ed.doc.tileset.is_variant(c, r);
                        const VariantBinding* sel_vb = g_ed.doc.tileset.find_variant(sel.x, sel.y);

                        if (is_sel) {
                            dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(255, 230, 40, 255), 0.0f, 0, 2.5f);
                        }
                        if (is_orig) {
                            const int v_count = g_ed.doc.tileset.count_variants_for_root(c, r);
                            if (v_count > 0) {
                                dl->AddCircleFilled(ImVec2(x1 - 4, y0 + 4), 3.0f, IM_COL32(60, 230, 90, 240));
                            }
                            if (sel_vb && sel_vb->root_x == c && sel_vb->root_y == r) {
                                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 140, 0, 240), 0.0f, 0, 2.0f);
                            }
                        } else if (is_extra) {
                            if (is_bound_var) {
                                const VariantBinding* vb = g_ed.doc.tileset.find_variant(c, r);
                                if (vb && vb->root_x == sel.x && vb->root_y == sel.y) {
                                    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(50, 220, 100, 90));
                                    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(50, 220, 100, 255), 0.0f, 0, 2.0f);
                                } else {
                                    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(70, 130, 240, 70));
                                    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(70, 130, 240, 160), 0.0f, 0, 1.0f);
                                }
                            } else {
                                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(140, 140, 140, 90), 0.0f, 0, 1.0f);
                            }
                        }
                    }

                    if (g_ed.tileset_mode == TilesetSidebarMode::Stamp && c == g_ed.stamp_col && r == g_ed.stamp_row) {
                        dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(0, 255, 255, 255), 0.0f, 0, 2.0f);
                    }
                }
            }

            g_ed.hovered_col_tile = current_hover;
            g_ed.hovered_terrain_tile = current_terrain_hover;

            if (g_ed.tileset_mode == TilesetSidebarMode::Collision && current_hover.x >= 0) {
                const float x0 = origin.x + static_cast<float>(current_hover.x) * (tile_ui_size + spacing);
                const float y0 = origin.y + static_cast<float>(current_hover.y) * (tile_ui_size + spacing);
                const float x1 = x0 + tile_ui_size;
                const float y1 = y0 + tile_ui_size;
                const uint8_t hover_col = g_ed.doc.tileset.get_tile_collision(current_hover.x, current_hover.y);
                const CollisionType* ct = g_ed.doc.get_collision_type(hover_col);
                const Rgb rgb = ct ? ct->color : (hover_col == 0 ? Rgb{80, 80, 80} : Rgb{235, 60, 50});
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(rgb.r, rgb.g, rgb.b, 255), 0.0f, 0, 2.0f);
            }

            if (g_ed.tileset_mode == TilesetSidebarMode::Variants && current_terrain_hover.x >= 0) {
                const float x0 = origin.x + static_cast<float>(current_terrain_hover.x) * (tile_ui_size + spacing);
                const float y0 = origin.y + static_cast<float>(current_terrain_hover.y) * (tile_ui_size + spacing);
                const float x1 = x0 + tile_ui_size;
                const float y1 = y0 + tile_ui_size;
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.5f);
            }

            ImGui::InvisibleButton("palette_hitbox", ImVec2(total_w, total_h));
            if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
                const ImVec2 click = ImGui::GetIO().MousePos;
                const int hit_c = static_cast<int>((click.x - origin.x) / (tile_ui_size + spacing));
                const int hit_r = static_cast<int>((click.y - origin.y) / (tile_ui_size + spacing));
                if (hit_c >= 0 && hit_c < cols && hit_r >= 0 && hit_r < rows) {
                    if (g_ed.tileset_mode == TilesetSidebarMode::Stamp) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            g_ed.stamp_col = hit_c;
                            g_ed.stamp_row = hit_r;
                            g_ed.paint_mode = TileMode::Stamp;
                            g_ed.status_msg = "Selected Stamp Tile (" + std::to_string(hit_c) + ", " + std::to_string(hit_r) + ").";
                        }
                    } else if (g_ed.tileset_mode == TilesetSidebarMode::Collision) {
                        const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                        const bool right = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                        if (left || right) {
                            if (hit_c != 10 || hit_r != 1) {
                                const Cell cur_tile = {hit_c, hit_r};
                                if (cur_tile != g_ed.last_col_painted) {
                                    const uint8_t new_t = left ? g_ed.active_collision_type : 0;
                                    g_ed.doc.tileset.set_tile_collision(hit_c, hit_r, new_t);
                                    g_ed.doc.mark_dirty();
                                    g_ed.last_col_painted = cur_tile;
                                    const CollisionType* ct = g_ed.doc.get_collision_type(new_t);
                                    const std::string name = ct ? ct->name : (new_t == 0 ? "None" : "Solid");
                                    g_ed.status_msg = "Set collision on (" + std::to_string(hit_c) + ", " + std::to_string(hit_r) + ") to " + name;
                                }
                            }
                        }
                    } else if (g_ed.tileset_mode == TilesetSidebarMode::Variants) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            const int hover_c = hit_c;
                            const int hover_r = hit_r;
                            const bool shift = ImGui::GetIO().KeyShift;
                            const Cell sel = g_ed.selected_terrain_tile;

                            if (shift) {
                                if (Tileset::is_base_origin_tile(sel.x, sel.y) && Tileset::is_variant_tile(hover_c, hover_r)) {
                                    const VariantBinding* existing = g_ed.doc.tileset.find_variant(hover_c, hover_r);
                                    if (existing && existing->root_x == sel.x && existing->root_y == sel.y) {
                                        g_ed.doc.tileset.remove_variant(hover_c, hover_r);
                                        g_ed.doc.mark_dirty();
                                        g_ed.doc.solve_all_autotiles();
                                        g_ed.status_msg = "Disconnected variant (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ").";
                                    } else {
                                        g_ed.doc.tileset.set_variant(hover_c, hover_r, sel.x, sel.y, g_ed.terrain_variant_prob);
                                        g_ed.doc.mark_dirty();
                                        g_ed.doc.solve_all_autotiles();
                                        g_ed.status_msg = "Assigned extra tile (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) +
                                                          ") as variant of Origin (" + std::to_string(sel.x) + ", " + std::to_string(sel.y) + ").";
                                    }
                                    g_ed.selected_terrain_tile = {hover_c, hover_r};
                                } else if (Tileset::is_variant_tile(sel.x, sel.y) && Tileset::is_base_origin_tile(hover_c, hover_r)) {
                                    const VariantBinding* existing = g_ed.doc.tileset.find_variant(sel.x, sel.y);
                                    if (existing && existing->root_x == hover_c && existing->root_y == hover_r) {
                                        g_ed.doc.tileset.remove_variant(sel.x, sel.y);
                                        g_ed.doc.mark_dirty();
                                        g_ed.doc.solve_all_autotiles();
                                        g_ed.status_msg = "Disconnected variant (" + std::to_string(sel.x) + ", " + std::to_string(sel.y) + ").";
                                    } else {
                                        g_ed.doc.tileset.set_variant(sel.x, sel.y, hover_c, hover_r, g_ed.terrain_variant_prob);
                                        g_ed.doc.mark_dirty();
                                        g_ed.doc.solve_all_autotiles();
                                        g_ed.status_msg = "Assigned extra tile (" + std::to_string(sel.x) + ", " + std::to_string(sel.y) +
                                                          ") as variant of Origin (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ").";
                                    }
                                    g_ed.selected_terrain_tile = {hover_c, hover_r};
                                }
                            } else {
                                g_ed.selected_terrain_tile = {hover_c, hover_r};
                                if (Tileset::is_base_origin_tile(hover_c, hover_r)) {
                                    const int n_vars = g_ed.doc.tileset.count_variants_for_root(hover_c, hover_r);
                                    g_ed.status_msg = "Selected Origin tile (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ") with " + std::to_string(n_vars) + " variant(s).";
                                } else {
                                    const VariantBinding* vb = g_ed.doc.tileset.find_variant(hover_c, hover_r);
                                    if (vb) {
                                        g_ed.status_msg = "Selected Variant (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ") bound to Origin (" + std::to_string(vb->root_x) + ", " + std::to_string(vb->root_y) + ").";
                                    } else {
                                        g_ed.status_msg = "Selected unassigned extra tile (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ").";
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                g_ed.last_col_painted = {-1, -1};
            }
            ImGui::EndChild();
        }

        if (g_ed.tileset_mode == TilesetSidebarMode::Stamp) {
            ImGui::Spacing();
            ImGui::Text("Selected Stamp: (%d, %d)", g_ed.stamp_col, g_ed.stamp_row);
            if (g_ed.stamp_col == 10 && g_ed.stamp_row == 1) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Tile (10, 1) is the Empty / Eraser Tile.");
            }
        } else if (g_ed.tileset_mode == TilesetSidebarMode::Collision) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(sec_hdr_col, "COLLISION TYPES");

            // Add Type Button
            if (ImGui::Button("+ Add Type", ImVec2(-1, 0))) {
                const uint8_t new_id = g_ed.doc.add_collision_type();
                g_ed.active_collision_type = new_id;
                g_ed.doc.mark_dirty();
            }

            ImGui::Spacing();
            const float avail_w = ImGui::GetContentRegionAvail().x;
            const float color_bar_w = 18.0f * g_ed.settings.scale;
            const float del_btn_w = 26.0f * g_ed.settings.scale;

            // Collision Types List
            for (size_t i = 0; i < g_ed.doc.collision_types.size(); ++i) {
                const auto ct = g_ed.doc.collision_types[i];
                const bool is_active = (g_ed.active_collision_type == ct.id);

                ImGui::PushID(static_cast<int>(ct.id));

                // 1. Color Preview / Picker
                float col_f[3] = { ct.color.r / 255.0f, ct.color.g / 255.0f, ct.color.b / 255.0f };
                if (ImGui::ColorEdit3("##color", col_f, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                    g_ed.doc.set_collision_type_color(ct.id, Rgb{
                        static_cast<uint8_t>(col_f[0] * 255.0f),
                        static_cast<uint8_t>(col_f[1] * 255.0f),
                        static_cast<uint8_t>(col_f[2] * 255.0f)
                    });
                }
                ImGui::SameLine(0, 4);

                // 2. Select / Activate Button
                char type_label[64];
                std::snprintf(type_label, sizeof(type_label), "%d: %s", ct.id, ct.name.c_str());
                const float sel_btn_w = avail_w - color_bar_w - (ct.id > 1 ? (del_btn_w + 8.0f) : 4.0f);

                {
                    ScopedStyleColor active_col(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.88f, 1.0f), is_active);
                    if (ImGui::Button(type_label, ImVec2(sel_btn_w, 0))) {
                        g_ed.active_collision_type = ct.id;
                    }
                }

                // 3. Delete Button (types > 1 only)
                if (ct.id > 1) {
                    ImGui::SameLine(0, 4);
                    ScopedStyleColor del_col(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
                    if (ImGui::Button("X", ImVec2(del_btn_w, 0))) {
                        const uint8_t dead_id = ct.id;
                        g_ed.doc.remove_collision_type(dead_id);
                        if (g_ed.active_collision_type == dead_id) {
                            g_ed.active_collision_type = 1;
                        }
                        ImGui::PopID();
                        break;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Delete collision type (reverts painted tiles of this type to 0)");
                    }
                }

                ImGui::PopID();
            }

            // Edit Active Type Name
            CollisionType* active_ct = g_ed.doc.get_collision_type(g_ed.active_collision_type);
            if (active_ct) {
                ImGui::Spacing();
                char edit_name[64];
                std::snprintf(edit_name, sizeof(edit_name), "%s", active_ct->name.c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##EditName", edit_name, sizeof(edit_name))) {
                    active_ct->name = edit_name;
                    g_ed.doc.mark_dirty();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Rename active collision type");
                }
            }

            // Global Collision Actions: Clear All
            ImGui::Spacing();
            const float half_btn_w = (avail_w - 4.0f) * 0.5f;
            if (ImGui::Button("Fill Solid (1)", ImVec2(half_btn_w, 0))) {
                for (int r = 0; r < g_ed.doc.tileset.rows; ++r) {
                    for (int c = 0; c < g_ed.doc.tileset.cols; ++c) {
                        if (c != 10 || r != 1) {
                            g_ed.doc.tileset.set_tile_collision(c, r, 1);
                        }
                    }
                }
                g_ed.doc.mark_dirty();
                g_ed.status_msg = "Filled solid (Type 1) on all tiles";
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Set all tiles to Solid (1)");
            }
            ImGui::SameLine(0, 4);
            if (ImGui::Button("Clear All Tiles", ImVec2(half_btn_w, 0))) {
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

            if (ImGui::Button("Save .terrain File", ImVec2(-1, 0))) {
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

                    if (ImGui::Button("Auto-bind Extra Cols (12+)##Origin", ImVec2(-1, 0))) {
                        g_ed.doc.tileset.auto_bind_extra_columns(sx, sy, g_ed.terrain_variant_prob);
                        g_ed.doc.mark_dirty();
                        g_ed.doc.solve_all_autotiles();
                        g_ed.status_msg = "Auto-bound extra columns to Origin (" + std::to_string(sx) + ", " + std::to_string(sy) + ").";
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Automatically bind matching row columns 12+ as variants of this origin tile");
                    }

                    if (root_vars > 0) {
                        if (ImGui::Button("Disconnect All Variants##Origin", ImVec2(-1, 0))) {
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

                        if (ImGui::Button("Disconnect Variant##Var", ImVec2(-1, 0))) {
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
            ImGui::BeginChild("VariantListChild##Sidebar", ImVec2(0, 140.0f * g_ed.settings.scale), ImGuiChildFlags_Borders);
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
        ImGui::Spacing();
        ImGui::TextDisabled("No tileset loaded.");
        ImGui::TextWrapped("Click 'Import Tileset...' or 'Create in Tileset Maker...' above to load or create tile art.");
    }
}

static void draw_sidebar_map_page() {
    const ImVec4 sec_hdr_col = g_ed.settings.dark ? ImVec4(0.4f, 0.75f, 1.0f, 1.0f) : ImVec4(0.12f, 0.45f, 0.85f, 1.0f);

    ImGui::TextColored(sec_hdr_col, "MAP PROPERTIES");
    char name_buf[128];
    std::snprintf(name_buf, sizeof(name_buf), "%s", g_ed.doc.name.c_str());
    if (ImGui::InputText("Level Name", name_buf, sizeof(name_buf))) {
        g_ed.doc.name = name_buf;
        g_ed.doc.mark_dirty();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(sec_hdr_col, "DIMENSIONS");
    ImGui::Text("Tile Size:    %d x %d px", g_ed.doc.tile_size, g_ed.doc.tile_size);
    ImGui::Text("Map Grid:     %d x %d cells", g_ed.doc.width, g_ed.doc.height);
    ImGui::Text("8x8 Units:    %d x %d tiles", g_ed.doc.width_8px(), g_ed.doc.height_8px());
    ImGui::Text("Resolution:   %d x %d px", g_ed.doc.pixel_width(), g_ed.doc.pixel_height());
    ImGui::Text("Edge Buffer:  %d cell outside border", g_ed.doc.buffer);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(sec_hdr_col, "STATISTICS");
    int placed_tiles = 0;
    for (int y = 0; y < g_ed.doc.height; ++y) {
        for (int x = 0; x < g_ed.doc.width; ++x) {
            if (!g_ed.doc.get_cell(x, y).is_empty()) ++placed_tiles;
        }
    }
    const int total_cells = g_ed.doc.width * g_ed.doc.height;
    const float fill_pct = (total_cells > 0) ? (static_cast<float>(placed_tiles) / static_cast<float>(total_cells) * 100.0f) : 0.0f;
    ImGui::Text("Placed Tiles: %d / %d (%.1f%%)", placed_tiles, total_cells, fill_pct);

    const int types_used = g_ed.doc.build_collision_grid().count_types_used();
    ImGui::Text("Collisions:   %d type(s) active on map", types_used);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(sec_hdr_col, "ACTIONS");
    if (ImGui::Button("Resize Canvas...", ImVec2(-1, 0))) {
        g_ed.resize_w = g_ed.doc.width_8px();
        g_ed.resize_h = g_ed.doc.height_8px();
        g_ed.show_resize_modal = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Change canvas width and height in 8x8 tile units");
    }

    if (ImGui::Button("Clear Map", ImVec2(-1, 0))) {
        g_ed.doc.clear_cells();
        g_ed.status_msg = "Cleared map.";
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear all placed tiles (supports Undo/Redo)");
    }
}

static void draw_sidebar_export_page() {
    const ImVec4 sec_hdr_col = g_ed.settings.dark ? ImVec4(0.4f, 0.75f, 1.0f, 1.0f) : ImVec4(0.12f, 0.45f, 0.85f, 1.0f);

    ImGui::TextColored(sec_hdr_col, "EXPORT SETTINGS");
#ifdef __EMSCRIPTEN__
    ImGui::TextDisabled("Exports are automatically downloaded by your browser.");
#else
    if (ImGui::Button("Export Destination...", ImVec2(-1, 0))) {
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
#endif

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(sec_hdr_col, "MAP FORMATS");
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
    ImGui::TextDisabled("Composite Map PNG is always exported.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(sec_hdr_col, "TILESET FORMATS");
    if (!g_ed.doc.tileset.is_valid()) {
        ImGui::TextDisabled("No tileset loaded.");
    } else {
        if (ImGui::Checkbox("Tileset Image (.png)", &g_ed.export_tileset_png)) {
            persist_settings();
        }
        if (ImGui::Checkbox("Tileset Project (.tilesetproj)", &g_ed.export_tileset_proj)) {
            persist_settings();
        }
        if (ImGui::Checkbox("Tileset Terrain (.terrain)", &g_ed.export_terrain)) {
            persist_settings();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    {
        ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.2f, 0.58f, 0.35f, 1.0f));
        if (ImGui::Button("EXPORT NOW", ImVec2(-1, 38.0f * g_ed.settings.scale))) {
            execute_export();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Export all checked formats to destination folder");
    }
}

static void draw_sidebar_content(SDL_Renderer* renderer) {
    // 3 Page Switcher Buttons at top of sidebar: Tileset | Map Properties | Export
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float gap = 4.0f;
    const float btn_w = std::floor((avail_w - gap * 2.0f) / 3.0f);
    const float btn_h = 28.0f * g_ed.settings.scale;

    {
        ScopedStyleColor active_col(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.88f, 1.0f), g_ed.sidebar_page == SidebarPage::Tileset);
        if (ImGui::Button("Tileset", ImVec2(btn_w, btn_h))) {
            g_ed.sidebar_page = SidebarPage::Tileset;
            g_ed.settings.sidebar_page = 0;
            persist_settings();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Tileset, stamp selection, collisions, and terrain variants");
    }
    ImGui::SameLine(0, gap);
    {
        ScopedStyleColor active_col(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.88f, 1.0f), g_ed.sidebar_page == SidebarPage::MapProperties);
        if (ImGui::Button("Map Properties", ImVec2(btn_w, btn_h))) {
            g_ed.sidebar_page = SidebarPage::MapProperties;
            g_ed.settings.sidebar_page = 1;
            persist_settings();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Level name, canvas dimensions, resize, and clear");
    }
    ImGui::SameLine(0, gap);
    {
        ScopedStyleColor active_col(ImGuiCol_Button, ImVec4(0.20f, 0.52f, 0.88f, 1.0f), g_ed.sidebar_page == SidebarPage::Export);
        if (ImGui::Button("Export", ImVec2(std::max(btn_w, avail_w - btn_w * 2.0f - gap * 2.0f), btn_h))) {
            g_ed.sidebar_page = SidebarPage::Export;
            g_ed.settings.sidebar_page = 2;
            persist_settings();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Export destination, file format toggles, and output generation");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_ed.sidebar_page == SidebarPage::Tileset) {
        draw_sidebar_tileset_page(renderer);
    } else if (g_ed.sidebar_page == SidebarPage::MapProperties) {
        draw_sidebar_map_page();
    } else if (g_ed.sidebar_page == SidebarPage::Export) {
        draw_sidebar_export_page();
    }
}

static void draw_modals(SDL_Renderer* renderer) {
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
                if (ImGui::Button(bid, ImVec2(36.0f * g_ed.settings.scale, 0))) {
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
        if (ImGui::Button("Apply", ImVec2(100.0f * g_ed.settings.scale, 0))) {
            g_ed.doc.resize_8px(g_ed.resize_w, g_ed.resize_h, g_ed.resize_anchor_x, g_ed.resize_anchor_y);
            g_ed.show_resize_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f * g_ed.settings.scale, 0))) {
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
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Checkbox("Start project with empty tileset/terrain", &g_ed.new_empty_tileset);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Clear current tileset and terrain variants, starting fresh in Tileset Maker");
        }

        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(100.0f * g_ed.settings.scale, 0))) {
            cancel_or_deselect();
            cancel_paste();
            g_ed.doc.reset_8px(g_ed.new_w, g_ed.new_h, g_ed.new_tile_size);
            g_ed.doc.name = g_ed.new_name;
            g_ed.current_map_path.clear();

            if (g_ed.new_empty_tileset) {
                g_ed.doc.clear_tileset();
                update_tileset_texture(renderer);
                g_ed.tileset_editor.reset_new("tileset", g_ed.new_tile_size);
                g_ed.selected_terrain_tile = {Tileset::kDefaultCenterCol, Tileset::kDefaultCenterRow};
                g_ed.hovered_terrain_tile = {-1, -1};
                g_ed.stamp_col = 9;
                g_ed.stamp_row = 2;
                g_ed.tileset_mode = TilesetSidebarMode::Stamp;
                g_ed.settings.last_tileset_path.clear();
                persist_settings();
                g_ed.status_msg = "Created new map with empty tileset/terrain.";
            } else {
                g_ed.status_msg = "Created new map.";
            }

            g_ed.new_empty_tileset = false;
            g_ed.show_new_modal = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f * g_ed.settings.scale, 0))) {
            g_ed.new_empty_tileset = false;
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
    const bool tmm_loaded = load_settings_file(g_ed.settings, settings_path());
    if (!tmm_loaded || std::abs(g_ed.settings.scale - 1.0f) < 0.001f) {
        tsm::Settings tsm_s;
        if (tsm::load_settings_file(tsm_s, tsm::settings_path())) {
            if (tsm_s.scale >= 0.75f && tsm_s.scale <= 2.0f && std::abs(tsm_s.scale - 1.0f) > 0.001f) {
                g_ed.settings.scale = tsm_s.scale;
            }
        }
    }

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
    g_ed.export_terrain = g_ed.settings.export_terrain;
    g_ed.export_tileset_png = g_ed.settings.export_tileset_png;
    g_ed.export_tileset_proj = g_ed.settings.export_tileset_proj;
    if (g_ed.settings.sidebar_page >= 0 && g_ed.settings.sidebar_page <= 2) {
        g_ed.sidebar_page = static_cast<SidebarPage>(g_ed.settings.sidebar_page);
    }

#ifdef __EMSCRIPTEN__
    double init_css_w = 0.0, init_css_h = 0.0;
    emscripten_get_element_css_size("#canvas", &init_css_w, &init_css_h);
    if (init_css_w < 10.0 || init_css_h < 10.0) {
        init_css_w = MAIN_THREAD_EM_ASM_DOUBLE({ return window.innerWidth; });
        init_css_h = MAIN_THREAD_EM_ASM_DOUBLE({ return window.innerHeight; });
    }
    const int win_w = (init_css_w >= 100.0) ? static_cast<int>(init_css_w) : 1280;
    const int win_h = (init_css_h >= 100.0) ? static_cast<int>(init_css_h) : 800;
#else
    const int win_w = (g_ed.settings.window_w >= 640) ? g_ed.settings.window_w : 1280;
    const int win_h = (g_ed.settings.window_h >= 480) ? g_ed.settings.window_h : 800;
#endif

    SDL_Window* window = SDL_CreateWindow("Tilemap Maker", win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    s_window = window;

#ifndef __EMSCRIPTEN__
    if (g_ed.settings.window_placed &&
        window_rect_visible(g_ed.settings.window_x, g_ed.settings.window_y, win_w, win_h)) {
        SDL_SetWindowPosition(window, g_ed.settings.window_x, g_ed.settings.window_y);
    }
    if (g_ed.settings.window_maximized) {
        SDL_MaximizeWindow(window);
    }
#endif

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

#ifdef __EMSCRIPTEN__
    {
        const float dpr = emscripten_get_device_pixel_ratio();
        const int buf_w = std::max(1, static_cast<int>(std::round(win_w * dpr)));
        const int buf_h = std::max(1, static_cast<int>(std::round(win_h * dpr)));
        emscripten_set_canvas_element_size("#canvas", buf_w, buf_h);
        SDL_SetWindowSize(window, win_w, win_h);
    }
#endif

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

    s_window = window;
    s_renderer = renderer;
    s_running = true;

    struct EditorRunner {
        static void frame() {
            if (!s_running) return;
            SDL_Window* window = s_window;
            SDL_Renderer* renderer = s_renderer;
            bool& running = s_running;
            ImGuiIO& io = ImGui::GetIO();

            g_ed.has_pinch = false;
            g_ed.pinch_scale = 1.0f;
            g_ed.pending_mouse_moves.clear();

#ifdef __EMSCRIPTEN__
            static int s_last_css_w = 0;
            static int s_last_css_h = 0;
            static float s_last_dpr = 0.0f;

            double css_w = 0.0, css_h = 0.0;
            emscripten_get_element_css_size("#canvas", &css_w, &css_h);
            if (css_w < 10.0 || css_h < 10.0) {
                css_w = MAIN_THREAD_EM_ASM_DOUBLE({ return window.innerWidth; });
                css_h = MAIN_THREAD_EM_ASM_DOUBLE({ return window.innerHeight; });
            }
            const float dpr = emscripten_get_device_pixel_ratio();
            const int target_w = static_cast<int>(css_w);
            const int target_h = static_cast<int>(css_h);

            if (target_w >= 100 && target_h >= 100 &&
                (target_w != s_last_css_w || target_h != s_last_css_h || std::abs(dpr - s_last_dpr) > 0.001f)) {
                s_last_css_w = target_w;
                s_last_css_h = target_h;
                s_last_dpr = dpr;

                const int buf_w = std::max(1, static_cast<int>(std::round(target_w * dpr)));
                const int buf_h = std::max(1, static_cast<int>(std::round(target_h * dpr)));
                emscripten_set_canvas_element_size("#canvas", buf_w, buf_h);
                SDL_SetWindowSize(window, target_w, target_h);
            }
#endif

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

#ifdef __EMSCRIPTEN__
            if (s_wasm_has_pinch) {
                g_ed.pinch_scale *= s_wasm_pinch_scale;
                g_ed.has_pinch = true;
                if (s_wasm_has_pinch_center) {
                    g_ed.has_pinch_center = true;
                    g_ed.pinch_center = ImVec2(s_wasm_pinch_center_x, s_wasm_pinch_center_y);
                }
                s_wasm_pinch_scale = 1.0f;
                s_wasm_has_pinch = false;
                s_wasm_has_pinch_center = false;
            }
#endif

            const SDL_WindowFlags win_flags = SDL_GetWindowFlags(window);
            if (win_flags & SDL_WINDOW_MINIMIZED) {
#ifndef __EMSCRIPTEN__
                SDL_Delay(20);
#endif
                return;
            }
#ifndef __EMSCRIPTEN__
            if (!(win_flags & (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS))) {
                SDL_Delay(32);
            }
#endif

        if (std::abs(g_ed.settings.scale - s_applied_scale) > 0.0001f || (g_ed.settings.dark ? 1 : 0) != s_applied_dark) {
            apply_app_theme(g_ed.settings.dark);
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
                    g_ed.new_empty_tileset = false;
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
#ifdef __EMSCRIPTEN__
                    web_trigger_file_dialog(".tilesetproj", WebFileTarget_TilesetProj);
#else
                    g_ed.tileset_editor.try_open_project_dialog();
#endif
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
                        g_ed.new_empty_tileset = false;
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
                    if (ImGui::MenuItem("Export Tileset...", nullptr, false, g_ed.doc.tileset.is_valid())) {
                        export_tileset_dialog();
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
                    ImGui::TextUnformatted("UI scale");
                    float percent = g_ed.settings.scale * 100.0f;
                    ImGui::SetNextItemWidth(220.0f * g_ed.settings.scale);
                    if (ImGui::SliderFloat("##view_scale_tmm", &percent, 75.0f, 200.0f, "%.0f%%")) {
                        set_ui_scale(percent / 100.0f);
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
                    ImGui::Separator();
                    if (ImGui::BeginMenu("Sidebar Page")) {
                        if (ImGui::MenuItem("Tileset", nullptr, g_ed.sidebar_page == SidebarPage::Tileset)) {
                            g_ed.sidebar_page = SidebarPage::Tileset;
                            g_ed.settings.sidebar_page = 0;
                            persist_settings();
                        }
                        if (ImGui::MenuItem("Map Properties", nullptr, g_ed.sidebar_page == SidebarPage::MapProperties)) {
                            g_ed.sidebar_page = SidebarPage::MapProperties;
                            g_ed.settings.sidebar_page = 1;
                            persist_settings();
                        }
                        if (ImGui::MenuItem("Export", nullptr, g_ed.sidebar_page == SidebarPage::Export)) {
                            g_ed.sidebar_page = SidebarPage::Export;
                            g_ed.settings.sidebar_page = 2;
                            persist_settings();
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Settings")) {
                    ImGui::TextUnformatted("Theme");
                    if (ImGui::MenuItem("Dark", nullptr, g_ed.settings.dark)) {
                        g_ed.settings.dark = true;
                        apply_app_theme(true);
                        persist_settings();
                    }
                    if (ImGui::MenuItem("Light", nullptr, !g_ed.settings.dark)) {
                        g_ed.settings.dark = false;
                        apply_app_theme(false);
                        persist_settings();
                    }
                    ImGui::Separator();
                    ImGui::TextUnformatted("UI scale");
                    float percent = g_ed.settings.scale * 100.0f;
                    ImGui::SetNextItemWidth(220.0f * g_ed.settings.scale);
                    if (ImGui::SliderFloat("##settings_scale_tmm", &percent, 75.0f, 200.0f, "%.0f%%")) {
                        set_ui_scale(percent / 100.0f);
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
#ifdef __EMSCRIPTEN__
                        web_trigger_file_dialog(".tilesetproj", WebFileTarget_TilesetProj);
#else
                        g_ed.tileset_editor.try_open_project_dialog();
#endif
                    }
                    if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
                        g_ed.tileset_editor.save_project();
                    }
                    if (ImGui::MenuItem("Save Project As...", "Shift+Ctrl+S")) {
                        g_ed.tileset_editor.save_project(true);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Import 12x4 Tileset...")) {
#ifdef __EMSCRIPTEN__
                        web_trigger_file_dialog(".png,.terrain", WebFileTarget_Import12x4);
#else
                        g_ed.tileset_editor.try_import_12x4_dialog();
#endif
                    }
                    if (ImGui::MenuItem("Import 5x3 Tileset...")) {
#ifdef __EMSCRIPTEN__
                        web_trigger_file_dialog(".png", WebFileTarget_Import5x3);
#else
                        g_ed.tileset_editor.try_import_5x3_dialog();
#endif
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
                    ImGui::Separator();
                    ImGui::TextUnformatted("UI scale");
                    float percent = g_ed.settings.scale * 100.0f;
                    ImGui::SetNextItemWidth(220.0f * g_ed.settings.scale);
                    if (ImGui::SliderFloat("##view_scale_tsm", &percent, 75.0f, 200.0f, "%.0f%%")) {
                        set_ui_scale(percent / 100.0f);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Settings")) {
                    ImGui::TextUnformatted("Theme");
                    if (ImGui::MenuItem("Dark", nullptr, g_ed.settings.dark)) {
                        g_ed.settings.dark = true;
                        apply_app_theme(true);
                        persist_settings();
                    }
                    if (ImGui::MenuItem("Light", nullptr, !g_ed.settings.dark)) {
                        g_ed.settings.dark = false;
                        apply_app_theme(false);
                        persist_settings();
                    }
                    ImGui::Separator();
                    ImGui::TextUnformatted("UI scale");
                    float percent = g_ed.settings.scale * 100.0f;
                    ImGui::SetNextItemWidth(220.0f * g_ed.settings.scale);
                    if (ImGui::SliderFloat("##settings_scale_tsm", &percent, 75.0f, 200.0f, "%.0f%%")) {
                        set_ui_scale(percent / 100.0f);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Pixel Grid", nullptr, g_ed.tileset_editor.settings.pixel_grid)) {
                        g_ed.tileset_editor.settings.pixel_grid = !g_ed.tileset_editor.settings.pixel_grid;
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
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

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
            const float splitter_w = 6.0f * g_ed.settings.scale;
            const float status_bar_h = std::max(24.0f * g_ed.settings.scale, ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 2.0f);
            const float avail_w = ImGui::GetContentRegionAvail().x;
            const float avail_h = std::max(100.0f, ImGui::GetContentRegionAvail().y - status_bar_h);

            const float min_sidebar = 300.0f * g_ed.settings.scale;
            const float min_canvas = 200.0f * g_ed.settings.scale;
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
            const float status_right_offset = 360.0f * g_ed.settings.scale;
            if (ImGui::GetWindowWidth() > status_right_offset + 100.0f) {
                ImGui::SameLine(ImGui::GetWindowWidth() - status_right_offset);
            }
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
            const float status_bar_h = std::max(24.0f * g_ed.settings.scale, ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 2.0f);
            const float avail_h = std::max(100.0f, ImGui::GetContentRegionAvail().y - status_bar_h);
            g_ed.tileset_editor.draw_content(renderer, window, avail_h);

            // Check if return was requested from inside TilesetEditor
            if (g_ed.tileset_editor.request_return_to_map) {
                g_ed.tileset_editor.request_return_to_map = false;
                switch_to_view(AppView::Tilemap, renderer);
            }

            // Status bar at bottom
            ImGui::Separator();
            ImGui::Text("%s", g_ed.tileset_editor.status.c_str());
            const float status_right_offset = 360.0f * g_ed.settings.scale;
            if (ImGui::GetWindowWidth() > status_right_offset + 100.0f) {
                ImGui::SameLine(ImGui::GetWindowWidth() - status_right_offset);
            }
            ImGui::Text("Tileset: %s (%dpx)", g_ed.tileset_editor.project_name, g_ed.tileset_editor.doc.tile_size);
        }

        ImGui::End(); // MainLayout##Window

        if (g_ed.current_view == AppView::Tilemap) {
            draw_modals(renderer);
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

#ifdef __EMSCRIPTEN__
        web_flush_downloads();
        if (!s_running) {
            emscripten_cancel_main_loop();
        }
#endif
    }
};

#ifdef __EMSCRIPTEN__
    web_init_file_io();
    emscripten_set_main_loop(&EditorRunner::frame, 0, 1);
#else
    while (s_running) {
        EditorRunner::frame();
    }
#endif

    persist_settings();
    s_window = nullptr;
    s_renderer = nullptr;

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
