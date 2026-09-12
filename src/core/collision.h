#pragma once

#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tmm {

struct CollisionType {
    uint8_t id = 1;
    std::string name = "Type 1";
    Rgb color{235, 60, 50};
};

struct CollisionGrid {
    int width = 0;   // In 8x8 collision cells
    int height = 0;  // In 8x8 collision cells
    std::vector<uint8_t> data; // 0 = none, 1..N = collision type ID

    uint8_t get_type(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        return data[static_cast<size_t>(y * width + x)];
    }

    void set_type(int x, int y, uint8_t type) {
        if (x >= 0 && y >= 0 && x < width && y < height) {
            data[static_cast<size_t>(y * width + x)] = type;
        }
    }

    bool is_solid(int x, int y) const {
        return get_type(x, y) != 0;
    }

    void set_solid(int x, int y, bool solid) {
        set_type(x, y, solid ? 1 : 0);
    }

    int count_types_used() const;
};

// MD Engine 32-bit collision values for type IDs (1..13):
// 0: None / Empty    (0x00000000)
// 1: Solid           (0x0000000F, COLLISION_ALL)
// 2: Top             (0x00000001, COLLISION_TOP)
// 3: Bottom          (0x00000002, COLLISION_BOTTOM)
// 4: Left            (0x00000004, COLLISION_LEFT)
// 5: Right           (0x00000008, COLLISION_RIGHT)
// 6: Ladder          (0x00000010, TILE_PROP_LADDER)
// 7: Tile Type 1     (0x00000020, TILE_PROP_TTYPE_1)
// 8: Tile Type 2     (0x00000040, TILE_PROP_TTYPE_2)
// 9: Tile Type 3     (0x00000060, TILE_PROP_TTYPE_3)
// 10: Tile Type 4    (0x00000080, TILE_PROP_TTYPE_4)
// 11: Tile Type 5    (0x000000A0, TILE_PROP_TTYPE_5)
// 12: Tile Type 6    (0x000000C0, TILE_PROP_TTYPE_6)
// 13: Tile Type 7    (0x000000E0, TILE_PROP_TTYPE_7)
uint32_t type_id_to_mde_value(uint8_t type_id);
uint8_t mde_value_to_type_id(uint32_t val);
std::string default_collision_type_name(uint8_t id);

// Compresses 8x8 collision grid bytes into MD Engine's 32-bit unsigned RLE format.
std::string compress_mde_collisions(const std::vector<uint8_t>& data);

// Decompresses MDE RLE format string into byte array (useful for testing & roundtripping)
std::vector<uint8_t> decompress_mde_collisions(const std::string& rle, int expected_size);

} // namespace tmm
