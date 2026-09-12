#include "editor.h"
#include "settings.h"

#include "core/collision.h"
#include "core/io.h"
#include "core/tilemap_doc.h"
#include "core/tileset.h"
#include "core/types.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "nfd.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tmm {

enum class Tool { Paint, Line, Erase, Rect, Fill, Select, Eyedropper };
enum class EditorViewMode { Tilemap, TilesetCollision };

struct EditorState {
    TilemapDoc doc;
    Settings settings;

    EditorViewMode view_mode = EditorViewMode::Tilemap;
    uint8_t active_collision_type = 1;
    float col_view_zoom = 3.0f;
    ImVec2 col_view_pan = ImVec2(40.0f, 40.0f);
    bool col_view_panning = false;
    Cell hovered_col_tile = {-1, -1};
    Cell last_col_painted = {-1, -1};

    Tool tool = Tool::Paint;
    TileMode paint_mode = TileMode::Terrain;
    int stamp_col = 9;
    int stamp_row = 2;
    int brush_size = 1;
    bool rect_fill = true;

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
    int new_w = 30;
    int new_h = 20;
    int new_tile_size = 16;

    bool show_resize_modal = false;
    int resize_w = 30;
    int resize_h = 20;
    int resize_anchor_x = -1; // -1: left, 0: center, 1: right
    int resize_anchor_y = -1; // -1: top, 0: center, 1: bottom

    bool show_export_modal = false;
    char export_folder[512] = "";
    bool export_png = true;
    bool export_col_json = true;
    bool export_col_bin = true;
    bool export_map_json = true;

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

static void apply_dark_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(8, 6);
    style.FramePadding = ImVec2(6, 4);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.52f, 0.56f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.14f, 0.15f, 0.18f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.19f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.25f, 0.27f, 0.31f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.29f, 0.31f, 0.36f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.10f, 0.11f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.28f, 0.30f, 0.34f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.36f, 0.38f, 0.44f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.44f, 0.47f, 0.53f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.38f, 0.72f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.35f, 0.65f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.45f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.28f, 0.31f, 0.37f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.22f, 0.45f, 0.70f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.28f, 0.32f, 0.39f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.33f, 0.38f, 0.46f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.22f, 0.24f, 0.27f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.26f, 0.29f, 0.35f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.20f, 0.23f, 0.28f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
}

static void apply_light_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(8, 6);
    style.FramePadding = ImVec2(6, 4);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.52f, 0.56f, 0.62f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.95f, 0.96f, 0.97f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.98f, 0.98f, 0.99f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.78f, 0.81f, 0.86f, 1.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.85f, 0.87f, 0.91f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.79f, 0.82f, 0.88f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.73f, 0.77f, 0.84f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.90f, 0.91f, 0.93f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.86f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.89f, 0.91f, 0.93f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.74f, 0.76f, 0.80f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.60f, 0.63f, 0.68f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.48f, 0.51f, 0.56f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.18f, 0.50f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.25f, 0.55f, 0.92f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.15f, 0.45f, 0.85f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.84f, 0.86f, 0.90f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.77f, 0.81f, 0.87f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.22f, 0.52f, 0.88f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.83f, 0.86f, 0.91f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.76f, 0.80f, 0.88f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.68f, 0.74f, 0.84f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.78f, 0.80f, 0.85f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.86f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.93f, 0.95f, 0.98f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.97f, 0.98f, 1.00f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.86f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
}

static void apply_theme(bool dark) {
    if (dark) {
        apply_dark_theme();
    } else {
        apply_light_theme();
    }
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
    g_ed.settings.brush_size = g_ed.brush_size;
    g_ed.settings.paint_mode = (g_ed.paint_mode == TileMode::Terrain ? 0 : 1);
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
    nfdu8filteritem_t filters[4] = {
        {"Tileset Files (*.png, *.h, *.terrain, *.json)", "png,h,terrain,json"},
        {"PNG Images (*.png)", "png"},
        {"C Headers (*.h)", "h"},
        {"Terrain Metadata (*.terrain, *.json)", "terrain,json"}
    };
    nfdu8char_t* out_path = nullptr;
    nfdresult_t res = NFD_OpenDialogU8(&out_path, filters, 4, nullptr);
    if (res == NFD_OKAY && out_path) {
        if (g_ed.doc.tileset.load_from_file(out_path)) {
            g_ed.doc.tile_size = g_ed.doc.tileset.tile_size;
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

static void open_terrain_dialog(SDL_Renderer* renderer) {
    nfdu8filteritem_t filters[3] = {
        {"Terrain / Variants (*.h, *.terrain, *.json)", "h,terrain,json"},
        {"C Headers (*.h)", "h"},
        {"Terrain Metadata (*.terrain, *.json)", "terrain,json"}
    };
    nfdu8char_t* out_path = nullptr;
    nfdresult_t res = NFD_OpenDialogU8(&out_path, filters, 3, nullptr);
    if (res == NFD_OKAY && out_path) {
        bool ok = false;
        if (g_ed.doc.tileset.is_valid()) {
            ok = g_ed.doc.tileset.import_variants_file(out_path);
        } else {
            ok = g_ed.doc.tileset.load_from_file(out_path);
            if (ok) {
                g_ed.doc.tile_size = g_ed.doc.tileset.tile_size;
                update_tileset_texture(renderer);
            }
        }
        if (ok) {
            g_ed.doc.solve_all_autotiles();
            g_ed.status_msg = "Imported terrain (" + std::to_string(g_ed.doc.tileset.variants.size()) +
                              " variants): " + std::string(out_path);
            persist_settings();
        } else {
            g_ed.status_msg = "Error importing terrain/variants: " + g_ed.doc.tileset.error;
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

    if (g_ed.export_png) {
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

    std::string msg = "Export complete: ";
    for (size_t i = 0; i < saved_files.size(); ++i) {
        if (i > 0) msg += ", ";
        msg += saved_files[i];
    }
    g_ed.status_msg = msg;
    g_ed.show_export_modal = false;
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
    g_ed.doc.set_clip_rect(nullptr);
    g_ed.is_moving_paste = false;
    g_ed.status_msg = "Paste cancelled.";
}

static void commit_paste() {
    if (!g_ed.paste_mode || g_ed.clipboard.is_empty()) return;
    g_ed.doc.paste_clipboard(g_ed.paste_pos.x, g_ed.paste_pos.y, g_ed.clipboard);
    g_ed.paste_mode = false;
    g_ed.has_selection = true;
    g_ed.selection = {g_ed.paste_pos.x, g_ed.paste_pos.y, g_ed.clipboard.w, g_ed.clipboard.h};
    g_ed.doc.set_clip_rect(&g_ed.selection);
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
        g_ed.doc.set_clip_rect(nullptr);
        g_ed.status_msg = "Move cancelled.";
    } else if (g_ed.has_selection) {
        g_ed.has_selection = false;
        g_ed.doc.set_clip_rect(nullptr);
        g_ed.status_msg = "Deselected.";
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
    g_ed.selection_lifted = false;
    g_ed.doc.set_clip_rect(&g_ed.selection);
    g_ed.is_moving_paste = false;
    g_ed.status_msg = "Pasting: Drag or use Arrows to move. Enter or click outside to commit, Esc to cancel.";
}

struct ScopedStyleColor {
    int count = 0;
    ScopedStyleColor(ImGuiCol idx, const ImVec4& col, bool condition = true) {
        if (condition) {
            ImGui::PushStyleColor(idx, col);
            count = 1;
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

static void draw_top_nav_and_view_row() {
    // Mode switcher buttons
    {
        ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.80f, 1.0f), g_ed.view_mode == EditorViewMode::Tilemap);
        if (ImGui::Button("Tilemap Editor")) {
            g_ed.view_mode = EditorViewMode::Tilemap;
        }
    }
    ImGui::SameLine();
    {
        ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.80f, 1.0f), g_ed.view_mode == EditorViewMode::TilesetCollision);
        if (ImGui::Button("Tileset Collision")) {
            g_ed.view_mode = EditorViewMode::TilesetCollision;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (g_ed.view_mode == EditorViewMode::Tilemap) {
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
        // Collision Mode View Controls
        if (ImGui::Button("-##ColZoomOut")) {
            g_ed.col_view_zoom = std::max(0.5f, g_ed.col_view_zoom / 1.25f);
        }
        ImGui::SameLine();
        ImGui::Text("%.0f%%", g_ed.col_view_zoom * 100.0f);
        ImGui::SameLine();
        if (ImGui::Button("+##ColZoomIn")) {
            g_ed.col_view_zoom = std::min(16.0f, g_ed.col_view_zoom * 1.25f);
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit View##ColFit")) {
            g_ed.col_view_zoom = 3.0f;
            g_ed.col_view_pan = ImVec2(40, 40);
        }
    }
}

static void draw_tool_selection_row() {
    if (g_ed.view_mode == EditorViewMode::TilesetCollision) {
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "COLLISION TYPES:");
        ImGui::SameLine();

        {
            ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.45f, 0.48f, 0.52f, 1.0f), g_ed.active_collision_type == 0);
            if (ImGui::Button("None (Clear)")) {
                g_ed.active_collision_type = 0;
            }
        }
        ImGui::SameLine();

        for (const auto& ct : g_ed.doc.collision_types) {
            const bool is_active = (g_ed.active_collision_type == ct.id);
            const ImVec4 btn_col(ct.color.r / 255.0f, ct.color.g / 255.0f, ct.color.b / 255.0f, is_active ? 1.0f : 0.65f);

            ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(btn_col.x, btn_col.y, btn_col.z, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, btn_col);
            if (is_active) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            }

            const std::string btn_label = ct.name + "##ColType" + std::to_string(ct.id);
            if (ImGui::Button(btn_label.c_str())) {
                g_ed.active_collision_type = ct.id;
            }

            if (is_active) {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
        }

        const bool can_add = (g_ed.doc.collision_types.size() < 13);
        if (!can_add) ImGui::BeginDisabled(true);
        if (ImGui::Button("+ Add Type")) {
            const uint8_t new_id = g_ed.doc.add_collision_type();
            if (new_id > 0) {
                g_ed.active_collision_type = new_id;
                const CollisionType* added_ct = g_ed.doc.get_collision_type(new_id);
                g_ed.status_msg = "Added new collision type: " + (added_ct ? added_ct->name : ("Type " + std::to_string(new_id)));
            }
        }
        if (!can_add) ImGui::EndDisabled();
        ImGui::SameLine();

        const bool can_remove = (g_ed.doc.collision_types.size() > 1 && g_ed.active_collision_type > 0);
        if (!can_remove) ImGui::BeginDisabled(true);
        if (ImGui::Button("- Remove")) {
            const uint8_t to_remove = g_ed.active_collision_type;
            g_ed.doc.remove_collision_type(to_remove);
            g_ed.active_collision_type = g_ed.doc.collision_types.front().id;
            g_ed.status_msg = "Removed collision type " + std::to_string(to_remove);
        }
        if (!can_remove) ImGui::EndDisabled();
        return;
    }

    ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "TOOLS:");
    ImGui::SameLine();

    auto tool_button = [](const char* label, Tool t, const char* shortcut, const char* tooltip) {
        const bool is_active = (g_ed.tool == t);
        const ImVec4 active_col(0.20f, 0.50f, 0.88f, 1.0f);
        const ImVec4 hover_col(0.28f, 0.58f, 0.95f, 1.0f);
        if (is_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, active_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover_col);
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
    tool_button("Line", Tool::Line, "2", "Straight line drawing tool");
    tool_button("Eraser", Tool::Erase, "3", "Eraser tool");
    tool_button("Rectangle", Tool::Rect, "4", "Rectangle / Box tool");
    tool_button("Fill", Tool::Fill, "5", "Flood fill contiguous tiles");
    tool_button("Select", Tool::Select, "6", "Rectangular selection & move tool");
    tool_button("Eyedropper", Tool::Eyedropper, "7", "Pick tile from map into stamp");
}

static void draw_tool_options_row() {
    if (g_ed.view_mode == EditorViewMode::TilesetCollision) {
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "COLLISION OPTIONS:");
        ImGui::SameLine();

        if (g_ed.active_collision_type > 0) {
            const CollisionType* ct = g_ed.doc.get_collision_type(g_ed.active_collision_type);
            if (ct) {
                ImGui::Text("Active: %s (Type %d)", ct->name.c_str(), ct->id);
                ImGui::SameLine();
                float c_flt[3] = {ct->color.r / 255.0f, ct->color.g / 255.0f, ct->color.b / 255.0f};
                ImGui::SetNextItemWidth(36);
                if (ImGui::ColorEdit3("##ActiveColColor", c_flt, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                    g_ed.doc.set_collision_type_color(g_ed.active_collision_type,
                        Rgb{static_cast<uint8_t>(c_flt[0] * 255.0f),
                            static_cast<uint8_t>(c_flt[1] * 255.0f),
                            static_cast<uint8_t>(c_flt[2] * 255.0f)});
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Change color for %s", ct->name.c_str());
                }
                ImGui::SameLine();
            }
        } else {
            ImGui::TextDisabled("Active: None (Erase collision)");
            ImGui::SameLine();
        }

        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::Button("Set All Type 1##ColSetAll")) {
            g_ed.doc.tileset.init_tile_collisions(1);
            g_ed.doc.mark_dirty();
            g_ed.status_msg = "Set all tiles to Type 1.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear All##ColClearAll")) {
            g_ed.doc.tileset.init_tile_collisions(0);
            g_ed.doc.mark_dirty();
            g_ed.status_msg = "Cleared collision on all tiles.";
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|  Left-click: Assign Collision  |  Right-click: Clear (None)  |  Middle-drag/Space: Pan  |  Scroll: Zoom");
        return;
    }

    // Helper for rendering brush thickness slider and quick preset buttons
    auto draw_thickness_controls = [](const char* label_prefix, const char* id_suffix) {
        ImGui::Text("Thickness:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        char slider_id[64];
        std::snprintf(slider_id, sizeof(slider_id), "##%sThickness%s", label_prefix, id_suffix);
        if (ImGui::SliderInt(slider_id, &g_ed.brush_size, 1, 4, "%d tiles")) {
            persist_settings();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Brush thickness: 1-4 tiles");
        }
        ImGui::SameLine();
        for (int s = 1; s <= 4; ++s) {
            ScopedStyleColor bcol(ImGuiCol_Button, ImVec4(0.24f, 0.48f, 0.80f, 1.0f), g_ed.brush_size == s);
            char btn_lbl[32];
            std::snprintf(btn_lbl, sizeof(btn_lbl), "%d##%sSz%d%s", s, label_prefix, s, id_suffix);
            if (ImGui::Button(btn_lbl, ImVec2(22, 0))) {
                g_ed.brush_size = s;
                persist_settings();
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
                g_ed.doc.begin_stroke("Clear Map");
                g_ed.doc.erase_rect({0, 0, g_ed.doc.width, g_ed.doc.height});
                g_ed.doc.end_stroke();
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
            ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "RECTANGLE OPTIONS:");
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
            draw_hint("Left-drag: Draw Rect  |  Right-drag: Erase Rect");
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
            if (g_ed.paste_mode) {
                ImGui::Text("Pasting: %d×%d at (%d, %d)", g_ed.clipboard.w, g_ed.clipboard.h, g_ed.paste_pos.x, g_ed.paste_pos.y);
            } else if (g_ed.has_selection) {
                ImGui::Text("Selected: %d×%d at (%d, %d)", g_ed.selection.w, g_ed.selection.h, g_ed.selection.x, g_ed.selection.y);
            } else {
                ImGui::TextDisabled("No active selection");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            const bool can_cut_copy = g_ed.has_selection && !g_ed.paste_mode;
            if (!can_cut_copy) ImGui::BeginDisabled(true);
            if (ImGui::Button("Cut##SelCut")) {
                if (g_ed.selection_lifted) {
                    g_ed.clipboard = g_ed.floating_clip;
                    g_ed.floating_clip.clear();
                    g_ed.doc.end_stroke();
                    g_ed.selection_lifted = false;
                    g_ed.has_selection = false;
                    g_ed.doc.set_clip_rect(nullptr);
                } else {
                    g_ed.doc.cut_rect(g_ed.selection, g_ed.clipboard);
                    g_ed.has_selection = false;
                    g_ed.doc.set_clip_rect(nullptr);
                }
                g_ed.status_msg = "Cut selection.";
            }
            ImGui::SameLine();
            if (ImGui::Button("Copy##SelCopy")) {
                if (g_ed.selection_lifted) {
                    g_ed.clipboard = g_ed.floating_clip;
                } else {
                    g_ed.clipboard = g_ed.doc.copy_rect(g_ed.selection);
                }
                g_ed.status_msg = "Copied selection.";
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
                if (g_ed.selection_lifted) {
                    g_ed.doc.end_stroke();
                    g_ed.selection_lifted = false;
                    g_ed.floating_clip.clear();
                    g_ed.has_selection = false;
                    g_ed.doc.set_clip_rect(nullptr);
                    g_ed.status_msg = "Deleted selected tiles.";
                } else {
                    g_ed.doc.erase_rect(g_ed.selection);
                    g_ed.status_msg = "Cleared selection.";
                }
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

            draw_hint("Drag to select or move selection");
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

static void draw_tileset_collision_viewport() {
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 30.0f || canvas_sz.y < 30.0f) return;
    const ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

    draw_list->PushClipRect(canvas_p0, canvas_p1, true);

    const ImU32 bg_col = g_ed.settings.dark ? IM_COL32(18, 20, 24, 255) : IM_COL32(220, 224, 230, 255);
    draw_list->AddRectFilled(canvas_p0, canvas_p1, bg_col);

    if (!g_ed.doc.tileset.is_valid() || !g_ed.tileset_texture) {
        const char* msg = "No valid tileset loaded. Import a tileset to configure tile collisions.";
        const ImVec2 txt_sz = ImGui::CalcTextSize(msg);
        draw_list->AddText(ImVec2(canvas_p0.x + (canvas_sz.x - txt_sz.x) * 0.5f,
                                  canvas_p0.y + (canvas_sz.y - txt_sz.y) * 0.5f),
                           IM_COL32(160, 160, 160, 255), msg);
        draw_list->PopClipRect();
        return;
    }

    ImGui::InvisibleButton("TilesetCollisionCanvas", canvas_sz,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool is_hovered = ImGui::IsItemHovered();

    // Mouse wheel zoom
    if (is_hovered && io.MouseWheel != 0.0f) {
        const float old_zoom = g_ed.col_view_zoom;
        const float factor = (io.MouseWheel > 0.0f) ? 1.25f : (1.0f / 1.25f);
        const float new_zoom = std::clamp(old_zoom * factor, 0.5f, 16.0f);

        const float mouse_rel_x = io.MousePos.x - (canvas_p0.x + g_ed.col_view_pan.x);
        const float mouse_rel_y = io.MousePos.y - (canvas_p0.y + g_ed.col_view_pan.y);
        g_ed.col_view_pan.x += mouse_rel_x * (1.0f - new_zoom / old_zoom);
        g_ed.col_view_pan.y += mouse_rel_y * (1.0f - new_zoom / old_zoom);
        g_ed.col_view_zoom = new_zoom;
    }

    // Panning with Middle Click or Space + Left Click
    const bool space_down = ImGui::IsKeyDown(ImGuiKey_Space);
    if (is_hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (space_down && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
        g_ed.col_view_panning = true;
    }
    if (g_ed.col_view_panning) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (space_down && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
            g_ed.col_view_pan.x += io.MouseDelta.x;
            g_ed.col_view_pan.y += io.MouseDelta.y;
        } else {
            g_ed.col_view_panning = false;
        }
    }

    const int cols = g_ed.doc.tileset.cols;
    const int rows = g_ed.doc.tileset.rows;
    const int ts = g_ed.doc.tileset.tile_size;
    const float tile_px = static_cast<float>(ts) * g_ed.col_view_zoom;
    const float origin_x = std::floor(canvas_p0.x + g_ed.col_view_pan.x);
    const float origin_y = std::floor(canvas_p0.y + g_ed.col_view_pan.y);

    const float atlas_w = cols * tile_px;
    const float atlas_h = rows * tile_px;

    // Draw checkerboard behind tileset
    const float chk_sz = 16.0f;
    const ImU32 chk1 = g_ed.settings.dark ? IM_COL32(28, 30, 36, 255) : IM_COL32(235, 238, 242, 255);
    const ImU32 chk2 = g_ed.settings.dark ? IM_COL32(36, 38, 46, 255) : IM_COL32(245, 248, 252, 255);
    for (float y = 0; y < atlas_h; y += chk_sz) {
        for (float x = 0; x < atlas_w; x += chk_sz) {
            const int ix = static_cast<int>(x / chk_sz);
            const int iy = static_cast<int>(y / chk_sz);
            const ImU32 col = ((ix + iy) % 2 == 0) ? chk1 : chk2;
            const float rx1 = std::min(origin_x + x + chk_sz, origin_x + atlas_w);
            const float ry1 = std::min(origin_y + y + chk_sz, origin_y + atlas_h);
            draw_list->AddRectFilled(ImVec2(origin_x + x, origin_y + y), ImVec2(rx1, ry1), col);
        }
    }

    // Render tileset texture
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (platform_io.DrawCallback_SetSamplerNearest != nullptr) {
        draw_list->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);
    }
    draw_list->AddImage(reinterpret_cast<ImTextureID>(g_ed.tileset_texture),
                        ImVec2(origin_x, origin_y),
                        ImVec2(origin_x + atlas_w, origin_y + atlas_h));
    if (platform_io.DrawCallback_SetSamplerLinear != nullptr) {
        draw_list->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
    }

    // Atlas outline
    draw_list->AddRect(ImVec2(origin_x, origin_y), ImVec2(origin_x + atlas_w, origin_y + atlas_h),
                       IM_COL32(70, 130, 240, 255), 0.0f, 0, 2.0f);

    // Tile grid lines
    const ImU32 grid_col = g_ed.settings.dark ? IM_COL32(255, 255, 255, 40) : IM_COL32(0, 0, 0, 40);
    for (int c = 1; c < cols; ++c) {
        const float gx = origin_x + c * tile_px;
        draw_list->AddLine(ImVec2(gx, origin_y), ImVec2(gx, origin_y + atlas_h), grid_col);
    }
    for (int r = 1; r < rows; ++r) {
        const float gy = origin_y + r * tile_px;
        draw_list->AddLine(ImVec2(origin_x, gy), ImVec2(origin_x + atlas_w, gy), grid_col);
    }

    // Draw collision overlay for each tile
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const uint8_t type_id = g_ed.doc.tileset.get_tile_collision(c, r);
            const float tx0 = origin_x + c * tile_px;
            const float ty0 = origin_y + r * tile_px;
            const float tx1 = tx0 + tile_px;
            const float ty1 = ty0 + tile_px;

            if (type_id != 0) {
                const CollisionType* ct = g_ed.doc.get_collision_type(type_id);
                const Rgb col_rgb = ct ? ct->color : Rgb{235, 60, 50};
                draw_list->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1),
                                         IM_COL32(col_rgb.r, col_rgb.g, col_rgb.b, 100));
                draw_list->AddRect(ImVec2(tx0, ty0), ImVec2(tx1, ty1),
                                   IM_COL32(col_rgb.r, col_rgb.g, col_rgb.b, 220), 0.0f, 0, 1.5f);

                if (tile_px >= 22.0f) {
                    char badge[16];
                    std::snprintf(badge, sizeof(badge), "T%d", static_cast<int>(type_id));
                    const ImVec2 bsz = ImGui::CalcTextSize(badge);
                    draw_list->AddRectFilled(ImVec2(tx0 + 2, ty0 + 2),
                                             ImVec2(tx0 + 4 + bsz.x, ty0 + 3 + bsz.y),
                                             IM_COL32(0, 0, 0, 190), 2.0f);
                    draw_list->AddText(ImVec2(tx0 + 3, ty0 + 2), IM_COL32(255, 255, 255, 255), badge);
                }
            }
        }
    }

    // Mouse hovering and collision painting
    g_ed.hovered_col_tile = {-1, -1};
    if (is_hovered && !g_ed.col_view_panning) {
        const float m_rel_x = (io.MousePos.x - origin_x) / tile_px;
        const float m_rel_y = (io.MousePos.y - origin_y) / tile_px;
        if (m_rel_x >= 0.0f && m_rel_y >= 0.0f && m_rel_x < cols && m_rel_y < rows) {
            g_ed.hovered_col_tile = {static_cast<int>(std::floor(m_rel_x)), static_cast<int>(std::floor(m_rel_y))};
        }
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

    if (g_ed.hovered_col_tile.x >= 0 && g_ed.hovered_col_tile.y >= 0) {
        const int hc = g_ed.hovered_col_tile.x;
        const int hr = g_ed.hovered_col_tile.y;
        const float hx0 = origin_x + hc * tile_px;
        const float hy0 = origin_y + hr * tile_px;
        const float hx1 = hx0 + tile_px;
        const float hy1 = hy0 + tile_px;

        draw_list->AddRect(ImVec2(hx0, hy0), ImVec2(hx1, hy1), IM_COL32(255, 255, 255, 240), 0.0f, 0, 2.0f);

        const uint8_t cur_type = g_ed.doc.tileset.get_tile_collision(hc, hr);
        const CollisionType* ct = g_ed.doc.get_collision_type(cur_type);
        const std::string cur_name = (cur_type == 0) ? "None" : (ct ? ct->name : "Type " + std::to_string(cur_type));
        const CollisionType* act = g_ed.doc.get_collision_type(g_ed.active_collision_type);
        const std::string act_name = (g_ed.active_collision_type == 0) ? "None" : (act ? act->name : "Type " + std::to_string(g_ed.active_collision_type));

        ImGui::SetTooltip("Tile (%d, %d)\nCollision: %s\nLeft-click: set %s\nRight-click: clear (None)",
                          hc, hr, cur_name.c_str(), act_name.c_str());

        if (io.MouseDown[ImGuiMouseButton_Left] && !space_down && !io.KeyCtrl) {
            const int prev_x = (g_ed.last_col_painted.x >= 0) ? g_ed.last_col_painted.x : hc;
            const int prev_y = (g_ed.last_col_painted.y >= 0) ? g_ed.last_col_painted.y : hr;
            apply_col_line(prev_x, prev_y, hc, hr, g_ed.active_collision_type);
            g_ed.last_col_painted = {hc, hr};
        } else if (io.MouseDown[ImGuiMouseButton_Right] && !space_down) {
            const int prev_x = (g_ed.last_col_painted.x >= 0) ? g_ed.last_col_painted.x : hc;
            const int prev_y = (g_ed.last_col_painted.y >= 0) ? g_ed.last_col_painted.y : hr;
            apply_col_line(prev_x, prev_y, hc, hr, 0);
            g_ed.last_col_painted = {hc, hr};
        } else {
            g_ed.last_col_painted = {-1, -1};
        }
    } else {
        if (!io.MouseDown[ImGuiMouseButton_Left] && !io.MouseDown[ImGuiMouseButton_Right]) {
            g_ed.last_col_painted = {-1, -1};
        }
    }

    draw_list->PopClipRect();
}

static void draw_canvas_viewport_content() {
    if (g_ed.view_mode == EditorViewMode::TilesetCollision) {
        draw_tileset_collision_viewport();
        return;
    }

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

    // Draw map background
    const ImU32 map_bg_col = g_ed.settings.dark ? IM_COL32(28, 30, 35, 255) : IM_COL32(245, 246, 250, 255);
    draw_list->AddRectFilled(ImVec2(map_x0, map_y0), ImVec2(map_x1, map_y1), map_bg_col);

    // Render placed tiles with nearest-neighbor point sampling
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (platform_io.DrawCallback_SetSamplerNearest != nullptr) {
        draw_list->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);
    }

    const bool has_texture = (g_ed.tileset_texture != nullptr && g_ed.texture_w > 0 && g_ed.texture_h > 0);
    const float inv_tex_w = has_texture ? (1.0f / static_cast<float>(g_ed.texture_w)) : 1.0f;
    const float inv_tex_h = has_texture ? (1.0f / static_cast<float>(g_ed.texture_h)) : 1.0f;
    const int ts = g_ed.doc.tile_size;

    for (int cy = 0; cy < g_ed.doc.height; ++cy) {
        for (int cx = 0; cx < g_ed.doc.width; ++cx) {
            const MapCell& cell = g_ed.doc.get_cell(cx, cy);
            if (cell.is_empty()) continue;

            const float x0 = cell_to_screen_x(cx);
            const float y0 = cell_to_screen_y(cy);
            const float x1 = cell_to_screen_x(cx + 1);
            const float y1 = cell_to_screen_y(cy + 1);

            if (x1 < canvas_p0.x || y1 < canvas_p0.y || x0 > canvas_p1.x || y0 > canvas_p1.y) {
                continue; // Frustum cull
            }

            if (has_texture && cell.atlas_x >= 0 && cell.atlas_y >= 0) {
                const float u0 = static_cast<float>(cell.atlas_x * ts) * inv_tex_w;
                const float v0 = static_cast<float>(cell.atlas_y * ts) * inv_tex_h;
                const float u1 = static_cast<float>((cell.atlas_x + 1) * ts) * inv_tex_w;
                const float v1 = static_cast<float>((cell.atlas_y + 1) * ts) * inv_tex_h;
                draw_list->AddImage(reinterpret_cast<ImTextureID>(g_ed.tileset_texture), ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(u0, v0), ImVec2(u1, v1));
            } else {
                draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(80, 120, 180, 255));
            }
        }
    }

    // Draw grid lines
    if (g_ed.settings.grid_lines && tile_px >= 4.0f) {
        const ImU32 grid_col = g_ed.settings.dark ? IM_COL32(56, 60, 68, 140) : IM_COL32(165, 170, 180, 160);
        for (int x = 0; x <= g_ed.doc.width; ++x) {
            const float gx = cell_to_screen_x(x);
            draw_list->AddLine(ImVec2(gx, map_y0), ImVec2(gx, map_y1), grid_col);
        }
        for (int y = 0; y <= g_ed.doc.height; ++y) {
            const float gy = cell_to_screen_y(y);
            draw_list->AddLine(ImVec2(map_x0, gy), ImVec2(map_x1, gy), grid_col);
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

    // Map boundary border
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
        g_ed.hovered_cell = {-1, -1};
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
        (cell_x >= g_ed.selection.x && cell_x < g_ed.selection.right() &&
         cell_y >= g_ed.selection.y && cell_y < g_ed.selection.bottom());

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
                    g_ed.floating_clip = g_ed.doc.copy_rect(g_ed.selection);
                    g_ed.doc.begin_stroke("Move Selection");
                    g_ed.doc.erase_rect(g_ed.selection);
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
            if (!mc.is_empty()) {
                g_ed.stamp_col = mc.atlas_x;
                g_ed.stamp_row = mc.atlas_y;
                g_ed.paint_mode = TileMode::Stamp;
                g_ed.status_msg = "Sampled tile (" + std::to_string(mc.atlas_x) + ", " + std::to_string(mc.atlas_y) + ")";
            }
            g_ed.is_drawing = false;
        }
    }

    if (g_ed.is_drawing) {
        if (g_ed.is_moving_paste) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const int dx = cell_x - g_ed.drag_start.x;
                const int dy = cell_y - g_ed.drag_start.y;
                const int max_x = std::max(0, g_ed.doc.width - g_ed.clipboard.w);
                const int max_y = std::max(0, g_ed.doc.height - g_ed.clipboard.h);
                g_ed.paste_pos.x = std::clamp(g_ed.paste_drag_origin.x + dx, 0, max_x);
                g_ed.paste_pos.y = std::clamp(g_ed.paste_drag_origin.y + dy, 0, max_y);
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
                const int max_x = std::max(0, g_ed.doc.width - g_ed.selection.w);
                const int max_y = std::max(0, g_ed.doc.height - g_ed.selection.h);
                g_ed.selection.x = std::clamp(g_ed.selection_drag_origin.x + dx, 0, max_x);
                g_ed.selection.y = std::clamp(g_ed.selection_drag_origin.y + dy, 0, max_y);
                g_ed.doc.set_clip_rect(&g_ed.selection);
            } else {
                g_ed.is_moving_selection = false;
                g_ed.is_drawing = false;
                g_ed.doc.set_clip_rect(&g_ed.selection);
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
                const int rx = std::clamp(std::min(g_ed.drag_start.x, cell_x), 0, g_ed.doc.width - 1);
                const int ry = std::clamp(std::min(g_ed.drag_start.y, cell_y), 0, g_ed.doc.height - 1);
                const int rx2 = std::clamp(std::max(g_ed.drag_start.x, cell_x), 0, g_ed.doc.width - 1);
                const int ry2 = std::clamp(std::max(g_ed.drag_start.y, cell_y), 0, g_ed.doc.height - 1);
                const int rw = rx2 - rx + 1;
                const int rh = ry2 - ry + 1;
                const float rpx0 = cell_to_screen_x(rx);
                const float rpy0 = cell_to_screen_y(ry);
                const float rpx1 = cell_to_screen_x(rx + rw);
                const float rpy1 = cell_to_screen_y(ry + rh);

                const ImU32 fill_col = g_ed.right_click_erasing ? IM_COL32(220, 50, 50, 70) : IM_COL32(60, 160, 240, 70);
                const ImU32 border_col = g_ed.right_click_erasing ? IM_COL32(255, 80, 80, 240) : IM_COL32(80, 180, 255, 240);

                if (g_ed.tool == Tool::Select || g_ed.rect_fill) {
                    draw_list->AddRectFilled(ImVec2(rpx0, rpy0), ImVec2(rpx1, rpy1), fill_col);
                    draw_list->AddRect(ImVec2(rpx0, rpy0), ImVec2(rpx1, rpy1), border_col, 0.0f, 0, 2.0f);
                } else {
                    const int bs = std::min(g_ed.brush_size, std::min(rw, rh));
                    const float inner_x0 = cell_to_screen_x(rx + bs);
                    const float inner_y0 = cell_to_screen_y(ry + bs);
                    const float inner_x1 = cell_to_screen_x(rx + rw - bs);
                    const float inner_y1 = cell_to_screen_y(ry + rh - bs);

                    if (bs * 2 >= rw || bs * 2 >= rh) {
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
                const int rx = std::min(g_ed.drag_start.x, cell_x);
                const int ry = std::min(g_ed.drag_start.y, cell_y);
                const int rw = std::abs(cell_x - g_ed.drag_start.x) + 1;
                const int rh = std::abs(cell_y - g_ed.drag_start.y) + 1;
                if (g_ed.rect_fill) {
                    if (g_ed.right_click_erasing) {
                        g_ed.doc.erase_rect({rx, ry, rw, rh});
                    } else {
                        g_ed.doc.fill_rect({rx, ry, rw, rh}, g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row);
                    }
                } else {
                    if (g_ed.right_click_erasing) {
                        g_ed.doc.erase_outline_rect({rx, ry, rw, rh}, g_ed.brush_size);
                    } else {
                        g_ed.doc.outline_rect({rx, ry, rw, rh}, g_ed.paint_mode, g_ed.stamp_col, g_ed.stamp_row, g_ed.brush_size);
                    }
                }
            } else if (g_ed.tool == Tool::Select) {
                const int rx = std::clamp(std::min(g_ed.drag_start.x, cell_x), 0, g_ed.doc.width - 1);
                const int ry = std::clamp(std::min(g_ed.drag_start.y, cell_y), 0, g_ed.doc.height - 1);
                const int rx2 = std::clamp(std::max(g_ed.drag_start.x, cell_x), 0, g_ed.doc.width - 1);
                const int ry2 = std::clamp(std::max(g_ed.drag_start.y, cell_y), 0, g_ed.doc.height - 1);
                g_ed.selection = {rx, ry, rx2 - rx + 1, ry2 - ry + 1};
                g_ed.has_selection = true;
                g_ed.selection_lifted = false;
                g_ed.floating_clip.clear();
                g_ed.doc.set_clip_rect(&g_ed.selection);
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
        draw_list->AddRect(ImVec2(spx0, spy0), ImVec2(spx1, spy1), box_col, 0.0f, 0, 2.0f);
        if (g_ed.paste_mode) {
            draw_list->AddRectFilled(ImVec2(spx0, spy0), ImVec2(spx1, spy1), IM_COL32(60, 180, 255, 40));
        } else if (g_ed.selection_lifted) {
            draw_list->AddRect(ImVec2(spx0 - 1, spy0 - 1), ImVec2(spx1 + 1, spy1 + 1), IM_COL32(0, 0, 0, 160), 0.0f, 0, 1.0f);
        }
    }

    draw_list->PopClipRect();
}

static void draw_sidebar_content(SDL_Renderer* renderer) {
    const ImVec4 sec_hdr_col = g_ed.settings.dark ? ImVec4(0.4f, 0.75f, 1.0f, 1.0f) : ImVec4(0.12f, 0.45f, 0.85f, 1.0f);

    // 1. Tileset Section
    ImGui::TextColored(sec_hdr_col, "TILESET");
    if (ImGui::Button("Import Tileset…", ImVec2(-1, 28))) {
        open_tileset_dialog(renderer);
    }
    if (ImGui::Button("Import Terrain / Variants…", ImVec2(-1, 24))) {
        open_terrain_dialog(renderer);
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

        // Interactive visual palette
        ImGui::Spacing();
        ImGui::BeginChild("AtlasScroll##Grid", ImVec2(0, 160), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        if (g_ed.tileset_texture && g_ed.doc.tileset.cols > 0 && g_ed.doc.tileset.rows > 0) {
            const float preview_scale = 2.0f;
            const float tile_ui_size = static_cast<float>(g_ed.doc.tileset.tile_size) * preview_scale;
            const float spacing = 2.0f;
            const float total_w = static_cast<float>(g_ed.doc.tileset.cols) * (tile_ui_size + spacing);
            const float total_h = static_cast<float>(g_ed.doc.tileset.rows) * (tile_ui_size + spacing);

            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(total_w, total_h));

            const bool grid_hovered = ImGui::IsItemHovered();
            const ImGuiIO& io = ImGui::GetIO();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
            if (platform_io.DrawCallback_SetSamplerNearest != nullptr) {
                dl->AddCallback(platform_io.DrawCallback_SetSamplerNearest, nullptr);
            }

            for (int r = 0; r < g_ed.doc.tileset.rows; ++r) {
                for (int c = 0; c < g_ed.doc.tileset.cols; ++c) {
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

                    if (g_ed.stamp_col == c && g_ed.stamp_row == r) {
                        dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(255, 220, 40, 255), 0, 0, 2.0f);
                    } else if (g_ed.doc.tileset.is_extra(c, r)) {
                        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(140, 90, 220, 160), 0, 0, 1.0f);
                    }

                    const uint8_t tile_col = g_ed.doc.tileset.get_tile_collision(c, r);
                    if (tile_col != 0) {
                        const CollisionType* ct = g_ed.doc.get_collision_type(tile_col);
                        const Rgb cr = ct ? ct->color : Rgb{235, 60, 50};
                        dl->AddRectFilled(ImVec2(x1 - 6, y1 - 6), ImVec2(x1 - 1, y1 - 1),
                                          IM_COL32(cr.r, cr.g, cr.b, 220));
                    }
                }
            }

            if (platform_io.DrawCallback_SetSamplerLinear != nullptr) {
                dl->AddCallback(platform_io.DrawCallback_SetSamplerLinear, nullptr);
            }

            if (grid_hovered) {
                const int hover_c = static_cast<int>((io.MousePos.x - p0.x) / (tile_ui_size + spacing));
                const int hover_r = static_cast<int>((io.MousePos.y - p0.y) / (tile_ui_size + spacing));
                if (hover_c >= 0 && hover_c < g_ed.doc.tileset.cols && hover_r >= 0 && hover_r < g_ed.doc.tileset.rows) {
                    const float hx0 = p0.x + hover_c * (tile_ui_size + spacing);
                    const float hy0 = p0.y + hover_r * (tile_ui_size + spacing);
                    dl->AddRect(ImVec2(hx0, hy0), ImVec2(hx0 + tile_ui_size, hy0 + tile_ui_size),
                                IM_COL32(255, 255, 255, 180), 0, 0, 1.5f);

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_ed.stamp_col = hover_c;
                        g_ed.stamp_row = hover_r;
                        g_ed.paint_mode = TileMode::Stamp;
                        g_ed.status_msg = "Selected stamp tile (" + std::to_string(hover_c) + ", " + std::to_string(hover_r) + ")";
                    }

                    const uint8_t tc = g_ed.doc.tileset.get_tile_collision(hover_c, hover_r);
                    const CollisionType* ct = g_ed.doc.get_collision_type(tc);
                    const std::string col_str = (tc == 0) ? "None" : (ct ? ct->name : "Type " + std::to_string(tc));
                    ImGui::SetTooltip("Tile (%d, %d)%s [Col: %s]", hover_c, hover_r,
                                      g_ed.doc.tileset.is_extra(hover_c, hover_r) ? " [Variant]" : "",
                                      col_str.c_str());
                }
            }
        }
        ImGui::EndChild();
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
    ImGui::Text("Size: %d × %d tiles (%d × %d px)", g_ed.doc.width, g_ed.doc.height,
                g_ed.doc.width * g_ed.doc.tile_size, g_ed.doc.height * g_ed.doc.tile_size);

    if (ImGui::Button("Resize Canvas…", ImVec2(-1, 26))) {
        g_ed.resize_w = g_ed.doc.width;
        g_ed.resize_h = g_ed.doc.height;
        g_ed.show_resize_modal = true;
    }
    if (ImGui::Button("Clear Map", ImVec2(-1, 26))) {
        g_ed.doc.begin_stroke("Clear Map");
        g_ed.doc.erase_rect({0, 0, g_ed.doc.width, g_ed.doc.height});
        g_ed.doc.end_stroke();
        g_ed.status_msg = "Cleared map.";
    }

    ImGui::Separator();
    // 3. Export Section
    ImGui::TextColored(sec_hdr_col, "EXPORT");
    if (ImGui::Button("Export Destination…", ImVec2(-1, 26))) {
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

    ImGui::Checkbox("Composite PNG", &g_ed.export_png);
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

    ImGui::Spacing();
    {
        ScopedStyleColor col(ImGuiCol_Button, ImVec4(0.2f, 0.58f, 0.35f, 1.0f));
        if (ImGui::Button("EXPORT ALL", ImVec2(-1, 36))) {
            execute_export();
        }
    }
}

static void draw_modals() {
    if (g_ed.show_resize_modal) {
        ImGui::OpenPopup("Resize Canvas##Modal");
    }
    if (ImGui::BeginPopupModal("Resize Canvas##Modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter new map size in tiles:");
        ImGui::InputInt("Width", &g_ed.resize_w);
        ImGui::InputInt("Height", &g_ed.resize_h);
        g_ed.resize_w = clampi(g_ed.resize_w, 1, 2048);
        g_ed.resize_h = clampi(g_ed.resize_h, 1, 2048);

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

        const bool losing = g_ed.doc.would_lose_tiles(g_ed.resize_w, g_ed.resize_h, g_ed.resize_anchor_x, g_ed.resize_anchor_y);
        if (losing) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Warning: Shrinking will cut off placed tiles!");
        }

        ImGui::Spacing();
        if (ImGui::Button("Apply", ImVec2(100, 28))) {
            g_ed.doc.resize(g_ed.resize_w, g_ed.resize_h, g_ed.resize_anchor_x, g_ed.resize_anchor_y);
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
        ImGui::InputInt("Width", &g_ed.new_w);
        ImGui::InputInt("Height", &g_ed.new_h);
        g_ed.new_w = clampi(g_ed.new_w, 1, 2048);
        g_ed.new_h = clampi(g_ed.new_h, 1, 2048);

        ImGui::Text("Tile Size:");
        ImGui::RadioButton("8x8", &g_ed.new_tile_size, 8);
        ImGui::SameLine();
        ImGui::RadioButton("16x16", &g_ed.new_tile_size, 16);

        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(100, 28))) {
            g_ed.doc.reset(g_ed.new_w, g_ed.new_h, g_ed.new_tile_size);
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
    if (g_ed.settings.brush_size >= 1 && g_ed.settings.brush_size <= 4) {
        g_ed.brush_size = g_ed.settings.brush_size;
    }
    g_ed.paint_mode = (g_ed.settings.paint_mode == 1) ? TileMode::Stamp : TileMode::Terrain;

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
            g_ed.doc.tile_size = g_ed.doc.tileset.tile_size;
            update_tileset_texture(renderer);
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Disable floating docking windows and disable ini file so everything stays fixed inside the main window
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    apply_theme(g_ed.settings.dark);

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
        g_ed.doc.set_clip_rect(g_ed.has_selection ? &g_ed.selection : nullptr);

        // Global shortcuts (processed after NewFrame so input events are current)
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
                if (g_ed.selection_lifted) {
                    g_ed.clipboard = g_ed.floating_clip;
                } else {
                    g_ed.clipboard = g_ed.doc.copy_rect(g_ed.selection);
                }
                g_ed.status_msg = "Copied selection.";
            }
            if (cmd && ImGui::IsKeyPressed(ImGuiKey_X) && g_ed.has_selection && !g_ed.paste_mode) {
                if (g_ed.selection_lifted) {
                    g_ed.clipboard = g_ed.floating_clip;
                    g_ed.floating_clip.clear();
                    g_ed.doc.end_stroke();
                    g_ed.selection_lifted = false;
                    g_ed.has_selection = false;
                    g_ed.doc.set_clip_rect(nullptr);
                } else {
                    g_ed.doc.cut_rect(g_ed.selection, g_ed.clipboard);
                    g_ed.has_selection = false;
                    g_ed.doc.set_clip_rect(nullptr);
                }
                g_ed.status_msg = "Cut selection.";
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
                        const int max_x = std::max(0, g_ed.doc.width - g_ed.clipboard.w);
                        const int max_y = std::max(0, g_ed.doc.height - g_ed.clipboard.h);
                        g_ed.paste_pos.x = std::clamp(g_ed.paste_pos.x + dx, 0, max_x);
                        g_ed.paste_pos.y = std::clamp(g_ed.paste_pos.y + dy, 0, max_y);
                        g_ed.selection.x = g_ed.paste_pos.x;
                        g_ed.selection.y = g_ed.paste_pos.y;
                        g_ed.doc.set_clip_rect(&g_ed.selection);
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
                    if (g_ed.selection_lifted) {
                        g_ed.doc.end_stroke();
                        g_ed.selection_lifted = false;
                        g_ed.floating_clip.clear();
                        g_ed.is_moving_selection = false;
                        g_ed.has_selection = false;
                        g_ed.doc.set_clip_rect(nullptr);
                        g_ed.status_msg = "Deleted selected tiles.";
                    } else {
                        g_ed.doc.erase_rect(g_ed.selection);
                        g_ed.status_msg = "Cleared selection.";
                    }
                }
                if (g_ed.has_selection && !g_ed.is_moving_selection) {
                    int dx = 0;
                    int dy = 0;
                    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  dx -= 1;
                    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) dx += 1;
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))    dy -= 1;
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))  dy += 1;
                    if (dx != 0 || dy != 0) {
                        const int max_x = std::max(0, g_ed.doc.width - g_ed.selection.w);
                        const int max_y = std::max(0, g_ed.doc.height - g_ed.selection.h);
                        const int target_x = std::clamp(g_ed.selection.x + dx, 0, max_x);
                        const int target_y = std::clamp(g_ed.selection.y + dy, 0, max_y);
                        if (target_x != g_ed.selection.x || target_y != g_ed.selection.y) {
                            if (!g_ed.selection_lifted) {
                                g_ed.selection_origin = g_ed.selection;
                                g_ed.floating_clip = g_ed.doc.copy_rect(g_ed.selection);
                                g_ed.doc.begin_stroke("Move Selection");
                                g_ed.doc.erase_rect(g_ed.selection);
                                g_ed.selection_lifted = true;
                            }
                            g_ed.selection.x = target_x;
                            g_ed.selection.y = target_y;
                            g_ed.doc.set_clip_rect(&g_ed.selection);
                            g_ed.status_msg = "Moved selection (floating). Press Ctrl+D or right-click to apply.";
                        }
                    }
                }
            }

            if (g_ed.view_mode == EditorViewMode::Tilemap && !cmd) {
                if (ImGui::IsKeyPressed(ImGuiKey_1)) g_ed.tool = Tool::Paint;
                if (ImGui::IsKeyPressed(ImGuiKey_2)) g_ed.tool = Tool::Line;
                if (ImGui::IsKeyPressed(ImGuiKey_3)) g_ed.tool = Tool::Erase;
                if (ImGui::IsKeyPressed(ImGuiKey_4)) g_ed.tool = Tool::Rect;
                if (ImGui::IsKeyPressed(ImGuiKey_5)) g_ed.tool = Tool::Fill;
                if (ImGui::IsKeyPressed(ImGuiKey_6)) g_ed.tool = Tool::Select;
                if (ImGui::IsKeyPressed(ImGuiKey_7)) g_ed.tool = Tool::Eyedropper;
            } else if (!cmd) {
                if (ImGui::IsKeyPressed(ImGuiKey_0)) g_ed.active_collision_type = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_1) && g_ed.doc.get_collision_type(1)) g_ed.active_collision_type = 1;
                if (ImGui::IsKeyPressed(ImGuiKey_2) && g_ed.doc.get_collision_type(2)) g_ed.active_collision_type = 2;
                if (ImGui::IsKeyPressed(ImGuiKey_3) && g_ed.doc.get_collision_type(3)) g_ed.active_collision_type = 3;
                if (ImGui::IsKeyPressed(ImGuiKey_4) && g_ed.doc.get_collision_type(4)) g_ed.active_collision_type = 4;
            }
        }

        // Main Menu Bar
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Map…", "Ctrl+N")) {
                    g_ed.show_new_modal = true;
                }
                if (ImGui::MenuItem("Open Map…", "Ctrl+O")) {
                    open_map_dialog(renderer);
                }
                if (ImGui::MenuItem("Save Map", "Ctrl+S")) {
                    save_map_dialog();
                }
                if (ImGui::MenuItem("Import Tileset…", "Ctrl+I")) {
                    open_tileset_dialog(renderer);
                }
                if (ImGui::MenuItem("Import Terrain / Variants…")) {
                    open_terrain_dialog(renderer);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Export All", "Ctrl+E")) {
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
                    if (g_ed.selection_lifted) {
                        g_ed.clipboard = g_ed.floating_clip;
                        g_ed.floating_clip.clear();
                        g_ed.doc.end_stroke();
                        g_ed.selection_lifted = false;
                        g_ed.has_selection = false;
                        g_ed.doc.set_clip_rect(nullptr);
                    } else {
                        g_ed.doc.cut_rect(g_ed.selection, g_ed.clipboard);
                        g_ed.has_selection = false;
                        g_ed.doc.set_clip_rect(nullptr);
                    }
                    g_ed.status_msg = "Cut selection.";
                }
                if (ImGui::MenuItem("Copy", "Ctrl+C", false, g_ed.has_selection && !g_ed.paste_mode)) {
                    if (g_ed.selection_lifted) {
                        g_ed.clipboard = g_ed.floating_clip;
                    } else {
                        g_ed.clipboard = g_ed.doc.copy_rect(g_ed.selection);
                    }
                    g_ed.status_msg = "Copied selection.";
                }
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, !g_ed.clipboard.is_empty())) {
                    start_paste();
                }
                if (ImGui::MenuItem("Clear Selection", "Del", false, g_ed.has_selection && !g_ed.paste_mode)) {
                    if (g_ed.selection_lifted) {
                        g_ed.doc.end_stroke();
                        g_ed.selection_lifted = false;
                        g_ed.floating_clip.clear();
                        g_ed.has_selection = false;
                        g_ed.doc.set_clip_rect(nullptr);
                        g_ed.status_msg = "Deleted selected tiles.";
                    } else {
                        g_ed.doc.erase_rect(g_ed.selection);
                        g_ed.status_msg = "Cleared selection.";
                    }
                }
                if (ImGui::MenuItem("Deselect", "Ctrl+D", false, g_ed.has_selection || g_ed.paste_mode)) {
                    deselect();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Resize Canvas…")) {
                    g_ed.resize_w = g_ed.doc.width;
                    g_ed.resize_h = g_ed.doc.height;
                    g_ed.show_resize_modal = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Tilemap Editor Mode", nullptr, g_ed.view_mode == EditorViewMode::Tilemap)) {
                    g_ed.view_mode = EditorViewMode::Tilemap;
                }
                if (ImGui::MenuItem("Tileset Collision Mode", nullptr, g_ed.view_mode == EditorViewMode::TilesetCollision)) {
                    g_ed.view_mode = EditorViewMode::TilesetCollision;
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
                    apply_theme(true);
                    persist_settings();
                }
                if (ImGui::MenuItem("Light Theme", nullptr, !g_ed.settings.dark)) {
                    g_ed.settings.dark = false;
                    apply_theme(false);
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
        draw_top_nav_and_view_row();
        ImGui::Separator();

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

        // 3. Status bar at bottom
        ImGui::Separator();
        ImGui::Text("%s", g_ed.status_msg.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 360);
        if (g_ed.view_mode == EditorViewMode::TilesetCollision) {
            if (g_ed.hovered_col_tile.x >= 0 && g_ed.hovered_col_tile.y >= 0) {
                const uint8_t cur_t = g_ed.doc.tileset.get_tile_collision(g_ed.hovered_col_tile.x, g_ed.hovered_col_tile.y);
                const CollisionType* ct = g_ed.doc.get_collision_type(cur_t);
                const std::string name = (cur_t == 0) ? "None" : (ct ? ct->name : "Type " + std::to_string(cur_t));
                ImGui::Text("Tile: (%d, %d) | Collision: %s", g_ed.hovered_col_tile.x, g_ed.hovered_col_tile.y, name.c_str());
            } else {
                const CollisionType* act = g_ed.doc.get_collision_type(g_ed.active_collision_type);
                const std::string act_name = (g_ed.active_collision_type == 0) ? "None" : (act ? act->name : "Type " + std::to_string(g_ed.active_collision_type));
                ImGui::Text("Collision Mode | Active: %s | %d Types", act_name.c_str(), static_cast<int>(g_ed.doc.collision_types.size()));
            }
        } else if (g_ed.hovered_cell.x >= 0 && g_ed.hovered_cell.y >= 0) {
            ImGui::Text("Cell: (%d, %d) | Map: %d×%d (%dpx)", g_ed.hovered_cell.x, g_ed.hovered_cell.y,
                        g_ed.doc.width, g_ed.doc.height, g_ed.doc.tile_size);
        } else {
            ImGui::Text("Map: %d×%d (%dpx tiles)", g_ed.doc.width, g_ed.doc.height, g_ed.doc.tile_size);
        }

        ImGui::End(); // MainLayout##Window

        draw_modals();

        // Render Frame
        ImGui::Render();
        if (g_ed.settings.dark) {
            SDL_SetRenderDrawColor(renderer, 24, 26, 30, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 235, 237, 242, 255);
        }
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
