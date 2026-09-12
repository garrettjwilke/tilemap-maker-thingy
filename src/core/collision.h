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

// Compresses 8x8 collision grid bytes into MD Engine's 32-bit unsigned RLE format.
// Type 0 uses 0x00000000, Type 1 uses 0x0000000F, Type N uses (((N - 1) * 0x20) | 0x0F).
std::string compress_mde_collisions(const std::vector<uint8_t>& data);

// Decompresses MDE RLE format string into byte array (useful for testing & roundtripping)
std::vector<uint8_t> decompress_mde_collisions(const std::string& rle, int expected_size);

} // namespace tmm
