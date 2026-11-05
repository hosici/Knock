#include "knock/window_settings.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Could not read test settings");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    output.close();
    require(static_cast<bool>(output), "Could not write test settings");
}

struct Fixture {
    std::filesystem::path directory;

    Fixture() {
        static unsigned int sequence = 0;
#ifdef _WIN32
        const auto process = GetCurrentProcessId();
#else
        const auto process = ::getpid();
#endif
        directory = std::filesystem::temp_directory_path() / ("knock-window-settings-test-" +
            std::to_string(process) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++sequence));
        require(std::filesystem::create_directory(directory), "Could not create isolated test directory");
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::filesystem::path settings() const { return directory / "settings.json"; }
    void no_temporary_files() const {
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            require(!entry.path().filename().string().starts_with(".window-settings-"), "A temporary settings file was left behind");
    }
};

void defaults_and_validation() {
    Fixture fixture;
    knock::WindowSettings settings(fixture.directory);
    require(settings.initialize(), "Missing settings should use defaults");
    require(settings.size() == knock::WindowSize{1720, 380}, "Default size changed");
    require(!std::filesystem::exists(fixture.settings()), "Reading defaults created a settings file");
    std::wstring reason;
    require(knock::valid_window_size({900, 300}, &reason) && reason.empty(), "Minimum dimensions rejected");
    require(knock::valid_window_size({3840, 800}), "Maximum dimensions rejected");
    for (const auto size : {knock::WindowSize{899, 380}, {3841, 380}, {1720, 299}, {1720, 801},
                           {std::numeric_limits<int>::min(), 380}, {1720, std::numeric_limits<int>::max()}}) {
        require(!settings.apply(size) && !settings.error().empty(), "Out-of-range dimensions accepted");
        require(settings.size() == knock::WindowSize{}, "Invalid dimensions changed current size");
        require(!std::filesystem::exists(fixture.settings()), "Invalid dimensions wrote a settings file");
    }
    require(settings.apply({900, 800}) && settings.error().empty(), "Valid dimensions did not clear the error");
    fixture.no_temporary_files();
    std::cout << "PASS defaults, boundaries, and invalid-size rollback\n";
}

void shared_preferences() {
    Fixture fixture;
    const Json original{{"shortcut", {{"enabled", true}, {"modifiers", 3}, {"key", 75}}},
        {"window", {{"width", 480}, {"height", 650}, {"bar_width", 1480}, {"bar_height", 420},
                    {"future_preference", "keep this"}}},
        {"unrelated", {{"text", "personal notes / 日本語"}, {"enabled", true}}}};
    write(fixture.settings(), original.dump());
    knock::WindowSettings settings(fixture.directory);
    require(settings.initialize() && settings.size() == knock::WindowSize{1480, 420}, "Stored size did not load");

    // Simulate the shortcut dialog saving after this object was initialized.
    // Applying a size must merge from disk, rather than a stale cached object.
    auto latest = original;
    latest["shortcut"] = {{"enabled", true}, {"modifiers", 6}, {"key", 74}};
    latest["new_preference"] = 17;
    write(fixture.settings(), latest.dump());
    require(settings.apply({1620, 500}), "Changed size did not save");
    const auto saved = Json::parse(read(fixture.settings()));
    latest["window"]["bar_width"] = 1620;
    latest["window"]["bar_height"] = 500;
    require(saved == latest, "Saving window size lost another preference");
    knock::WindowSettings restarted(fixture.directory);
    require(restarted.initialize() && restarted.size() == settings.size(), "Size did not survive restart");
    fixture.no_temporary_files();
    std::cout << "PASS restart and latest shortcut/unrelated-preference preservation\n";
}

void invalid_saved_dimensions() {
    Fixture fixture;
    std::vector<Json> invalid{
        nullptr, Json::array(), "not an object",
        Json{{"bar_width", 1720}}, Json{{"bar_width", 1720}, {"bar_height", 380.0}},
        Json{{"bar_width", "1720"}, {"bar_height", 380}}, Json{{"bar_width", true}, {"bar_height", 380}},
        Json{{"bar_width", -1}, {"bar_height", 380}}, Json{{"bar_width", 900}, {"bar_height", 801}},
        Json{{"bar_width", std::numeric_limits<std::uint64_t>::max()}, {"bar_height", 380}}
    };
    for (const auto& window : invalid) {
        const auto bytes = Json{{"window", window}, {"untouched", "keep"}}.dump();
        write(fixture.settings(), bytes);
        knock::WindowSettings settings(fixture.directory);
        require(!settings.initialize() && !settings.error().empty(), "Invalid stored dimensions were accepted");
        require(settings.size() == knock::WindowSize{}, "Invalid stored dimensions changed defaults");
        require(read(fixture.settings()) == bytes, "Loading invalid dimensions changed the file");
        require(settings.apply({1500, 600}), "Explicit Apply could not repair invalid dimensions");
        const auto saved = Json::parse(read(fixture.settings()));
        require(saved["window"]["bar_width"] == 1500 && saved["window"]["bar_height"] == 600 &&
                saved["untouched"] == "keep", "Repair lost unrelated preferences");
    }
    fixture.no_temporary_files();
    std::cout << "PASS invalid stored values stay intact until an explicit Apply\n";
}

void corrupt_settings() {
    Fixture fixture;
    const std::vector<std::string> corrupt{"{unfinished", "[]", "null", "17", std::string(65537, ' ')};
    for (const auto& bytes : corrupt) {
        write(fixture.settings(), bytes);
        knock::WindowSettings settings(fixture.directory);
        require(!settings.initialize(), "Corrupt settings were accepted");
        require(!settings.apply({1500, 650}), "Applying dimensions overwrote corrupt settings");
        require(settings.size() == knock::WindowSize{} && !settings.error().empty(), "Failed save changed current settings");
        require(read(fixture.settings()) == bytes, "Corrupt settings were not preserved exactly");
    }
    // This file fits while compact, but adding a window entry and pretty
    // printing it would exceed the shared settings file budget.
    const auto near_limit = Json{{"untouched", std::string(65490, 'x')}}.dump();
    require(near_limit.size() <= 65536, "Oversized test fixture");
    write(fixture.settings(), near_limit);
    knock::WindowSettings settings(fixture.directory);
    require(settings.initialize() && !settings.apply({1500, 650}), "Save exceeded the shared settings limit");
    require(read(fixture.settings()) == near_limit, "An oversized save changed existing settings");
    fixture.no_temporary_files();
    std::cout << "PASS corrupt and oversized files remain unchanged\n";
}

void failed_save() {
    Fixture fixture;
    knock::WindowSettings settings(fixture.directory);
    require(settings.apply({1480, 610}), "Initial saved size failed");
    const auto original = read(fixture.settings());
    const auto preserved = fixture.directory / "settings-preserved.json";
    std::filesystem::rename(fixture.settings(), preserved);
    std::filesystem::create_directory(fixture.settings());
    require(!settings.apply({1600, 750}) && settings.size() == knock::WindowSize{1480, 610}, "Failed save changed the active size");
    require(!settings.initialize() && settings.size() == knock::WindowSize{1480, 610}, "Failed reload changed the active size");
    require(read(preserved) == original && std::filesystem::is_directory(fixture.settings()), "Failed save replaced existing content");
    fixture.no_temporary_files();
    std::filesystem::remove(fixture.settings());
    std::filesystem::rename(preserved, fixture.settings());
    require(settings.apply({1600, 750}) && settings.error().empty(), "Save did not recover after the obstruction was removed");
    std::cout << "PASS failed-save/reload rollback and recovery\n";
}

void legacy_popup_size_and_notes_panel() {
    Fixture fixture;
    const Json legacy{{"window", {{"width", 420}, {"height", 510}}}, {"shortcut", {{"enabled", true}}}};
    write(fixture.settings(), legacy.dump());
    knock::WindowSettings settings(fixture.directory);
    require(settings.initialize() && settings.size() == knock::WindowSize{} && !settings.notes_collapsed(),
            "A saved popup size from an older version was not ignored");
    require(read(fixture.settings()) == legacy.dump(), "Loading legacy settings rewrote them");
    require(settings.set_notes_collapsed(true) && settings.notes_collapsed(), "Collapsing notes did not save");
    auto saved = Json::parse(read(fixture.settings()));
    require(saved["window"]["width"] == 420 && saved["window"]["height"] == 510 && saved["window"]["notes_collapsed"] == true &&
            saved["shortcut"] == legacy["shortcut"], "Notes setting lost other preferences");
    knock::WindowSettings restarted(fixture.directory);
    require(restarted.initialize() && restarted.notes_collapsed(), "Collapsed notes did not survive restart");
    require(restarted.apply({1600, 400}) && restarted.notes_collapsed(), "Applying a size reset the notes panel");
    fixture.no_temporary_files();
    std::cout << "PASS old popup sizes are kept but ignored; the notes panel state persists\n";
}

#ifdef _WIN32
void locked_destination() {
    Fixture fixture;
    knock::WindowSettings settings(fixture.directory);
    require(settings.apply({1470, 620}), "Lock fixture initialization failed");
    const auto bytes = read(fixture.settings());
    struct LockedFile {
        HANDLE handle;
        ~LockedFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } lock{CreateFileW(fixture.settings().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(lock.handle != INVALID_HANDLE_VALUE, "Could not lock the settings destination");
    require(!settings.apply({1510, 710}), "Replacement ignored the destination's sharing mode");
    require(settings.size() == knock::WindowSize{1470, 620}, "Failed replacement changed active dimensions");
    require(read(fixture.settings()) == bytes, "Failed replacement changed the original file");
    fixture.no_temporary_files();
    CloseHandle(lock.handle);
    lock.handle = INVALID_HANDLE_VALUE;
    require(settings.apply({1510, 710}), "Replacement did not recover after unlocking");
    std::cout << "PASS Windows replacement failure preserves bytes and removes its temporary file\n";
}
#endif
}

int main() {
    try {
        defaults_and_validation();
        shared_preferences();
        invalid_saved_dimensions();
        corrupt_settings();
        failed_save();
        legacy_popup_size_and_notes_panel();
#ifdef _WIN32
        locked_destination();
#endif
        std::cout << "All window settings checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
