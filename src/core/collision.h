#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tmm {

struct CollisionGrid {
    int width = 0;   // In 8x8 collision cells
    int height = 0;  // In 8x8 collision cells
    std::vector<uint8_t> data; // 1 = solid, 0 = empty

    bool is_solid(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return false;
        return data[static_cast<size_t>(y * width + x)] != 0;
    }

    void set_solid(int x, int y, bool solid) {
        if (x >= 0 && y >= 0 && x < width && y < height) {
            data[static_cast<size_t>(y * width + x)] = solid ? 1 : 0;
        }
    }
};

// Compresses 8x8 collision grid bytes (0 or 1) into MD Engine's 32-bit unsigned RLE format.
// Solid tiles use 0x0000000F (COLLISION_ALL), empty tiles use 0x00000000.
std::string compress_mde_collisions(const std::vector<uint8_t>& data);

// Decompresses MDE RLE format string into byte array (useful for testing & roundtripping)
std::vector<uint8_t> decompress_mde_collisions(const std::string& rle, int expected_size);

} // namespace tmm
