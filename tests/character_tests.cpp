#include "knock/character.hpp"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string fingerprint(const knock::Character& character) {
    std::string result(character.pixels.begin(), character.pixels.end());
    for (auto color : character.palette) result += std::to_string(color) + ",";
    return result;
}
}

int main() {
    try {
        // FNV-1a is fixed, so a job keeps its character across builds and machines.
        check(knock::character_seed("") == 0xCBF29CE484222325ull, "seed changed for the empty name");
        check(knock::character_seed("a") == 0xAF63DC4C8601EC8Cull, "seed changed for a known name");
        const auto first = knock::make_character("EDIT-DESIGN-2");
        check(fingerprint(first) == fingerprint(knock::make_character("EDIT-DESIGN-2")), "same name, different character");
        std::cout << "PASS the same job always gets the same character\n";

        std::set<std::string> seen;
        std::set<std::string> silhouettes;
        for (int i = 0; i < 500; ++i) {
            const auto name = "JOB-" + std::to_string(i);
            const auto character = knock::make_character(name);
            int filled = 0;
            for (auto value : character.pixels) {
                check(value < character.palette.size(), "pixel outside the palette");
                filled += value != 0;
            }
            check(filled >= 40, name + " is nearly empty");
            check(seen.insert(fingerprint(character)).second, name + " duplicates another character");
            silhouettes.insert(std::to_string(character.body) + "/" + std::to_string(character.eyes) + "/" +
                               std::to_string(character.accessory) + "/" + std::to_string(character.colors));
        }
        check(silhouettes.size() > 350, "too few distinct part combinations: " + std::to_string(silhouettes.size()));
        std::cout << "PASS 500 job names give 500 different characters\n";

        // Neighbouring labels, as agents usually number them, must also differ visibly.
        int same_parts = 0;
        for (int i = 1; i < 100; ++i) {
            const auto a = knock::make_character("AUDIO-" + std::to_string(i));
            const auto b = knock::make_character("AUDIO-" + std::to_string(i + 1));
            same_parts += a.body == b.body && a.colors == b.colors && a.accessory == b.accessory;
        }
        check(same_parts < 5, "neighbouring names often share the same look");
        std::cout << "PASS neighbouring job names look different\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
