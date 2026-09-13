#pragma once

#include "collision.h"
#include "tileset.h"
#include "types.h"

#include <string>
#include <vector>

namespace tmm {

enum class TileMode { Empty, Stamp, Terrain };

struct MapCell {
    TileMode mode = TileMode::Empty;
    int atlas_x = 10;
    int atlas_y = 1;
    float roll = 0.0f; // Random roll [0, 1) for variant resolution

    bool is_empty() const {
        return mode == TileMode::Empty || (atlas_x == 10 && atlas_y == 1) || atlas_x < 0 || atlas_y < 0;
    }
};

struct CellChange {
    Cell pos;
    MapCell old_cell;
    MapCell new_cell;
};

struct UndoAction {
    std::string name;
    int old_w = 0;
    int old_h = 0;
    int new_w = 0;
    int new_h = 0;
    std::vector<CellChange> changes;
};

struct Clipboard {
    int w = 0;
    int h = 0;
    std::vector<std::pair<Cell, MapCell>> cells; // Relative to top-left

    bool is_empty() const { return cells.empty(); }
    void clear() { cells.clear(); w = h = 0; }
};

class TilemapDoc {
public:
    std::string name = "untitled";
    int width = 20;   // In cells (20 cells of 16px = 40 tiles of 8px = 320px)
    int height = 14;  // In cells (14 cells of 16px = 28 tiles of 8px = 224px)
    int origin_x = 0;
    int origin_y = 0;
    int tile_size = 16;
    int buffer = 1;   // 1 tile buffer around the outside
    Tileset tileset;
    std::vector<CollisionType> collision_types;

    TilemapDoc(int w = 20, int h = 14, int ts = 16);

    void reset(int w = 20, int h = 14, int ts = 16);
    void reset_8px(int w_8px, int h_8px, int ts = 16);

    // Dimension metrics in cells, 8x8 tile units, and pixel units
    int factor() const { return (tile_size == 8) ? 1 : 2; }
    int total_width() const { return width + 2 * buffer; }
    int total_height() const { return height + 2 * buffer; }
    size_t total_cells() const { return static_cast<size_t>(total_width() * total_height()); }
    int width_8px() const { return width * factor(); }
    int height_8px() const { return height * factor(); }
    int pixel_width() const { return width * tile_size; }
    int pixel_height() const { return height * tile_size; }

    // Collision type management
    uint8_t add_collision_type();
    bool remove_collision_type(uint8_t id);
    void set_collision_type_color(uint8_t id, Rgb color);
    const CollisionType* get_collision_type(uint8_t id) const;
    CollisionType* get_collision_type(uint8_t id);

    // Bounds checking
    bool in_bounds(int x, int y) const {
        return x >= -buffer && y >= -buffer && x < width + buffer && y < height + buffer;
    }
    bool in_active_bounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height;
    }
    bool is_buffer_cell(int x, int y) const {
        return in_bounds(x, y) && !in_active_bounds(x, y);
    }
    size_t cell_index(int x, int y) const {
        return static_cast<size_t>((y + buffer) * total_width() + (x + buffer));
    }

    const MapCell& get_cell(int x, int y) const;
    void set_cell(int x, int y, const MapCell& cell);
    MapCell& cell_at(int x, int y);
    const MapCell& cell_at(int x, int y) const;
    void clear_cells();

    bool is_terrain(int x, int y) const {
        if (!in_bounds(x, y)) return false;
        return get_cell(x, y).mode == TileMode::Terrain;
    }

    // Drawing operations
    void paint_cell(int x, int y, TileMode mode, int stamp_col = -1, int stamp_row = -1, int brush_size = 1);
    void erase_cell(int x, int y, int brush_size = 1);
    void draw_line(int x0, int y0, int x1, int y1, TileMode mode, int stamp_col = -1, int stamp_row = -1, int brush_size = 1);
    void erase_line(int x0, int y0, int x1, int y1, int brush_size = 1);
    void fill_rect(const Rect& rect, TileMode mode, int stamp_col = -1, int stamp_row = -1);
    void outline_rect(const Rect& rect, TileMode mode, int stamp_col = -1, int stamp_row = -1, int brush_size = 1);
    void erase_rect(const Rect& rect);
    void erase_outline_rect(const Rect& rect, int brush_size = 1);
    void fill_ellipse(const Rect& rect, TileMode mode, int stamp_col = -1, int stamp_row = -1);
    void outline_ellipse(const Rect& rect, TileMode mode, int stamp_col = -1, int stamp_row = -1, int brush_size = 1);
    void erase_ellipse(const Rect& rect);
    void erase_outline_ellipse(const Rect& rect, int brush_size = 1);
    void flood_fill(int x, int y, TileMode mode, int stamp_col = -1, int stamp_row = -1);

    // Clipboard & Selection
    Clipboard copy_rect(const Rect& rect) const;
    Clipboard copy_ellipse(const Rect& rect) const;
    void cut_rect(const Rect& rect, Clipboard& clip);
    void cut_ellipse(const Rect& rect, Clipboard& clip);
    void paste_clipboard(int x, int y, const Clipboard& clip);

    // Autotile solver
    void solve_autotiles_around(int x, int y, int radius = 2);
    void solve_all_autotiles();
    void reroll_variants(); // Re-rolls random chances for all terrain cells

    // Resize
    bool would_lose_tiles(int new_w, int new_h, int anchor_x, int anchor_y) const;
    void resize(int new_w, int new_h, int anchor_x, int anchor_y);
    bool would_lose_tiles_8px(int new_w_8px, int new_h_8px, int anchor_x = -1, int anchor_y = -1) const;
    void resize_8px(int new_w_8px, int new_h_8px, int anchor_x = -1, int anchor_y = -1);

    // Collision generation
    CollisionGrid build_collision_grid() const;

    // Selection clipping
    enum class ClipShape { Rect, Ellipse };
    void set_clip_rect(const Rect* r, ClipShape shape = ClipShape::Rect) {
        if (r) {
            clip_rect_ = *r;
            clip_shape_ = shape;
            has_clip_ = true;
        } else {
            has_clip_ = false;
            clip_shape_ = ClipShape::Rect;
        }
    }
    const Rect* get_clip_rect() const { return has_clip_ ? &clip_rect_ : nullptr; }
    ClipShape get_clip_shape() const { return clip_shape_; }
    bool in_clip(int x, int y) const {
        if (!has_clip_) return true;
        if (clip_shape_ == ClipShape::Rect) {
            return (x >= clip_rect_.x && y >= clip_rect_.y &&
                    x < clip_rect_.right() && y < clip_rect_.bottom());
        } else {
            if (x < clip_rect_.x || y < clip_rect_.y || x >= clip_rect_.right() || y >= clip_rect_.bottom()) {
                return false;
            }
            const float cx = static_cast<float>(clip_rect_.x) + static_cast<float>(clip_rect_.w) * 0.5f;
            const float cy = static_cast<float>(clip_rect_.y) + static_cast<float>(clip_rect_.h) * 0.5f;
            const float rx = std::max(0.5f, static_cast<float>(clip_rect_.w) * 0.5f);
            const float ry = std::max(0.5f, static_cast<float>(clip_rect_.h) * 0.5f);
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float dx = (px - cx) / rx;
            const float dy = (py - cy) / ry;
            return (dx * dx + dy * dy) <= 1.0f;
        }
    }

    // Undo / Redo
    void begin_stroke(const std::string& action_name);
    void record_change(int x, int y);
    void end_stroke();
    void cancel_stroke();

    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }
    bool undo();
    bool redo();

    bool is_dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }
    void mark_dirty() { dirty_ = true; }

private:
    std::vector<MapCell> cells_;
    static const MapCell kEmptyCell;

    bool has_clip_ = false;
    ClipShape clip_shape_ = ClipShape::Rect;
    Rect clip_rect_{0, 0, 0, 0};

    bool stroke_in_progress_ = false;
    UndoAction current_action_;
    std::vector<CellChange> pending_changes_;
    std::vector<uint8_t> pending_recorded_;
    std::vector<UndoAction> undo_stack_;
    std::vector<UndoAction> redo_stack_;
    bool dirty_ = false;

    void record_cell_internal(int x, int y);
    void update_cell_autotile(int x, int y);
};

} // namespace tmm
