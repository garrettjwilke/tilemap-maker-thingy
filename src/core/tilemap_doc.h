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
    int atlas_x = -1;
    int atlas_y = -1;
    float roll = 0.0f; // Random roll [0, 1) for variant resolution

    bool is_empty() const {
        return mode == TileMode::Empty || atlas_x < 0 || atlas_y < 0;
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
    int width = 30;   // In tiles
    int height = 20;  // In tiles
    int origin_x = 0;
    int origin_y = 0;
    int tile_size = 16;
    Tileset tileset;

    TilemapDoc(int w = 30, int h = 20, int ts = 16);

    void reset(int w = 30, int h = 20, int ts = 16);

    bool in_bounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height;
    }

    const MapCell& get_cell(int x, int y) const;
    void set_cell(int x, int y, const MapCell& cell);

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
    void flood_fill(int x, int y, TileMode mode, int stamp_col = -1, int stamp_row = -1);

    // Clipboard & Selection
    Clipboard copy_rect(const Rect& rect) const;
    void cut_rect(const Rect& rect, Clipboard& clip);
    void paste_clipboard(int x, int y, const Clipboard& clip);

    // Autotile solver
    void solve_autotiles_around(int x, int y, int radius = 2);
    void solve_all_autotiles();
    void reroll_variants(); // Re-rolls random chances for all terrain cells

    // Resize
    bool would_lose_tiles(int new_w, int new_h, int anchor_x, int anchor_y) const;
    void resize(int new_w, int new_h, int anchor_x, int anchor_y);

    // Collision generation
    CollisionGrid build_collision_grid() const;

    // Undo / Redo
    void begin_stroke(const std::string& action_name);
    void record_change(int x, int y);
    void end_stroke();

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
