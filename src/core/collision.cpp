#include "collision.h"

#include <cstdio>
#include <sstream>

namespace tmm {

int CollisionGrid::count_types_used() const {
    bool seen[256] = {false};
    int count = 0;
    for (uint8_t t : data) {
        if (t != 0 && !seen[t]) {
            seen[t] = true;
            count++;
        }
    }
    return count;
}

static const uint32_t kMdeCollisionValues[14] = {
    0x00000000, // 0: None / Empty
    0x0000000F, // 1: Solid (COLLISION_ALL)
    0x00000001, // 2: Top (COLLISION_TOP)
    0x00000002, // 3: Bottom (COLLISION_BOTTOM)
    0x00000004, // 4: Left (COLLISION_LEFT)
    0x00000008, // 5: Right (COLLISION_RIGHT)
    0x00000010, // 6: Ladder (TILE_PROP_LADDER)
    0x00000020, // 7: Tile Type 1 (TILE_PROP_TTYPE_1)
    0x00000040, // 8: Tile Type 2 (TILE_PROP_TTYPE_2)
    0x00000060, // 9: Tile Type 3 (TILE_PROP_TTYPE_3)
    0x00000080, // 10: Tile Type 4 (TILE_PROP_TTYPE_4)
    0x000000A0, // 11: Tile Type 5 (TILE_PROP_TTYPE_5)
    0x000000C0, // 12: Tile Type 6 (TILE_PROP_TTYPE_6)
    0x000000E0  // 13: Tile Type 7 (TILE_PROP_TTYPE_7)
};

uint32_t type_id_to_mde_value(uint8_t type_id) {
    if (type_id == 0) return 0;
    if (type_id <= 13) {
        return kMdeCollisionValues[type_id];
    }
    // Wrap to 1..13 if beyond 13
    const uint8_t wrapped = static_cast<uint8_t>(((type_id - 1) % 13) + 1);
    return kMdeCollisionValues[wrapped];
}

uint8_t mde_value_to_type_id(uint32_t val) {
    if (val == 0) return 0;
    // Check exact table match
    for (uint8_t i = 1; i <= 13; ++i) {
        if (kMdeCollisionValues[i] == val) {
            return i;
        }
    }
    // Check directional flags
    if ((val & 0x0F) == 0x0F) return 1; // Solid
    if (val & 0x01) return 2; // Top
    if (val & 0x02) return 3; // Bottom
    if (val & 0x04) return 4; // Left
    if (val & 0x08) return 5; // Right
    if (val & 0x10) return 6; // Ladder
    // Check tile type bits
    const uint32_t ttype = (val & 0xE0);
    for (uint8_t i = 7; i <= 13; ++i) {
        if (kMdeCollisionValues[i] == ttype) {
            return i;
        }
    }
    return 1; // Fallback to solid
}

std::string default_collision_type_name(uint8_t id) {
    switch (id) {
        case 1: return "Type 1 (Solid)";
        case 2: return "Type 2 (Top)";
        case 3: return "Type 3 (Bottom)";
        case 4: return "Type 4 (Left)";
        case 5: return "Type 5 (Right)";
        case 6: return "Type 6 (Ladder)";
        case 7: return "Type 7 (T1)";
        case 8: return "Type 8 (T2)";
        case 9: return "Type 9 (T3)";
        case 10: return "Type 10 (T4)";
        case 11: return "Type 11 (T5)";
        case 12: return "Type 12 (T6)";
        case 13: return "Type 13 (T7)";
        default: return "Type " + std::to_string(id);
    }
}

std::string compress_mde_collisions(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return "";
    }
    std::string parts;
    int64_t last_val = -1;
    int count = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        const uint32_t val = type_id_to_mde_value(data[i]);

        if (static_cast<int64_t>(val) != last_val) {
            if (count == 1) {
                parts += "!";
            } else if (count > 0) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%x+", count);
                parts += buf;
            }
            count = 0;
            last_val = static_cast<int64_t>(val);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08x", val);
            parts += buf;
        }
        count++;
    }
    if (count == 1) {
        parts += "!";
    } else if (count > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%x+", count);
        parts += buf;
    }
    return parts;
}

std::vector<uint8_t> decompress_mde_collisions(const std::string& rle, int expected_size) {
    std::vector<uint8_t> out;
    if (rle.empty()) {
        if (expected_size > 0) out.resize(static_cast<size_t>(expected_size), 0);
        return out;
    }
    out.reserve(expected_size > 0 ? static_cast<size_t>(expected_size) : 1024);

    size_t i = 0;
    while (i < rle.size()) {
        // Read 8 hex chars value
        if (i + 8 > rle.size()) break;
        std::string hex_val = rle.substr(i, 8);
        i += 8;
        uint32_t val = 0;
        std::stringstream ss;
        ss << std::hex << hex_val;
        ss >> val;

        const uint8_t type_val = mde_value_to_type_id(val);

        // Check if followed by '!' or '<hex>+'
        if (i < rle.size() && rle[i] == '!') {
            out.push_back(type_val);
            i++;
        } else {
            size_t plus_pos = rle.find('+', i);
            if (plus_pos != std::string::npos) {
                std::string count_hex = rle.substr(i, plus_pos - i);
                i = plus_pos + 1;
                uint32_t count = 0;
                std::stringstream css;
                css << std::hex << count_hex;
                css >> count;
                for (uint32_t c = 0; c < count; ++c) {
                    out.push_back(type_val);
                }
            } else {
                out.push_back(type_val);
            }
        }
    }
    return out;
}

} // namespace tmm
