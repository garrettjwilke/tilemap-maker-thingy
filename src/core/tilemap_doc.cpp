#include "tilemap_doc.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace tmm {

const MapCell TilemapDoc::kEmptyCell = MapCell{};

namespace {
float random_01() {
    static thread_local std::mt19937 rng([]() {
        std::random_device rd;
        return rd();
    }());
    static thread_local std::uniform_real_distribution<float> dist(0.0001f, 0.9999f);
    return dist(rng);
}
} // namespace

TilemapDoc::TilemapDoc(int w, int h, int ts) {
    reset(w, h, ts);
}

void TilemapDoc::reset(int w, int h, int ts) {
    width = clampi(w, 1, 2048);
    height = clampi(h, 1, 2048);
    tile_size = (ts == 8) ? 8 : 16;
    name = "untitled";
    origin_x = 0;
    origin_y = 0;
    cells_.assign(static_cast<size_t>(width * height), MapCell{});
    undo_stack_.clear();
    redo_stack_.clear();
    dirty_ = false;

    collision_types.clear();
    collision_types.push_back(CollisionType{1, default_collision_type_name(1), Rgb{235, 60, 50}});
}

const MapCell& TilemapDoc::get_cell(int x, int y) const {
    if (!in_bounds(x, y)) return kEmptyCell;
    return cells_[static_cast<size_t>(y * width + x)];
}

void TilemapDoc::set_cell(int x, int y, const MapCell& cell) {
    if (!in_bounds(x, y)) return;
    record_cell_internal(x, y);
    cells_[static_cast<size_t>(y * width + x)] = cell;
    mark_dirty();
}

void TilemapDoc::record_cell_internal(int x, int y) {
    if (!stroke_in_progress_) return;
    if (!in_bounds(x, y)) return;
    const size_t idx = static_cast<size_t>(y * width + x);
    if (idx < pending_recorded_.size() && pending_recorded_[idx]) {
        return; // Already recorded old state
    }
    if (idx < pending_recorded_.size()) {
        pending_recorded_[idx] = 1;
    }
    CellChange ch;
    ch.pos = {x, y};
    ch.old_cell = get_cell(x, y);
    pending_changes_.push_back(ch);
}

void TilemapDoc::begin_stroke(const std::string& action_name) {
    if (stroke_in_progress_) {
        end_stroke();
    }
    stroke_in_progress_ = true;
    current_action_ = UndoAction{};
    current_action_.name = action_name;
    current_action_.old_w = width;
    current_action_.old_h = height;
    pending_changes_.clear();
    pending_recorded_.assign(static_cast<size_t>(width * height), 0);
}

void TilemapDoc::record_change(int x, int y) {
    record_cell_internal(x, y);
}

void TilemapDoc::end_stroke() {
    if (!stroke_in_progress_) return;
    stroke_in_progress_ = false;

    // Fill in new_cell for recorded changes
    for (auto& ch : pending_changes_) {
        ch.new_cell = get_cell(ch.pos.x, ch.pos.y);
    }
    if (!pending_changes_.empty() || current_action_.old_w != width || current_action_.old_h != height) {
        current_action_.new_w = width;
        current_action_.new_h = height;
        current_action_.changes = std::move(pending_changes_);
        undo_stack_.push_back(std::move(current_action_));
        if (undo_stack_.size() > 64) {
            undo_stack_.erase(undo_stack_.begin());
        }
        redo_stack_.clear();
    }
    pending_changes_.clear();
    pending_recorded_.clear();
}

bool TilemapDoc::undo() {
    if (undo_stack_.empty()) return false;
    UndoAction act = std::move(undo_stack_.back());
    undo_stack_.pop_back();

    // Check size change
    if (act.old_w != width || act.old_h != height) {
        width = act.old_w;
        height = act.old_h;
        cells_.assign(static_cast<size_t>(width * height), MapCell{});
    }

    for (const auto& ch : act.changes) {
        if (in_bounds(ch.pos.x, ch.pos.y)) {
            cells_[static_cast<size_t>(ch.pos.y * width + ch.pos.x)] = ch.old_cell;
        }
    }
    solve_all_autotiles();
    redo_stack_.push_back(std::move(act));
    mark_dirty();
    return true;
}

bool TilemapDoc::redo() {
    if (redo_stack_.empty()) return false;
    UndoAction act = std::move(redo_stack_.back());
    redo_stack_.pop_back();

    if (act.new_w != width || act.new_h != height) {
        width = act.new_w;
        height = act.new_h;
        cells_.assign(static_cast<size_t>(width * height), MapCell{});
    }

    for (const auto& ch : act.changes) {
        if (in_bounds(ch.pos.x, ch.pos.y)) {
            cells_[static_cast<size_t>(ch.pos.y * width + ch.pos.x)] = ch.new_cell;
        }
    }
    solve_all_autotiles();
    undo_stack_.push_back(std::move(act));
    mark_dirty();
    return true;
}

void TilemapDoc::update_cell_autotile(int x, int y) {
    if (!in_bounds(x, y)) return;
    MapCell& c = cells_[static_cast<size_t>(y * width + x)];
    if (c.mode != TileMode::Terrain) return;

    const bool nb_E  = is_terrain(x + 1, y);
    const bool nb_SE = is_terrain(x + 1, y + 1);
    const bool nb_S  = is_terrain(x, y + 1);
    const bool nb_SW = is_terrain(x - 1, y + 1);
    const bool nb_W  = is_terrain(x - 1, y);
    const bool nb_NW = is_terrain(x - 1, y - 1);
    const bool nb_N  = is_terrain(x, y - 1);
    const bool nb_NE = is_terrain(x + 1, y - 1);

    const uint8_t mask = static_cast<uint8_t>(
        (nb_E  ? 0x01 : 0) |
        (nb_SE ? 0x02 : 0) |
        (nb_S  ? 0x04 : 0) |
        (nb_SW ? 0x08 : 0) |
        (nb_W  ? 0x10 : 0) |
        (nb_NW ? 0x20 : 0) |
        (nb_N  ? 0x40 : 0) |
        (nb_NE ? 0x80 : 0)
    );

    const Cell root = Tileset::autotile_cell_for_mask(mask);
    const Cell chosen = tileset.resolve_variant(root.x, root.y, c.roll);
    c.atlas_x = chosen.x;
    c.atlas_y = chosen.y;
}

void TilemapDoc::solve_autotiles_around(int cx, int cy, int radius) {
    for (int y = cy - radius; y <= cy + radius; ++y) {
        for (int x = cx - radius; x <= cx + radius; ++x) {
            update_cell_autotile(x, y);
        }
    }
}

void TilemapDoc::solve_all_autotiles() {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            update_cell_autotile(x, y);
        }
    }
}

void TilemapDoc::reroll_variants() {
    begin_stroke("Reroll Variants");
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            MapCell& c = cells_[static_cast<size_t>(y * width + x)];
            if (c.mode == TileMode::Terrain) {
                record_cell_internal(x, y);
                c.roll = random_01();
            } else if (c.mode == TileMode::Stamp && (c.atlas_x == 9 && c.atlas_y == 2 || tileset.is_extra(c.atlas_x, c.atlas_y))) {
                record_cell_internal(x, y);
                const Cell chosen = tileset.resolve_variant(9, 2, random_01());
                c.atlas_x = chosen.x;
                c.atlas_y = chosen.y;
            }
        }
    }
    solve_all_autotiles();
    end_stroke();
}

void TilemapDoc::paint_cell(int x, int y, TileMode mode, int stamp_col, int stamp_row, int brush_size) {
    brush_size = std::max(brush_size, 1);
    for (int by = 0; by < brush_size; ++by) {
        for (int bx = 0; bx < brush_size; ++bx) {
            const int cx = x + bx;
            const int cy = y + by;
            if (!in_bounds(cx, cy)) continue;
            record_cell_internal(cx, cy);
            MapCell& c = cells_[static_cast<size_t>(cy * width + cx)];
            c.mode = mode;
            if (mode == TileMode::Terrain) {
                if (c.roll == 0.0f) c.roll = random_01();
            } else if (mode == TileMode::Stamp) {
                c.atlas_x = stamp_col;
                c.atlas_y = stamp_row;
            } else {
                c = MapCell{};
            }
        }
    }
    solve_autotiles_around(x, y, brush_size + 1);
    mark_dirty();
}

void TilemapDoc::erase_cell(int x, int y, int brush_size) {
    brush_size = std::max(brush_size, 1);
    for (int by = 0; by < brush_size; ++by) {
        for (int bx = 0; bx < brush_size; ++bx) {
            const int cx = x + bx;
            const int cy = y + by;
            if (!in_bounds(cx, cy)) continue;
            record_cell_internal(cx, cy);
            cells_[static_cast<size_t>(cy * width + cx)] = MapCell{};
        }
    }
    solve_autotiles_around(x, y, brush_size + 1);
    mark_dirty();
}

void TilemapDoc::draw_line(int x0, int y0, int x1, int y1, TileMode mode, int stamp_col, int stamp_row, int brush_size) {
    begin_stroke(mode == TileMode::Terrain ? "Draw Line Terrain" : "Draw Line Stamp");
    brush_size = std::max(brush_size, 1);

    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    int x = x0;
    int y = y0;

    while (true) {
        for (int by = 0; by < brush_size; ++by) {
            for (int bx = 0; bx < brush_size; ++bx) {
                const int cx = x + bx;
                const int cy = y + by;
                if (!in_bounds(cx, cy)) continue;
                record_cell_internal(cx, cy);
                MapCell& c = cells_[static_cast<size_t>(cy * width + cx)];
                c.mode = mode;
                if (mode == TileMode::Terrain) {
                    if (c.roll == 0.0f) c.roll = random_01();
                } else if (mode == TileMode::Stamp) {
                    c.atlas_x = stamp_col;
                    c.atlas_y = stamp_row;
                } else {
                    c = MapCell{};
                }
            }
        }
        if (x == x1 && y == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }

    solve_all_autotiles();
    end_stroke();
    mark_dirty();
}

void TilemapDoc::erase_line(int x0, int y0, int x1, int y1, int brush_size) {
    begin_stroke("Erase Line");
    brush_size = std::max(brush_size, 1);

    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    int x = x0;
    int y = y0;

    while (true) {
        for (int by = 0; by < brush_size; ++by) {
            for (int bx = 0; bx < brush_size; ++bx) {
                const int cx = x + bx;
                const int cy = y + by;
                if (!in_bounds(cx, cy)) continue;
                record_cell_internal(cx, cy);
                cells_[static_cast<size_t>(cy * width + cx)] = MapCell{};
            }
        }
        if (x == x1 && y == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }

    solve_all_autotiles();
    end_stroke();
    mark_dirty();
}

void TilemapDoc::outline_rect(const Rect& rect, TileMode mode, int stamp_col, int stamp_row, int brush_size) {
    begin_stroke(mode == TileMode::Terrain ? "Outline Rect Terrain" : "Outline Rect Stamp");
    brush_size = std::max(brush_size, 1);

    for (int y = rect.y; y < rect.bottom(); ++y) {
        for (int x = rect.x; x < rect.right(); ++x) {
            if (!in_bounds(x, y)) continue;
            const bool is_border = (x < rect.x + brush_size || x >= rect.right() - brush_size ||
                                    y < rect.y + brush_size || y >= rect.bottom() - brush_size);
            if (!is_border) continue;

            record_cell_internal(x, y);
            MapCell& c = cells_[static_cast<size_t>(y * width + x)];
            c.mode = mode;
            if (mode == TileMode::Terrain) {
                c.roll = random_01();
            } else if (mode == TileMode::Stamp) {
                c.atlas_x = stamp_col;
                c.atlas_y = stamp_row;
            }
        }
    }
    solve_all_autotiles();
    end_stroke();
    mark_dirty();
}

void TilemapDoc::erase_outline_rect(const Rect& rect, int brush_size) {
    begin_stroke("Erase Outline Rect");
    brush_size = std::max(brush_size, 1);

    for (int y = rect.y; y < rect.bottom(); ++y) {
        for (int x = rect.x; x < rect.right(); ++x) {
            if (!in_bounds(x, y)) continue;
            const bool is_border = (x < rect.x + brush_size || x >= rect.right() - brush_size ||
                                    y < rect.y + brush_size || y >= rect.bottom() - brush_size);
            if (!is_border) continue;

            record_cell_internal(x, y);
            cells_[static_cast<size_t>(y * width + x)] = MapCell{};
        }
    }
    solve_all_autotiles();
    end_stroke();
    mark_dirty();
}

void TilemapDoc::fill_rect(const Rect& rect, TileMode mode, int stamp_col, int stamp_row) {
    begin_stroke(mode == TileMode::Terrain ? "Paint Rect Terrain" : "Paint Rect Stamp");
    for (int y = rect.y; y < rect.bottom(); ++y) {
        for (int x = rect.x; x < rect.right(); ++x) {
            if (!in_bounds(x, y)) continue;
            record_cell_internal(x, y);
            MapCell& c = cells_[static_cast<size_t>(y * width + x)];
            c.mode = mode;
            if (mode == TileMode::Terrain) {
                c.roll = random_01();
            } else if (mode == TileMode::Stamp) {
                c.atlas_x = stamp_col;
                c.atlas_y = stamp_row;
            }
        }
    }
    solve_all_autotiles();
    end_stroke();
}

void TilemapDoc::erase_rect(const Rect& rect) {
    begin_stroke("Erase Rect");
    for (int y = rect.y; y < rect.bottom(); ++y) {
        for (int x = rect.x; x < rect.right(); ++x) {
            if (!in_bounds(x, y)) continue;
            record_cell_internal(x, y);
            cells_[static_cast<size_t>(y * width + x)] = MapCell{};
        }
    }
    solve_all_autotiles();
    end_stroke();
}

void TilemapDoc::flood_fill(int start_x, int start_y, TileMode mode, int stamp_col, int stamp_row) {
    if (!in_bounds(start_x, start_y)) return;
    const MapCell target_cell = get_cell(start_x, start_y);
    if (mode == target_cell.mode) {
        if (mode == TileMode::Stamp && target_cell.atlas_x == stamp_col && target_cell.atlas_y == stamp_row) {
            return;
        }
    }

    begin_stroke("Flood Fill");
    std::vector<Cell> queue = {{start_x, start_y}};
    std::vector<bool> visited(static_cast<size_t>(width * height), false);
    visited[static_cast<size_t>(start_y * width + start_x)] = true;

    auto matches = [&](int x, int y) -> bool {
        if (!in_bounds(x, y)) return false;
        const MapCell& c = get_cell(x, y);
        if (mode == TileMode::Terrain && target_cell.mode == TileMode::Terrain) {
            return c.mode == TileMode::Terrain;
        }
        if (c.mode != target_cell.mode) return false;
        if (target_cell.mode == TileMode::Stamp) {
            return c.atlas_x == target_cell.atlas_x && c.atlas_y == target_cell.atlas_y;
        }
        return true;
    };

    while (!queue.empty()) {
        const Cell cur = queue.back();
        queue.pop_back();

        record_cell_internal(cur.x, cur.y);
        MapCell& c = cells_[static_cast<size_t>(cur.y * width + cur.x)];
        c.mode = mode;
        if (mode == TileMode::Terrain) {
            c.roll = random_01();
        } else if (mode == TileMode::Stamp) {
            c.atlas_x = stamp_col;
            c.atlas_y = stamp_row;
        } else {
            c = MapCell{};
        }

        const int dx[] = {1, -1, 0, 0};
        const int dy[] = {0, 0, 1, -1};
        for (int i = 0; i < 4; ++i) {
            const int nx = cur.x + dx[i];
            const int ny = cur.y + dy[i];
            if (in_bounds(nx, ny) && !visited[static_cast<size_t>(ny * width + nx)] && matches(nx, ny)) {
                visited[static_cast<size_t>(ny * width + nx)] = true;
                queue.push_back({nx, ny});
            }
        }
    }
    solve_all_autotiles();
    end_stroke();
}

Clipboard TilemapDoc::copy_rect(const Rect& rect) const {
    Clipboard clip;
    clip.w = rect.w;
    clip.h = rect.h;
    for (int y = rect.y; y < rect.bottom(); ++y) {
        for (int x = rect.x; x < rect.right(); ++x) {
            if (in_bounds(x, y)) {
                clip.cells.push_back({{x - rect.x, y - rect.y}, get_cell(x, y)});
            }
        }
    }
    return clip;
}

void TilemapDoc::cut_rect(const Rect& rect, Clipboard& clip) {
    clip = copy_rect(rect);
    erase_rect(rect);
}

void TilemapDoc::paste_clipboard(int x, int y, const Clipboard& clip) {
    if (clip.is_empty()) return;
    begin_stroke("Paste");
    for (const auto& item : clip.cells) {
        const int dest_x = x + item.first.x;
        const int dest_y = y + item.first.y;
        if (in_bounds(dest_x, dest_y)) {
            record_cell_internal(dest_x, dest_y);
            cells_[static_cast<size_t>(dest_y * width + dest_x)] = item.second;
        }
    }
    solve_all_autotiles();
    end_stroke();
}

bool TilemapDoc::would_lose_tiles(int new_w, int new_h, int anchor_x, int anchor_y) const {
    new_w = clampi(new_w, 1, 2048);
    new_h = clampi(new_h, 1, 2048);
    const int dx = new_w - width;
    const int dy = new_h - height;
    int left_add = 0;
    if (anchor_x > 0) left_add = dx;
    else if (anchor_x == 0) left_add = dx / 2;

    int top_add = 0;
    if (anchor_y > 0) top_add = dy;
    else if (anchor_y == 0) top_add = dy / 2;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const MapCell& c = get_cell(x, y);
            if (!c.is_empty()) {
                const int nx = x + left_add;
                const int ny = y + top_add;
                if (nx < 0 || ny < 0 || nx >= new_w || ny >= new_h) {
                    return true;
                }
            }
        }
    }
    return false;
}

void TilemapDoc::resize(int new_w, int new_h, int anchor_x, int anchor_y) {
    new_w = clampi(new_w, 1, 2048);
    new_h = clampi(new_h, 1, 2048);
    if (new_w == width && new_h == height) return;

    begin_stroke("Resize Canvas");
    const int dx = new_w - width;
    const int dy = new_h - height;
    int left_add = 0;
    if (anchor_x > 0) left_add = dx;
    else if (anchor_x == 0) left_add = dx / 2;

    int top_add = 0;
    if (anchor_y > 0) top_add = dy;
    else if (anchor_y == 0) top_add = dy / 2;

    // Record all existing cells for undo
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            record_cell_internal(x, y);
        }
    }

    std::vector<MapCell> new_cells(static_cast<size_t>(new_w * new_h), MapCell{});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int nx = x + left_add;
            const int ny = y + top_add;
            if (nx >= 0 && ny >= 0 && nx < new_w && ny < new_h) {
                new_cells[static_cast<size_t>(ny * new_w + nx)] = cells_[static_cast<size_t>(y * width + x)];
            }
        }
    }
    width = new_w;
    height = new_h;
    cells_ = std::move(new_cells);
    solve_all_autotiles();
    end_stroke();
    mark_dirty();
}

uint8_t TilemapDoc::add_collision_type() {
    static const Rgb kPalette[13] = {
        {235, 60, 50},   // 1: Red (Solid)
        {40, 180, 100},  // 2: Green (Top)
        {60, 130, 240},  // 3: Blue (Bottom)
        {235, 190, 40},  // 4: Yellow (Left)
        {160, 80, 230},  // 5: Purple (Right)
        {30, 200, 240},  // 6: Cyan (Ladder)
        {245, 120, 30},  // 7: Orange (Tile Type 1)
        {240, 90, 180},  // 8: Magenta (Tile Type 2)
        {32, 178, 170},  // 9: Teal (Tile Type 3)
        {150, 220, 40},  // 10: Lime (Tile Type 4)
        {255, 105, 180}, // 11: Pink (Tile Type 5)
        {255, 160, 0},   // 12: Amber (Tile Type 6)
        {120, 110, 240}  // 13: Indigo (Tile Type 7)
    };
    if (collision_types.size() >= 13) {
        return 0;
    }
    uint8_t max_id = 0;
    for (const auto& ct : collision_types) {
        if (ct.id > max_id) max_id = ct.id;
    }
    const uint8_t new_id = max_id + 1;
    CollisionType ct;
    ct.id = new_id;
    ct.name = default_collision_type_name(new_id);
    const size_t pal_idx = static_cast<size_t>((new_id - 1) % 13);
    ct.color = kPalette[pal_idx];
    collision_types.push_back(ct);
    mark_dirty();
    return new_id;
}

bool TilemapDoc::remove_collision_type(uint8_t id) {
    if (collision_types.size() <= 1) {
        return false;
    }
    auto it = std::find_if(collision_types.begin(), collision_types.end(),
                           [id](const CollisionType& ct) { return ct.id == id; });
    if (it == collision_types.end()) return false;
    collision_types.erase(it);

    for (auto& tc : tileset.tile_collisions) {
        if (tc == id) tc = 1;
    }
    mark_dirty();
    return true;
}

void TilemapDoc::set_collision_type_color(uint8_t id, Rgb color) {
    for (auto& ct : collision_types) {
        if (ct.id == id) {
            ct.color = color;
            mark_dirty();
            break;
        }
    }
}

const CollisionType* TilemapDoc::get_collision_type(uint8_t id) const {
    for (const auto& ct : collision_types) {
        if (ct.id == id) return &ct;
    }
    return nullptr;
}

CollisionGrid TilemapDoc::build_collision_grid() const {
    CollisionGrid grid;
    // Collision tile size is always 8x8 px
    const int factor = tile_size / 8; // 1 for 8px, 2 for 16px
    grid.width = width * factor;
    grid.height = height * factor;
    grid.data.assign(static_cast<size_t>(grid.width * grid.height), 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const MapCell& c = get_cell(x, y);
            if (!c.is_empty()) {
                const uint8_t col_type = tileset.get_tile_collision(c.atlas_x, c.atlas_y);
                if (col_type != 0) {
                    for (int fy = 0; fy < factor; ++fy) {
                        for (int fx = 0; fx < factor; ++fx) {
                            grid.set_type(x * factor + fx, y * factor + fy, col_type);
                        }
                    }
                }
            }
        }
    }
    return grid;
}

} // namespace tmm
