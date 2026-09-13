#pragma once

#include "types.h"

#include <algorithm>
#include <string>
#include <vector>

namespace tmm {

struct TilesetPreviewLayout {
    float tile_ui_size = 0.0f;
    float total_w = 0.0f;
    float total_h = 0.0f;
    float child_h = 0.0f;
    bool needs_vscroll = false;
};

inline TilesetPreviewLayout compute_tileset_preview_layout(
    float outer_w, float avail_sidebar_h,
    int cols, int rows,
    float spacing = 1.0f,
    float pad_x = 4.0f, float pad_y = 4.0f,
    float border_size = 1.0f,
    float scrollbar_size = 14.0f)
{
    TilesetPreviewLayout layout{};
    if (cols <= 0 || rows <= 0 || outer_w <= 0.0f) {
        return layout;
    }
    const float total_gaps_x = (cols > 1) ? static_cast<float>(cols - 1) * spacing : 0.0f;
    const float total_gaps_y = (rows > 1) ? static_cast<float>(rows - 1) * spacing : 0.0f;

    float inner_w = std::max(1.0f, outer_w - (border_size * 2.0f + pad_x * 2.0f));
    float tile_ui_size = std::max(1.0f, (inner_w - total_gaps_x) / static_cast<float>(cols));
    float total_h = static_cast<float>(rows) * tile_ui_size + total_gaps_y;
    const float content_h = total_h + (border_size * 2.0f + pad_y * 2.0f);

    const float max_child_h = std::max(180.0f, std::min(420.0f, avail_sidebar_h - 140.0f));
    float child_h = content_h;
    bool needs_vscroll = false;
    if (content_h > max_child_h) {
        needs_vscroll = true;
        child_h = max_child_h;
        inner_w = std::max(1.0f, inner_w - scrollbar_size);
        tile_ui_size = std::max(1.0f, (inner_w - total_gaps_x) / static_cast<float>(cols));
        total_h = static_cast<float>(rows) * tile_ui_size + total_gaps_y;
    }

    layout.tile_ui_size = tile_ui_size;
    layout.total_w = cols * tile_ui_size + total_gaps_x;
    layout.total_h = total_h;
    layout.child_h = child_h;
    layout.needs_vscroll = needs_vscroll;
    return layout;
}

struct VariantBinding {
    int x = 0;
    int y = 0;
    int root_x = 0;
    int root_y = 0;
    float probability = 0.3f;
};

class Tileset {
public:
    static constexpr int kBaseCols = 12;
    static constexpr int kBaseRows = 4;
    static constexpr int kDefaultCenterCol = 9;
    static constexpr int kDefaultCenterRow = 2;

    int tile_size = 16;
    int cols = kBaseCols;
    int rows = kBaseRows;
    std::vector<Rgb> palette;
    std::vector<uint8_t> pixels; // Indexed pixels: (cols * tile_size) * (rows * tile_size)
    std::vector<VariantBinding> variants;
    std::vector<uint8_t> tile_collisions;
    std::string png_path;
    std::string terrain_path;
    std::string error;

    Tileset() {
        init_tile_collisions(1);
    }

    bool is_valid() const { return !pixels.empty() && cols > 0 && rows > 0 && tile_size > 0; }
    int image_width() const { return cols * tile_size; }
    int image_height() const { return rows * tile_size; }

    bool in_bounds(int col, int row) const {
        return col >= 0 && row >= 0 && col < cols && row < rows;
    }

    uint8_t get_tile_collision(int col, int row) const {
        if (col < 0 || row < 0 || col >= cols || row >= rows) return 0;
        if (col == 10 && row == 1) return 0; // (10, 1) is always empty / no collision
        const size_t idx = static_cast<size_t>(row * cols + col);
        if (idx < tile_collisions.size()) {
            return tile_collisions[idx];
        }
        return 1;
    }

    void set_tile_collision(int col, int row, uint8_t type) {
        if (col == 10 && row == 1) return; // (10, 1) is always empty / no collision
        if (col >= 0 && row >= 0 && col < cols && row < rows) {
            const size_t idx = static_cast<size_t>(row * cols + col);
            if (tile_collisions.size() < static_cast<size_t>(cols * rows)) {
                init_tile_collisions(1);
            }
            tile_collisions[idx] = type;
        }
    }

    void init_tile_collisions(uint8_t default_type = 1) {
        tile_collisions.assign(static_cast<size_t>(cols * rows), default_type);
        if (in_bounds(10, 1)) {
            tile_collisions[static_cast<size_t>(1 * cols + 10)] = 0;
        }
    }

    bool is_extra(int col, int row) const {
        return col >= kBaseCols || row >= kBaseRows;
    }

    uint8_t get_pixel(int col, int row, int px, int py) const;
    std::vector<uint8_t> get_tile_pixels(int col, int row) const;

    // Autotile 47-peering lookup:
    // Mask bits: 0x01=E, 0x02=SE, 0x04=S, 0x08=SW, 0x10=W, 0x20=NW, 0x40=N, 0x80=NE
    static Cell autotile_cell_for_mask(uint8_t mask);
    static uint8_t canonicalize_mask(uint8_t mask);

    // Resolves root tile to variant based on probabilities:
    Cell resolve_variant(int root_x, int root_y, float roll01) const;

    // Loading methods:
    bool load_from_file(const std::string& path);
    bool load_png_file(const std::string& path);
    bool load_png_raw(const std::string& path);
    bool load_terrain_file(const std::string& path);
    bool load_header_file(const std::string& path);
    bool import_variants_file(const std::string& path);
    void parse_terrain_text(const std::string& text, const std::string& path);
    bool parse_c_header(const std::string& text, const std::string& path);

    // Helpers and variant management
    static bool is_base_origin_tile(int col, int row) {
        return col >= 0 && col < kBaseCols && row >= 0 && row < kBaseRows;
    }
    static bool is_variant_tile(int col, int row) {
        return col >= kBaseCols && row >= 0;
    }
    std::vector<VariantBinding> variants_for_root(int root_x, int root_y) const;
    const VariantBinding* find_variant(int x, int y) const;
    VariantBinding* find_variant(int x, int y);
    bool is_variant(int x, int y) const;
    bool is_origin(int x, int y) const;
    int count_variants_for_root(int root_x, int root_y) const;
    bool set_variant(int x, int y, int root_x, int root_y, float probability = 0.3f);
    bool remove_variant(int x, int y);
    void remove_variants_for_root(int root_x, int root_y);
    void clear_variants();
    void auto_bind_extra_columns(int root_x = kDefaultCenterCol, int root_y = kDefaultCenterRow, float probability = 1.0f);
    std::string default_terrain_path() const;
};

} // namespace tmm
