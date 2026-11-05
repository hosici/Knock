#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace knock {

// A small pixel-art creature generated from a job (chat) name. The same name
// always produces the same creature; the generator uses only fixed integer
// arithmetic so the result does not depend on the compiler or platform.
struct Character {
    static constexpr int width = 24;
    static constexpr int height = 20;
    // 0 is transparent; other values index palette (0xRRGGBB).
    std::array<std::uint8_t, width * height> pixels{};
    std::array<std::uint32_t, 8> palette{};
    // Readable description of the chosen parts, for tests and debugging.
    int body = 0, eyes = 0, mouth = 0, accessory = 0, pattern = 0, colors = 0;

    std::uint8_t at(int x, int y) const { return pixels[static_cast<std::size_t>(y * width + x)]; }
};

std::uint64_t character_seed(std::string_view name);
Character make_character(std::string_view name);

}
