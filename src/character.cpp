#include "knock/character.hpp"

#include <algorithm>

namespace knock {
namespace {

enum : std::uint8_t { clear = 0, body = 1, shade = 2, dark = 3, light = 4, accent = 5, accent_shade = 6, cheek = 7 };

// Hand-picked pairs of a main colour and its darker side. The first entry is
// the orange of the original Knock artwork.
constexpr std::uint32_t bodies[][2] = {
    {0xE8784F, 0xB85A38}, {0xE0655F, 0xA9474A}, {0x4FB3A6, 0x37847A}, {0x9C86D9, 0x7461A8},
    {0x8FAF4E, 0x6A8636}, {0x5E9FE0, 0x4377AE}, {0xD9B86C, 0xA88A4A}, {0xD97BA3, 0xA75879},
    {0x8C9AA8, 0x66717D}, {0xE6B83C, 0xB38B24},
};
constexpr std::uint32_t accents[][2] = {
    {0x3A3A3A, 0x242424}, {0x77736A, 0x55524B}, {0xE8E2D0, 0xBDB6A2}, {0x4A6FD6, 0x324C99},
    {0xC94C4C, 0x8F3434}, {0x5BBF6A, 0x3F8A4A}, {0x7C5BC4, 0x573F8C},
};

// SplitMix64: a tiny, well-mixed, fully specified generator.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int below(int count) { return static_cast<int>(next() % static_cast<std::uint64_t>(count)); }
    int between(int low, int high) { return low + below(high - low + 1); }
    bool chance(int percent) { return below(100) < percent; }
private:
    std::uint64_t state_;
};

class Canvas {
public:
    explicit Canvas(Character& character) : c_(character) {}
    void set(int x, int y, std::uint8_t value) {
        if (x >= 0 && y >= 0 && x < Character::width && y < Character::height)
            c_.pixels[static_cast<std::size_t>(y * Character::width + x)] = value;
    }
    std::uint8_t get(int x, int y) const {
        if (x < 0 || y < 0 || x >= Character::width || y >= Character::height) return clear;
        return c_.at(x, y);
    }
    // Draws a pixel and its mirror across the vertical centre line.
    void pair(int x, int y, std::uint8_t value) { set(x, y, value); set(Character::width - 1 - x, y, value); }
    void box(int left, int top, int right, int bottom, std::uint8_t value) {
        for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) set(x, y, value);
    }
    void pair_box(int left, int top, int right, int bottom, std::uint8_t value) {
        for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) pair(x, y, value);
    }
    // Recolours only pixels that already belong to the body.
    void tint(int x, int y, std::uint8_t value) {
        const auto current = get(x, y);
        if (current == body || current == shade) set(x, y, value);
    }
private:
    Character& c_;
};

struct Shape { int left, top, right, bottom; };

Shape draw_body(Canvas& canvas, Random& random, int kind) {
    constexpr int center = Character::width / 2;
    const int floor = Character::height - 1;
    int half = 0, height = 0, leg = 2;
    switch (kind) {
    case 0: half = random.between(5, 7); height = random.between(7, 9); break;    // blob
    case 1: half = random.between(7, 9); height = random.between(6, 8); leg = 3; break; // walker
    case 2: half = random.between(4, 5); height = random.between(9, 11); break;   // tall
    case 3: half = random.between(5, 7); height = random.between(8, 10); leg = 1; break; // bean
    case 4: half = random.between(7, 9); height = random.between(5, 7); leg = 3; break; // tank
    default: half = random.between(5, 6); height = random.between(8, 10); leg = 0; break; // ghost
    }
    Shape shape{center - half, floor - leg - height + 1, center + half - 1, floor - leg};
    canvas.box(shape.left, shape.top, shape.right, shape.bottom, body);
    // The lower rows are the shaded side of the body.
    canvas.box(shape.left, shape.bottom - (height > 7 ? 1 : 0), shape.right, shape.bottom, shade);
    auto corner = [&](int depth) {
        for (int step = 0; step < depth; ++step) {
            for (int x = 0; x < depth - step; ++x) {
                canvas.pair(shape.left + x, shape.top + step, clear);
                if (kind == 3) canvas.pair(shape.left + x, shape.bottom - step, clear);
            }
        }
    };
    switch (kind) {
    case 0:
        corner(2);
        canvas.pair(shape.left, shape.bottom, clear);
        for (int y = shape.bottom + 1; y <= floor; ++y)
            canvas.pair_box(center - half + 2, y, center - half + 3, y, shade);
        break;
    case 1: {
        corner(1);
        for (int y = shape.bottom + 1; y <= floor; ++y) {
            canvas.pair(shape.left + 1, y, shade);
            canvas.pair(shape.left + 4, y, shade);
        }
        // A short tail on one side keeps walkers from looking like tables.
        canvas.set(shape.right + 1, shape.top + 2, body);
        canvas.set(shape.right + 2, shape.top + 1, body);
        break;
    }
    case 2:
        corner(1);
        for (int y = shape.top + height / 2; y <= shape.top + height / 2 + 3; ++y) canvas.pair(shape.left - 1, y, shade);
        canvas.pair(shape.left - 2, shape.top + height / 2 + 3, shade);
        for (int y = shape.bottom + 1; y <= floor; ++y) canvas.pair_box(shape.left + 1, y, shape.left + 2, y, shade);
        break;
    case 3:
        corner(3);
        canvas.pair(shape.left + 2, floor, shade);
        canvas.pair(shape.left + 3, floor, shade);
        break;
    case 4:
        corner(2);
        canvas.box(shape.left - 1, shape.bottom + 1, shape.right + 1, floor, dark);
        for (int x = shape.left; x <= shape.right; x += 2) canvas.set(x, shape.bottom + 2, accent_shade);
        break;
    default:
        corner(3);
        for (int x = shape.left; x <= shape.right; ++x)
            if ((x - shape.left) % 3 == 1) canvas.set(x, shape.bottom, clear);
        break;
    }
    return shape;
}

void draw_pattern(Canvas& canvas, Random& random, const Shape& shape, int kind) {
    const int width = shape.right - shape.left + 1;
    switch (kind) {
    case 1: { // mirrored spots
        const int spots = random.between(2, 4);
        for (int i = 0; i < spots; ++i) {
            const int x = shape.left + 1 + random.below(std::max(1, width / 2 - 1));
            const int y = shape.top + 2 + random.below(std::max(1, shape.bottom - shape.top - 3));
            canvas.tint(x, y, shade);
            canvas.tint(Character::width - 1 - x, y, shade);
        }
        break;
    }
    case 2: // belly
        for (int y = shape.top + (shape.bottom - shape.top) / 2 + 1; y < shape.bottom; ++y)
            for (int x = shape.left + width / 4 + 1; x <= shape.right - width / 4 - 1; ++x) canvas.tint(x, y, light);
        break;
    case 3: // stripes
        for (int y = shape.top + 2; y < shape.bottom - 1; y += 3)
            for (int x = shape.left; x <= shape.right; ++x) canvas.tint(x, y, shade);
        break;
    case 4: // scarf
        for (int x = shape.left; x <= shape.right; ++x) {
            if (canvas.get(x, shape.top + (shape.bottom - shape.top) / 2 + 1) != clear)
                canvas.set(x, shape.top + (shape.bottom - shape.top) / 2 + 1, accent);
        }
        canvas.set(shape.left + 2, shape.top + (shape.bottom - shape.top) / 2 + 2, accent);
        canvas.set(shape.left + 2, shape.top + (shape.bottom - shape.top) / 2 + 3, accent_shade);
        break;
    default: break;
    }
}

int draw_eyes(Canvas& canvas, Random& random, const Shape& shape, int kind) {
    const int width = shape.right - shape.left + 1;
    const int y = shape.top + std::max(2, (shape.bottom - shape.top) / 3);
    const int x = shape.left + std::max(1, width / 4 - 1);
    const int look = random.below(3) - 1; // pupils look left, ahead or right
    switch (kind) {
    case 0: canvas.pair_box(x, y, x + 1, y + 1, dark); break;
    case 1:
        canvas.pair_box(x - 1, y - 1, x + 1, y + 1, light);
        canvas.set(x + look, y, dark);
        canvas.set(Character::width - 1 - x + look, y, dark);
        canvas.set(x + look, y + 1, dark);
        canvas.set(Character::width - 1 - x + look, y + 1, dark);
        break;
    case 2: canvas.pair_box(x - 1, y + 1, x + 1, y + 1, dark); break; // sleepy
    case 3: // visor
        canvas.box(shape.left + 1, y, shape.right - 1, y + 1, dark);
        canvas.pair(x, y, light);
        break;
    case 4: { // one eye
        constexpr int center = Character::width / 2;
        canvas.box(center - 2, y - 1, center + 1, y + 1, light);
        canvas.box(center - 1 + look, y, center + look, y + 1, dark);
        break;
    }
    default: // stern
        canvas.pair_box(x - 1, y, x + 1, y, dark);
        canvas.pair(x, y + 1, dark);
        canvas.pair(x - 1, y - 1, dark);
        break;
    }
    return y;
}

void draw_mouth(Canvas& canvas, const Shape& shape, int eyes_y, int kind, bool cheeks) {
    constexpr int center = Character::width / 2;
    const int y = std::min(shape.bottom - 2, eyes_y + 3);
    if (cheeks) {
        const int width = shape.right - shape.left + 1;
        canvas.pair(shape.left + std::max(1, width / 4 - 2), y - 1, cheek);
    }
    switch (kind) {
    case 1: canvas.pair(center - 1, y, dark); break;
    case 2:
        canvas.pair_box(center - 2, y, center - 1, y, dark);
        canvas.pair(center - 3, y - 1, dark);
        break;
    case 3: // teeth
        canvas.pair_box(center - 2, y, center - 1, y + 1, dark);
        canvas.pair(center - 1, y, light);
        break;
    case 4: // open
        canvas.pair_box(center - 1, y, center - 1, y + 1, dark);
        canvas.pair(center - 1, y + 1, cheek);
        break;
    default: break;
    }
}

void draw_accessory(Canvas& canvas, Random& random, const Shape& shape, int eyes_y, int kind) {
    constexpr int center = Character::width / 2;
    switch (kind) {
    case 1: // headphones
        for (int x = shape.left + 1; x <= shape.right - 1; ++x) canvas.set(x, shape.top - 2, dark);
        canvas.pair(shape.left, shape.top - 1, dark);
        canvas.pair_box(shape.left - 1, eyes_y - 1, shape.left, eyes_y + 2, accent);
        canvas.pair(shape.left - 1, eyes_y + 2, accent_shade);
        break;
    case 2: // antenna
        canvas.box(center, shape.top - 3, center, shape.top - 1, dark);
        canvas.box(center - 1, shape.top - 5, center + 1, shape.top - 4, accent);
        break;
    case 3: // two antennae
        canvas.pair(center - 3, shape.top - 1, dark);
        canvas.pair(center - 4, shape.top - 2, dark);
        canvas.pair(center - 5, shape.top - 3, accent);
        break;
    case 4: { // cap with a brim to one side
        canvas.box(shape.left + 1, shape.top - 2, shape.right - 1, shape.top, accent);
        canvas.box(shape.left + 1, shape.top, shape.right - 1, shape.top, accent_shade);
        const bool right = random.chance(50);
        canvas.box(right ? shape.right : shape.left - 3, shape.top, right ? shape.right + 3 : shape.left, shape.top, accent_shade);
        break;
    }
    case 5: // horns
        canvas.pair(shape.left + 1, shape.top - 1, light);
        canvas.pair(shape.left + 1, shape.top - 2, light);
        canvas.pair(shape.left, shape.top - 3, light);
        break;
    case 6: // sprout
        canvas.box(center, shape.top - 3, center, shape.top - 1, accent_shade);
        canvas.box(center - 2, shape.top - 4, center - 1, shape.top - 3, accent);
        canvas.box(center + 1, shape.top - 5, center + 2, shape.top - 4, accent);
        break;
    case 7: // helmet with a viewing slot
        canvas.box(shape.left - 1, shape.top - 2, shape.right + 1, eyes_y - 1, accent);
        canvas.box(shape.left - 1, eyes_y + 2, shape.right + 1, eyes_y + 2, accent_shade);
        canvas.box(shape.left - 1, eyes_y - 1, shape.left, eyes_y + 2, accent_shade);
        canvas.box(shape.right, eyes_y - 1, shape.right + 1, eyes_y + 2, accent_shade);
        canvas.box(shape.left + 2, shape.top - 1, shape.left + 3, eyes_y - 2, accent_shade);
        break;
    case 8: { // flower at one side of the head
        const bool right = random.chance(50);
        const int x = right ? shape.right : shape.left;
        const int y = shape.top;
        canvas.set(x, y - 2, accent); canvas.set(x - 1, y - 1, accent);
        canvas.set(x + 1, y - 1, accent); canvas.set(x, y, accent);
        canvas.set(x, y - 1, light);
        canvas.set(x + (right ? -1 : 1), y + 1, accent_shade);
        break;
    }
    case 9: // bow
        canvas.box(center - 3, shape.top - 2, center - 2, shape.top - 1, accent);
        canvas.box(center + 1, shape.top - 2, center + 2, shape.top - 1, accent);
        canvas.box(center - 1, shape.top - 2, center, shape.top - 1, accent_shade);
        break;
    default: break;
    }
}
}

std::uint64_t character_seed(std::string_view name) {
    std::uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a
    for (unsigned char c : name) {
        hash ^= c;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

Character make_character(std::string_view name) {
    Character character;
    Random random(character_seed(name));
    character.colors = random.below(static_cast<int>(std::size(bodies)));
    int accent_index = random.below(static_cast<int>(std::size(accents)));
    character.palette = {0, bodies[character.colors][0], bodies[character.colors][1], 0x0B0B0B, 0xF2F0E6,
                         accents[accent_index][0], accents[accent_index][1], 0xF0A3A3};
    character.body = random.below(6);
    character.eyes = random.below(6);
    character.mouth = random.below(5);
    character.accessory = random.below(10);
    character.pattern = random.below(5);
    // A helmet covers the eyes' row edges; a visor already reads as a helmet.
    if (character.accessory == 7 && character.eyes == 3) character.eyes = 0;
    Canvas canvas(character);
    const auto shape = draw_body(canvas, random, character.body);
    draw_pattern(canvas, random, shape, character.pattern);
    draw_accessory(canvas, random, shape, shape.top + std::max(2, (shape.bottom - shape.top) / 3), character.accessory);
    const int eyes_y = draw_eyes(canvas, random, shape, character.eyes);
    draw_mouth(canvas, shape, eyes_y, character.mouth, random.chance(35));
    return character;
}

}
