#pragma once

#include <filesystem>
#include <string>

namespace knock {

// The interface is a wide bar along the top of the screen.
struct WindowSize {
    int width = 1720;
    int height = 380;
    bool operator==(const WindowSize&) const = default;
};

bool valid_window_size(WindowSize size, std::wstring* reason = nullptr);

// Sizes are in logical pixels. Call from the UI thread, like HotkeyManager;
// each save reads the latest shared settings before replacing the file.
class WindowSettings {
public:
    explicit WindowSettings(std::filesystem::path data_directory);

    bool initialize();
    bool apply(WindowSize size);
    bool set_notes_collapsed(bool collapsed);
    const WindowSize& size() const noexcept { return size_; }
    bool notes_collapsed() const noexcept { return notes_collapsed_; }
    const std::wstring& error() const noexcept { return error_; }

private:
    std::filesystem::path directory_;
    WindowSize size_;
    bool notes_collapsed_ = false;
    std::wstring error_;
};

}
