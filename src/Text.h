#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace bilibili {
struct TextStyle {
    unsigned width, height;
    float size;
    uint32_t color;
    bool bold, wrap, centerInk;
    bool operator==(const TextStyle& other) const {
        return width == other.width && height == other.height && size == other.size &&
            color == other.color && bold == other.bold && wrap == other.wrap && centerInk == other.centerInk;
    }
};
struct TextPixels {
    // BGRA with premultiplied alpha, as required by PixelFormat32bppPARGB.
    std::vector<uint8_t> bytes;
    int offsetY = 0;
};
TextPixels rasterizeText(std::wstring_view text, const TextStyle& style);
}
