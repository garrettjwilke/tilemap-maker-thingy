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

std::string compress_mde_collisions(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return "";
    }
    std::string parts;
    int64_t last_val = -1;
    int count = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        const uint8_t type_id = data[i];
        uint32_t val = 0;
        if (type_id == 1) {
            val = 0x0F;
        } else if (type_id > 1) {
            val = static_cast<uint32_t>(((type_id - 1) * 0x20) | 0x0F);
        }

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

        uint8_t type_val = 0;
        if (val == 0) {
            type_val = 0;
        } else if ((val & 0xE0) == 0) {
            type_val = 1;
        } else {
            type_val = static_cast<uint8_t>(((val & 0xE0) / 0x20) + 1);
        }

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
