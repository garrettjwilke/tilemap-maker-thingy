#pragma once

#include "types.h"

#include <string>
#include <vector>

namespace tmm {

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
        if (col == 10 && row == 1) {
            const size_t idx = static_cast<size_t>(row * cols + col);
            if (idx < tile_collisions.size()) {
                return tile_collisions[idx];
            }
            return 0; // (10, 1) is always empty in standard autotiles
        }
        const size_t idx = static_cast<size_t>(row * cols + col);
        if (idx < tile_collisions.size()) {
            return tile_collisions[idx];
        }
        return 1;
    }

    void set_tile_collision(int col, int row, uint8_t type) {
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
        if (default_type != 0 && in_bounds(10, 1)) {
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

    // Helpers
    std::vector<VariantBinding> variants_for_root(int root_x, int root_y) const;
};

} // namespace tmm
