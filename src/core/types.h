#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace tmm {

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb& o) const { return !(*this == o); }
};

struct Cell {
    int x = 0;
    int y = 0;

    bool operator==(const Cell& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
    bool operator<(const Cell& o) const {
        if (y != o.y) return y < o.y;
        return x < o.x;
    }
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace tmm
