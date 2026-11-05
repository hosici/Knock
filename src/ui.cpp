#include "knock/ui.hpp"
#include "knock/platform.hpp"
#include "knock/character.hpp"
#include "knock/pixel_font.hpp"
#include "knock/report.hpp"
#include "knock/hotkey.hpp"
#include "knock/note_images.hpp"
#include "knock/window_settings.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <set>
#include <vector>

namespace knock {
namespace {
constexpr wchar_t window_class[] = L"Knock.Tray.v1";
constexpr wchar_t content_class[] = L"Knock.Content.v1";
constexpr wchar_t panel_class[] = L"Knock.Panel.v1";
constexpr UINT tray_message = WM_APP + 1;
constexpr UINT change_message = WM_APP + 2;
constexpr UINT show_message = WM_APP + 20;
// The bar: a dark frame, lighter panels for the working area, and darker
// wells for rows, text boxes and buttons.
constexpr COLORREF frame = RGB(27, 27, 27);
constexpr COLORREF background = RGB(32, 32, 32);
constexpr COLORREF well = RGB(23, 23, 23);
constexpr COLORREF raised = RGB(47, 47, 47);
constexpr COLORREF hover_fill = RGB(40, 40, 40);
constexpr COLORREF divider = RGB(44, 44, 44);
constexpr COLORREF top_background = RGB(21, 21, 21);
constexpr COLORREF border = RGB(71, 71, 71);
constexpr COLORREF search_fill = RGB(64, 64, 64);
constexpr COLORREF blue = RGB(96, 136, 222);
constexpr COLORREF blue_fill = RGB(38, 50, 80);
constexpr COLORREF green = RGB(128, 178, 70);
constexpr COLORREF green_fill = RGB(35, 50, 25);
constexpr COLORREF orange_fill = RGB(72, 42, 30);
constexpr COLORREF foreground = RGB(225, 225, 217);
constexpr COLORREF muted = RGB(151, 151, 145);
constexpr COLORREF subdued = RGB(104, 104, 99);
constexpr COLORREF orange = RGB(224, 129, 88);

void fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
void outline(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}
std::wstring edit_text(HWND edit) {
    int length = GetWindowTextLengthW(edit);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}
std::wstring shortened(const std::string& utf8, std::size_t limit, bool keep_tail = false) {
    auto value = widen(utf8);
    if (value.size() <= limit) return value;
    if (keep_tail) {
        auto start = value.size() - limit;
        if (value[start] >= 0xDC00 && value[start] <= 0xDFFF) ++start;
        return L"..." + value.substr(start);
    }
    if (limit && value[limit - 1] >= 0xD800 && value[limit - 1] <= 0xDBFF) --limit;
    return value.substr(0, limit) + L"...";
}
std::string trim(std::string value) {
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}
std::string send_token() {
    GUID identifier{};
    if (FAILED(CoCreateGuid(&identifier))) throw std::runtime_error("Could not create a message ID");
    wchar_t encoded[40]{};
    if (!StringFromGUID2(identifier, encoded, 40)) throw std::runtime_error("Could not format the message ID");
    std::string result;
    for (wchar_t value : encoded) {
        if (value >= L'0' && value <= L'9') result += static_cast<char>(value);
        else if (value >= L'a' && value <= L'f') result += static_cast<char>(value);
        else if (value >= L'A' && value <= L'F') result += static_cast<char>(value - L'A' + 'a');
    }
    return result;
}
std::wstring display_time(std::int64_t milliseconds) {
    std::time_t time = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
    localtime_s(&local, &time);
    auto now = std::time(nullptr);
    std::tm today{};
    localtime_s(&today, &now);
    wchar_t result[64]{};
    std::wcsftime(result, 64, local.tm_year == today.tm_year && local.tm_yday == today.tm_yday
        ? L"%H:%M, TODAY" : L"%d %b, %H:%M", &local);
    return result;
}
std::string get_string(const Json& item, const char* key, const char* fallback = "") {
    auto it = item.find(key);
    return it != item.end() && it->is_string() ? it->get<std::string>() : fallback;
}
std::string tag_key(std::string value) {
    for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return value;
}
std::vector<std::string> tags_of(const Json& item) {
    auto found = item.find("tags");
    return found != item.end() && found->is_array() ? found->get<std::vector<std::string>>()
                                                  : std::vector<std::string>{};
}
std::wstring tag_caption(const std::vector<std::string>& tags) {
    std::wstring result;
    for (const auto& tag : tags) {
        if (!result.empty()) result += L"  ";
        result += L"#" + widen(tag);
    }
    return result;
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance), store_(data_directory()), server_(store_) {}
    ~App() {
        store_.set_changed({});
        server_.stop();
        quitting_ = true;
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        for (HFONT font : {body_font_, small_font_, title_font_, row_font_}) if (font) DeleteObject(font);
        if (edit_brush_) DeleteObject(edit_brush_);
        if (search_brush_) DeleteObject(search_brush_);
        if (icon_) DestroyIcon(icon_);
    }

    int run(bool show_on_start) {
        store_.load();
        const bool settings_loaded = window_settings_.initialize();
        recover_completions();
        snapshot_ = store_.snapshot();
        busy_was_active_ = busy_deadline() > 0;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpfnWndProc = root_proc;
        wc.lpszClassName = window_class;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Could not register the tray window.");
        wc.lpfnWndProc = content_proc;
        wc.lpszClassName = content_class;
        RegisterClassExW(&wc);
        wc.lpfnWndProc = panel_proc;
        wc.lpszClassName = panel_class;
        RegisterClassExW(&wc);
        edit_brush_ = CreateSolidBrush(well);
        search_brush_ = CreateSolidBrush(search_fill);
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, window_class, L"Knock",
            WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1720, 380, nullptr, nullptr, instance_, this);
        if (!window_) throw std::runtime_error("Could not create the tray window.");
        content_ = CreateWindowExW(WS_EX_CONTROLPARENT, content_class, L"Knock jobs", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, 10, 10, window_, nullptr, instance_, this);
        if (!content_) throw std::runtime_error("Could not create the inbox.");
        tasks_view_ = CreateWindowExW(0, panel_class, L"Task list", WS_CHILD | WS_VISIBLE,
            0, 0, 10, 10, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(61)), instance_, this);
        notes_view_ = CreateWindowExW(0, panel_class, L"Notes", WS_CHILD | WS_VISIBLE,
            0, 0, 10, 10, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(62)), instance_, this);
        if (!tasks_view_ || !notes_view_) throw std::runtime_error("Could not create the side panels.");
        dpi_ = GetDpiForWindow(window_);
        if (!dpi_) dpi_ = 96;
        create_fonts();
        task_entry_edit_ = make_edit(window_, 38, false, L"New task...");
        footer_edit_ = make_edit(window_, 30, false, L"search");
        presence_edit_ = make_edit(window_, 45, true, L"Out for coffee...");
        SendMessageW(presence_edit_, EM_SETLIMITTEXT, 500, 0);
        SetWindowTextW(presence_edit_, widen(get_string(presence(), "message")).c_str());
        icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
        if (!icon_) icon_ = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        add_tray();
        store_.set_changed([this](const Json& change) {
            auto owned = std::make_unique<Json>(change);
            if (PostMessageW(window_, change_message, 0, reinterpret_cast<LPARAM>(owned.get()))) owned.release();
        });
        server_.start();
        if (!settings_loaded) feedback(window_settings_.error());
        if (!hotkey_.initialize(window_)) feedback(hotkey_.error());
        SetTimer(window_, 1, 1000, nullptr);
        layout();
        if (show_on_start) show();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN && (message.hwnd == window_ || IsChild(window_, message.hwnd))) {
                if (message.wParam == VK_ESCAPE) { escape(); continue; }
                if (note_keys(message.hwnd, message.wParam)) continue;
                if (settings_keys(message.hwnd, message.wParam)) continue;
                if (message.wParam == VK_RETURN && message.hwnd == footer_edit_) { notes_enter(); continue; }
                if (message.wParam == VK_RETURN && message.hwnd == task_entry_edit_) { add_task(); continue; }
                if (message.wParam == VK_RETURN && message.hwnd == presence_edit_ && GetKeyState(VK_SHIFT) >= 0) {
                    save_presence_message(true); continue;
                }
                if (message.wParam == VK_RETURN && message.hwnd == tag_query_edit_) { activate_tag_query(); continue; }
                if (message.hwnd == composer_edit_ &&
                    ((message.wParam == 'V' && GetKeyState(VK_CONTROL) < 0) ||
                     (message.wParam == VK_INSERT && GetKeyState(VK_SHIFT) < 0))) {
                    SendMessageW(composer_edit_, WM_PASTE, 0, 0); continue;
                }
                if (message.wParam == VK_RETURN && message.hwnd == composer_edit_ && GetKeyState(VK_SHIFT) >= 0) {
                    send_note_message(); continue;
                }
                if (message.wParam == VK_RETURN && GetKeyState(VK_CONTROL) < 0) {
                    if (message.hwnd == reply_edit_) send_reply(); else save_detail();
                    continue;
                }
            }
            if (IsWindowVisible(window_) && IsDialogMessageW(window_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    enum class Detail { none, task, note };
    enum class SaveIntent { explicit_action, background };
    enum class SettingsPage { general, preferences, replies, reply };
    enum class NotePage { list, find, remove, trash, search };
    struct Button {
        HWND handle = nullptr;
        std::wstring label;
        std::function<void()> action;
        bool active = false;
        bool seen = false;
        bool hover = false;
        bool left_aligned = false;
        bool note_result = false;
        bool bare = false;
        bool arrow = false;
        bool close = false;
        Json record;
    };
    // A job card collects everything one chat label has sent to Knock.
    struct Card {
        std::string chat;
        std::string state; // waiting, working or done
        std::vector<Json> pending;
        Json job, done, answered;
        bool running = false;
        std::int64_t activity = 0;
    };
    struct CardGeometry { RECT tag, close, name, art, box, actions; };
    struct PanelRow { Json item; int top; int height; };
    struct Banner { std::string id; std::wstring title; std::wstring text; bool inbox = false; };
    struct NoteRow { std::size_t index; int top; int height; int text_height; };
    struct NoteDraft { std::string text; std::vector<std::string> images; std::string client_id; };
    struct NoteEdit { std::string message_id; std::int64_t revision = 0; NoteDraft draft; };
    struct MenuItem {
        UINT id;
        std::wstring label;
        bool enabled = true;
        bool separator = false;
    };

    HINSTANCE instance_;
    Store store_;
    PipeServer server_;
    HotkeyManager hotkey_{data_directory()};
    WindowSettings window_settings_{data_directory()};
    HWND window_ = nullptr;
    HWND content_ = nullptr;
    HICON icon_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HBRUSH search_brush_ = nullptr;
    HWND tasks_view_ = nullptr;
    HWND notes_view_ = nullptr;
    HWND presence_edit_ = nullptr;
    HWND task_entry_edit_ = nullptr;
    RECT tasks_rect_{}, middle_rect_{}, presence_rect_{}, notes_rect_{}, arrow_rect_{};
    RECT search_bounds_{}, presence_box_{}, task_entry_bounds_{};
    int full_width_ = 0;
    int lease_line_y_ = 0;
    int quiet_line_y_ = 0;
    int filter_line_y_ = 0;
    std::vector<PanelRow> task_rows_, note_list_rows_;
    int tasks_scroll_ = 0, notes_scroll_ = 0, tasks_extent_ = 0, notes_extent_ = 0;
    int hover_task_ = -1, hover_note_ = -1;
    std::vector<Card> cards_;
    int cards_scroll_ = 0, cards_extent_ = 0, card_width_ = 0, card_gap_ = 0;
    std::map<std::string, Character> characters_;
    bool presence_dirty_ = false;
    bool laying_out_ = false;
    bool relayout_ = false;
    HFONT body_font_ = nullptr;
    HFONT small_font_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT row_font_ = nullptr;
    UINT taskbar_created_ = 0;
    UINT dpi_ = 96;
    int button_id_ = 100;
    std::map<int, Button> buttons_;
    std::map<std::string, int> button_keys_;
    Json snapshot_;
    Detail detail_ = Detail::none;
    std::string detail_id_;
    std::string detail_original_title_;
    std::string detail_status_ = "todo";
    std::vector<std::string> detail_tags_;
    bool detail_dirty_ = false;
    bool populating_ = false;
    bool context_menu_ = false;
    bool quitting_ = false;
    bool keep_open_ = false;
    bool settings_open_ = false;
    HWND settings_return_focus_ = nullptr;
    HWND settings_width_edit_ = nullptr;
    HWND settings_height_edit_ = nullptr;
    HWND settings_retention_edit_ = nullptr;
    HWND settings_reply_edit_ = nullptr;
    SettingsPage settings_page_ = SettingsPage::general;
    int saved_reply_index_ = -1;
    int saved_reply_offset_ = 0;
    HWND footer_edit_ = nullptr;
    HWND title_edit_ = nullptr;
    HWND body_edit_ = nullptr;
    HWND operator_edit_ = nullptr;
    HWND project_edit_ = nullptr;
    HWND reply_edit_ = nullptr;
    HWND tag_query_edit_ = nullptr;
    HWND composer_edit_ = nullptr;
    HWND note_query_edit_ = nullptr;
    HWND note_find_edit_ = nullptr;
    NotePage note_page_ = NotePage::list;
    std::vector<Json> note_results_;
    bool note_results_dirty_ = true;
    bool note_search_pending_ = false;
    std::size_t note_search_page_offset_ = 0;
    std::size_t note_search_total_ = 0;
    std::optional<std::size_t> note_search_next_;
    std::string note_selected_id_;
    int note_result_offset_ = 0;
    int note_result_rows_ = 1;
    RECT note_results_bounds_{};
    bool dragging_note_results_ = false;
    std::vector<NotePage> note_command_choices_;
    std::size_t note_command_selection_ = 0;
    std::size_t note_command_first_ = 0;
    std::size_t note_command_visible_ = 0;
    RECT note_command_bounds_{};
    ULONGLONG note_delete_click_until_ = 0;
    std::string note_undo_id_;
    std::string note_undo_title_;
    ULONGLONG note_undo_until_ = 0;
    RECT note_undo_bounds_{};
    bool note_find_open_ = false;
    bool note_find_pending_ = false;
    std::vector<Json> note_find_results_;
    std::size_t note_find_selection_ = 0;
    std::string note_highlight_id_;
    ULONGLONG note_highlight_until_ = 0;
    bool note_metadata_open_ = false;
    bool note_draft_dirty_ = false;
    NoteDraft note_draft_;
    std::optional<NoteEdit> note_edit_;
    Json note_record_ = Json::object();
    Json note_messages_ = Json::array();
    std::size_t note_offset_ = 0;
    std::size_t note_message_count_ = 0;
    int note_scroll_ = 0;
    int note_history_height_ = 0;
    int note_layout_width_ = 0;
    UINT note_layout_dpi_ = 0;
    bool note_layout_dirty_ = true;
    RECT note_history_bounds_{};
    RECT note_composer_bounds_{};
    RECT note_attachment_bounds_{};
    std::vector<NoteRow> note_rows_;
    std::map<std::string, std::unique_ptr<NoteImage>> note_image_cache_;
    bool tag_picker_open_ = false;
    std::string note_filter_;
    bool untagged_filter_ = false;
    int note_body_y_ = 0;
    std::vector<std::string> tag_choices_;
    int tag_offset_ = 0;
    int tag_visible_rows_ = 1;
    RECT tag_panel_{};
    std::string reply_id_;
    std::string reply_draft_;
    std::map<std::string, std::string> drafts_;
    bool busy_was_active_ = false;
    bool busy_summary_pending_ = false;
    ULONGLONG busy_retry_after_ = 0;
    int content_height_ = 0;
    int viewport_height_ = 0;
    bool dragging_scroll_ = false;
    bool dragging_note_scroll_ = false;
    HWND dragging_editor_ = nullptr;
    int drag_start_y_ = 0;
    int drag_start_scroll_ = 0;
    int wheel_remainder_ = 0;
    std::deque<Banner> banners_;
    std::optional<Banner> active_banner_;
    ULONGLONG banner_started_ = 0;
    std::wstring feedback_;
    ULONGLONG feedback_until_ = 0;

    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    RECT rect(int x, int y, int width, int height) const { return RECT{x, y, x + width, y + height}; }
    bool dashboard() const {
        return !settings_open_ && detail_ == Detail::none && note_page_ == NotePage::list && !tag_picker_open_;
    }

    void create_fonts() {
        for (HFONT font : {body_font_, small_font_, title_font_}) if (font) DeleteObject(font);
        auto make = [this](int size, int weight) {
            return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");
        };
        body_font_ = make(12, FW_NORMAL);
        small_font_ = make(10, FW_NORMAL);
        title_font_ = make(13, FW_NORMAL);
        if (row_font_) DeleteObject(row_font_);
        row_font_ = make(13, FW_NORMAL);
        for (HWND edit : {footer_edit_, title_edit_, body_edit_, operator_edit_, project_edit_, reply_edit_, tag_query_edit_, composer_edit_,
                          settings_width_edit_, settings_height_edit_, settings_retention_edit_, settings_reply_edit_, presence_edit_,
                          task_entry_edit_, note_query_edit_, note_find_edit_})
            if (edit) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_), TRUE);
    }
    void text(HDC dc, const std::wstring& value, RECT bounds, HFONT font,
              COLORREF color = foreground, UINT flags = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX) const {
        auto old = SelectObject(dc, font);
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &bounds, flags);
        SelectObject(dc, old);
    }
    int text_height(const std::wstring& value, int width, HFONT font, int minimum = 0) const {
        HDC dc = GetDC(content_ ? content_ : window_);
        auto old = SelectObject(dc, font);
        RECT bounds{0, 0, std::max(width, px(20)), 0};
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &bounds, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(content_ ? content_ : window_, dc);
        return std::max(minimum, static_cast<int>(bounds.bottom));
    }
    void feedback(std::wstring message) {
        feedback_ = std::move(message);
        feedback_until_ = GetTickCount64() + 4500;
        InvalidateRect(window_, nullptr, FALSE);
        InvalidateRect(content_, nullptr, FALSE);
    }
    Json mutate(Json command, bool trusted = true) {
        auto result = store_.handle(command, trusted);
        if (!result.value("ok", false)) {
            auto error = get_string(result, "error", "The change could not be saved.");
            feedback(error == "input_busy" ? L"Input is already in use. Release it first." : widen(error));
        }
        return result;
    }
    const Json& preferences() const {
        static const Json empty = Json::object();
        const auto found = snapshot_.find("preferences");
        return found != snapshot_.end() && found->is_object() ? *found : empty;
    }
    const Json& presence() const {
        static const Json empty = Json::object();
        const auto found = snapshot_.find("presence");
        return found != snapshot_.end() && found->is_object() ? *found : empty;
    }
    std::wstring presence_caption() const {
        const auto state = get_string(presence(), "state");
        return state == "at_pc" ? L"AT PC" : state == "away" ? L"AWAY" : state == "busy" ? L"BUSY" : L"SET STATUS";
    }
    std::wstring presence_message() const {
        const auto message = get_string(presence(), "message");
        if (!message.empty()) return widen(message);
        const auto state = get_string(presence(), "state");
        return state == "at_pc" ? L"At the PC" : state == "away" ? L"Away from the PC" :
            state == "busy" ? L"Busy right now" : L"No status set";
    }
    std::int64_t busy_deadline() const { return preferences().value("busy_until", std::int64_t(0)); }
    bool busy() const { return busy_deadline() > now_ms(); }
    std::wstring busy_caption() const {
        const auto minutes = std::max<std::int64_t>(1, (busy_deadline() - now_ms() + 59999) / 60000);
        return L"QUIET · " + std::to_wstring(minutes) + L"m remaining";
    }
    bool save_preferences(Json changes) {
        changes["op"] = "preferences.update";
        const auto result = mutate(std::move(changes));
        if (!result.value("ok", false)) return false;
        snapshot_["preferences"] = result.at("preferences");
        return true;
    }
    void quiet_banners() {
        banners_.clear();
        active_banner_.reset();
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = 1;
        data.uFlags = NIF_INFO; // An empty szInfo removes an existing banner.
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    void busy_summary() {
        snapshot_ = store_.snapshot();
        std::size_t requests = 0, results = 0;
        for (const auto& request : snapshot_["requests"])
            if (!request.value("archived", false) && get_string(request, "status") == "pending") ++requests;
        for (const auto& job : snapshot_["jobs"])
            if (!job.value("archived", false) && get_string(job, "status") != "running") ++results;
        Banner summary;
        summary.inbox = true;
        summary.title = L"Knock - quiet timer finished";
        summary.text = std::to_wstring(requests) + L" pending request(s), " + std::to_wstring(results) +
            L" finished result(s) in the inbox.";
        banners_.push_back(std::move(summary));
        if (!active_banner_) next_banner();
    }
    void update_busy_mode() {
        const bool active = busy();
        if (active) {
            if (!busy_was_active_) quiet_banners();
            busy_summary_pending_ = false;
            busy_retry_after_ = 0;
        } else if (busy_was_active_) busy_summary_pending_ = true;
        if (active != busy_was_active_) {
            busy_was_active_ = active;
            layout();
        }
        if (busy_summary_pending_ && GetTickCount64() >= busy_retry_after_) {
            // A failed state write or snapshot must be retried, without
            // repeatedly announcing the same completion during this session.
            busy_retry_after_ = GetTickCount64() + 5000;
            try {
                if (!busy_deadline() || save_preferences({{"busy_until", 0}})) {
                    busy_summary();
                    busy_summary_pending_ = false;
                }
            } catch (const std::exception& error) { feedback(widen(error.what())); }
        }
        update_tray();
    }
    void set_busy_minutes(int minutes) {
        const bool previous = busy_was_active_;
        if (!save_preferences({{"busy_until", minutes ? now_ms() + std::int64_t(minutes) * 60000 : 0}})) return;
        busy_retry_after_ = 0;
        busy_was_active_ = previous;
        update_busy_mode();
        layout();
    }
    void busy_menu(POINT point) {
        const std::vector<MenuItem> items{{31, L"Quiet for 15 minutes"}, {32, L"Quiet for 30 minutes"},
            {33, L"Quiet for 1 hour"}, {34, L"End quiet mode", busy()}};
        const int selected = popup_menu(point, items);
        if (selected >= 31 && selected <= 34) {
            const int minutes[]{15, 30, 60, 0};
            set_busy_minutes(minutes[selected - 31]);
        }
    }
    void open_busy_menu() {
        POINT point{};
        GetCursorPos(&point);
        busy_menu(point);
    }


    void recover_completions() {
        auto directory = data_directory() / L"completions";
        if (!std::filesystem::exists(directory)) return;
        for (const auto& file : std::filesystem::directory_iterator(directory)) {
            if (!file.is_regular_file() || file.path().extension() != L".json" || file.file_size() > 1024 * 1024) continue;
            try {
                std::ifstream stream(file.path());
                auto command = Json::parse(stream);
                if (get_string(command, "op") != "job.finish") continue;
                auto result = store_.handle(command);
                if (result.value("ok", false)) std::filesystem::remove(file.path());
            } catch (const std::exception&) { /* Keep unreadable evidence for recovery. */ }
        }
    }

    void add_tray() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = 1;
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = tray_message;
        data.hIcon = icon_;
        wcscpy_s(data.szTip, L"Knock - agent inbox");
        Shell_NotifyIconW(NIM_ADD, &data);
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    void update_tray() {
        int pending = 0;
        for (const auto& request : snapshot_["requests"])
            if (get_string(request, "status") == "pending") ++pending;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = 1;
        data.uFlags = NIF_TIP;
        auto tip = pending ? L"Knock - " + std::to_wstring(pending) + L" pending request(s)" : L"Knock - agent inbox";
        if (get_string(presence(), "state", "unknown") != "unknown") tip += L" - " + presence_caption();
        if (busy()) tip += L" - " + busy_caption();
        wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    void next_banner() {
        active_banner_.reset();
        if (busy()) { banners_.clear(); return; }
        if (banners_.empty()) return;
        active_banner_ = std::move(banners_.front());
        banners_.pop_front();
        banner_started_ = GetTickCount64();
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = 1;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        wcsncpy_s(data.szInfoTitle, active_banner_->title.c_str(), _TRUNCATE);
        wcsncpy_s(data.szInfo, active_banner_->text.c_str(), _TRUNCATE);
        if (!Shell_NotifyIconW(NIM_MODIFY, &data)) active_banner_.reset();
    }
    void enqueue_banner(const Json& event) {
        if (busy()) return;
        Banner banner;
        banner.id = get_string(event, "id");
        banner.title = L"@" + widen(get_string(event, "chat")) + L" - " +
            (get_string(event, "type") == "request" ? L"permission request" :
             get_string(event, "type") == "done" ? L"finished" : L"command update");
        banner.text = widen(get_string(event, "message", get_string(event, "title").c_str()));
        if (banner.text.empty()) banner.text = L"Open Knock to view the update.";
        if (banners_.size() >= 32) banners_.pop_front();
        banners_.push_back(std::move(banner));
        if (!active_banner_) next_banner();
    }
    int column_width(int full, int percent, int low, int high) const {
        return std::clamp(full * percent / 100, px(low), px(high));
    }
    void position_popup() {
        // The bar sits centred along the top of the monitor with the tray or
        // pointer, inside the work area so it never covers a top taskbar.
        POINT cursor{};
        GetCursorPos(&cursor);
        NOTIFYICONIDENTIFIER identifier{};
        identifier.cbSize = sizeof(identifier);
        identifier.hWnd = window_;
        identifier.uID = 1;
        RECT tray{};
        bool has_tray = SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &tray));
        HMONITOR monitor = has_tray ? MonitorFromRect(&tray, MONITOR_DEFAULTTONEAREST)
                                    : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        const int work_width = static_cast<int>(info.rcWork.right - info.rcWork.left);
        const int work_height = static_cast<int>(info.rcWork.bottom - info.rcWork.top);
        full_width_ = std::min(px(window_settings_.size().width), work_width - px(16));
        int width = full_width_;
        if (window_settings_.notes_collapsed()) width -= column_width(full_width_, 17, 220, 320) + px(4);
        int height = std::min(px(window_settings_.size().height), work_height - px(16));
        int x = info.rcWork.left + (work_width - width) / 2;
        int y = info.rcWork.top + px(8);
        SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }
    void show(const std::string& focus_id = {}, bool pinned = false, bool inbox = false) {
        if (!IsWindowVisible(window_)) keep_open_ = pinned;
        else if (pinned) keep_open_ = true;
        recover_completions();
        bool title_needs_attention = false;
        if (!focus_id.empty() || inbox) {
            if (settings_open_) close_settings();
            if (!save_detail()) title_needs_attention = true;
            else {
                detail_ = Detail::none;
                destroy_detail_edits();
                reset_tag_picker();
                if (note_page_ != NotePage::list) close_note_page();
            }
        }
        position_popup();
        layout();
        if (!focus_id.empty() && !title_needs_attention) scroll_to_card(focus_id);
        ShowWindow(window_, SW_SHOWNORMAL);
        SetForegroundWindow(window_);
        SetFocus(title_needs_attention && title_edit_ ? title_edit_ : window_);
        InvalidateRect(window_, nullptr, FALSE);
        InvalidateRect(content_, nullptr, FALSE);
    }

    void hide() {
        if (!save_detail(SaveIntent::background)) return;
        if (reply_edit_) drafts_[reply_id_] = narrow(edit_text(reply_edit_));
        ShowWindow(window_, SW_HIDE);
        keep_open_ = false;
        save_presence_message(false);
    }
    bool startup_enabled() const {
        wchar_t value[32768]{};
        DWORD size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            L"Knock", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
    }
    void toggle_startup() {
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
            nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            feedback(L"Could not update startup setting."); return;
        }
        LSTATUS result;
        if (startup_enabled()) result = RegDeleteValueW(key, L"Knock");
        else {
            auto value = quote_argument(executable_path().wstring()) + L" --background";
            result = RegSetValueExW(key, L"Knock", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(key);
        if (result != ERROR_SUCCESS) feedback(L"Could not update startup setting.");
    }
    int popup_menu(POINT point, const std::vector<MenuItem>& items) {
        HMENU menu = CreatePopupMenu();
        if (!menu) return 0;
        MENUINFO appearance{};
        appearance.cbSize = sizeof(appearance);
        appearance.fMask = MIM_BACKGROUND;
        appearance.hbrBack = edit_brush_;
        SetMenuInfo(menu, &appearance);
        for (const auto& item : items) {
            MENUITEMINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STATE;
            info.fType = MFT_OWNERDRAW | (item.separator ? MFT_SEPARATOR : 0);
            info.fState = item.enabled && !item.separator ? MFS_ENABLED : MFS_DISABLED;
            info.wID = item.id;
            info.dwItemData = reinterpret_cast<ULONG_PTR>(&item);
            if (!item.separator) {
                info.fMask |= MIIM_STRING;
                info.dwTypeData = const_cast<wchar_t*>(item.label.c_str());
            }
            InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
        }
        const bool previous_menu = context_menu_;
        context_menu_ = true;
        SetForegroundWindow(window_);
        int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                      point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        context_menu_ = previous_menu;
        PostMessageW(window_, WM_NULL, 0, 0);
        return selected;
    }
    void measure_menu(MEASUREITEMSTRUCT& measure) {
        const auto* item = reinterpret_cast<const MenuItem*>(measure.itemData);
        if (!item) return;
        HDC dc = GetDC(window_);
        auto previous = SelectObject(dc, body_font_);
        SIZE size{};
        GetTextExtentPoint32W(dc, item->label.c_str(), static_cast<int>(item->label.size()), &size);
        SelectObject(dc, previous);
        ReleaseDC(window_, dc);
        measure.itemWidth = static_cast<UINT>(std::max(px(142), static_cast<int>(size.cx) + px(24)));
        measure.itemHeight = static_cast<UINT>(px(item->separator ? 7 : 26));
    }
    void draw_menu(const DRAWITEMSTRUCT& draw) {
        const auto* item = reinterpret_cast<const MenuItem*>(draw.itemData);
        if (!item) return;
        RECT bounds = draw.rcItem;
        fill(draw.hDC, bounds, draw.itemState & ODS_SELECTED ? RGB(35, 35, 35) : background);
        if (item->separator) {
            fill(draw.hDC, RECT{bounds.left + px(7), (bounds.top + bounds.bottom) / 2,
                 bounds.right - px(7), (bounds.top + bounds.bottom) / 2 + 1}, border);
            return;
        }
        bounds.left += px(10);
        bounds.right -= px(10);
        text(draw.hDC, item->label, bounds, body_font_, item->enabled ? foreground : subdued,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    void context_menu() {
        POINT point{};
        GetCursorPos(&point);
        const std::vector<MenuItem> items{
            {1, L"Open Knock"}, {2, L"Settings..."},
            {6, busy() ? busy_caption() : L"Quiet notifications..."},
            {3, startup_enabled() ? L"[x] Start with Windows" : L"[ ] Start with Windows"},
            {4, L"Open data folder"}, {7, L"Status..."}, {0, L"", false, true}, {5, L"Quit"}
        };
        int selected = popup_menu(point, items);
        if (selected == 1) show();
        if (selected == 2) { show(); open_settings(); }
        if (selected == 6) busy_menu(point);
        if (selected == 7) { show(); open_presence(); }
        if (selected == 3) { toggle_startup(); layout(); }
        if (selected == 4) ShellExecuteW(window_, L"open", data_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (selected == 5 && settings_open_) close_settings();
        if (selected == 5) {
            if (!save_detail()) {
                show();
                if (title_edit_) SetFocus(title_edit_);
                return;
            }
            quitting_ = true;
            DestroyWindow(window_);
            return;
        }
        // A newly opened popup needs to keep focus. Returning it to the tray
        // here would immediately deactivate and hide an ordinary tray open.
        if (IsWindowVisible(window_) || selected == 4) return;
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = 1;
        Shell_NotifyIconW(NIM_SETFOCUS, &data);
    }
    void open_settings() {
        if (settings_open_) return;
        if (!save_detail(SaveIntent::background)) return;
        settings_return_focus_ = GetFocus();
        settings_open_ = true;
        settings_page_ = SettingsPage::general;
        settings_width_edit_ = make_edit(content_, 41, false, L"Width");
        settings_height_edit_ = make_edit(content_, 42, false, L"Height");
        for (HWND edit : {settings_width_edit_, settings_height_edit_}) {
            SetWindowLongPtrW(edit, GWL_STYLE, GetWindowLongPtrW(edit, GWL_STYLE) | ES_NUMBER);
            SendMessageW(edit, EM_SETLIMITTEXT, 4, 0);
        }
        SetWindowTextW(settings_width_edit_, std::to_wstring(window_settings_.size().width).c_str());
        SetWindowTextW(settings_height_edit_, std::to_wstring(window_settings_.size().height).c_str());
        settings_retention_edit_ = make_edit(content_, 43, false, L"Days");
        SetWindowLongPtrW(settings_retention_edit_, GWL_STYLE,
            GetWindowLongPtrW(settings_retention_edit_, GWL_STYLE) | ES_NUMBER);
        SendMessageW(settings_retention_edit_, EM_SETLIMITTEXT, 3, 0);
        SetWindowTextW(settings_retention_edit_, std::to_wstring(preferences().value("trash_retention_days", 30)).c_str());
        feedback_.clear();
        layout();
        SetFocus(settings_width_edit_);
    }
    void close_settings() {
        if (!settings_open_) return;
        for (HWND* edit : {&settings_width_edit_, &settings_height_edit_, &settings_retention_edit_, &settings_reply_edit_}) {
            if (*edit) DestroyWindow(*edit);
            *edit = nullptr;
        }
        settings_open_ = false;
        settings_page_ = SettingsPage::general;
        feedback_.clear();
        layout();
        SetFocus(IsWindow(settings_return_focus_) && IsWindowVisible(settings_return_focus_)
            ? settings_return_focus_ : window_);
        settings_return_focus_ = nullptr;
    }
    void settings_page(SettingsPage page) {
        if (settings_reply_edit_ && page != SettingsPage::reply) {
            DestroyWindow(settings_reply_edit_);
            settings_reply_edit_ = nullptr;
        }
        settings_page_ = page;
        feedback_.clear();
        layout();
        SetFocus(page == SettingsPage::general ? settings_width_edit_ :
            page == SettingsPage::preferences ? settings_retention_edit_ :
            page == SettingsPage::reply ? settings_reply_edit_ : window_);
    }
    void settings_back() {
        if (settings_page_ == SettingsPage::reply) settings_page(SettingsPage::replies);
        else if (settings_page_ == SettingsPage::replies) settings_page(SettingsPage::preferences);
        else if (settings_page_ == SettingsPage::preferences) settings_page(SettingsPage::general);
        else close_settings();
    }
    void open_presence() {
        // Status lives in its own column now; this just puts the cursor there.
        if (!presence_edit_) return;
        SetFocus(presence_edit_);
        SendMessageW(presence_edit_, EM_SETSEL, 0, -1);
    }
    std::optional<std::string> presence_text() {
        std::string message;
        try { message = narrow(edit_text(presence_edit_)); }
        catch (const std::exception&) {
            feedback(L"Message contains invalid Unicode. Please edit it.");
            return std::nullopt;
        }
        if (message.size() > 500) {
            feedback(L"This message is too long (maximum 500 UTF-8 bytes).");
            return std::nullopt;
        }
        return message;
    }
    void set_presence(const std::string& state) {
        const auto message = presence_text();
        if (!message) return;
        KillTimer(window_, 7);
        const auto result = mutate({{"op", "presence.update"}, {"state", state}, {"message", *message}});
        if (!result.value("ok", false)) return;
        snapshot_["presence"] = result.at("presence");
        presence_dirty_ = false;
        update_tray();
        layout();
    }
    // Saves a changed message with the current status. Enter, leaving the box,
    // hiding Knock and a short pause after typing all save it.
    void save_presence_message(bool explicit_save) {
        KillTimer(window_, 7);
        if (!presence_dirty_ || !presence_edit_) return;
        const auto state = get_string(presence(), "state", "unknown");
        if (state != "at_pc" && state != "away" && state != "busy") {
            if (explicit_save) feedback(L"Choose At PC, Away or Busy to share this message.");
            return;
        }
        set_presence(state);
        if (explicit_save && !presence_dirty_) feedback(L"Status saved.");
    }
    std::vector<std::string> saved_replies() const {
        return preferences().value("saved_replies", std::vector<std::string>{});
    }
    static std::wstring reply_caption(const std::string& value, std::size_t limit = 64) {
        auto caption = shortened(value, limit);
        for (auto& ch : caption) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        return caption;
    }
    void edit_saved_reply(int index) {
        const auto replies = saved_replies();
        if (index >= static_cast<int>(replies.size()) || (index < 0 && replies.size() >= 12)) return;
        saved_reply_index_ = index;
        settings_page_ = SettingsPage::reply;
        if (!settings_reply_edit_) settings_reply_edit_ = make_edit(content_, 44, true, L"Write a reply you use often...");
        SendMessageW(settings_reply_edit_, EM_SETLIMITTEXT, 2000, 0);
        SetWindowTextW(settings_reply_edit_, index < 0 ? L"" : widen(replies[static_cast<std::size_t>(index)]).c_str());
        feedback_.clear();
        layout();
        SetFocus(settings_reply_edit_);
    }
    void save_saved_reply() {
        if (!settings_reply_edit_) return;
        const auto value = narrow(edit_text(settings_reply_edit_));
        if (trim(value).empty()) { feedback(L"Write a reply before saving."); return; }
        if (value.size() > 2000) { feedback(L"This reply is too long (maximum 2,000 UTF-8 bytes)."); return; }
        auto replies = saved_replies();
        if (saved_reply_index_ < 0) replies.push_back(value);
        else if (static_cast<std::size_t>(saved_reply_index_) < replies.size())
            replies[static_cast<std::size_t>(saved_reply_index_)] = value;
        else { feedback(L"That saved reply was removed. Go back and add it again."); return; }
        if (!save_preferences({{"saved_replies", replies}})) return;
        settings_page(SettingsPage::replies);
        feedback(L"Reply saved.");
    }
    void delete_saved_reply() {
        auto replies = saved_replies();
        if (saved_reply_index_ < 0 || static_cast<std::size_t>(saved_reply_index_) >= replies.size()) return;
        replies.erase(replies.begin() + saved_reply_index_);
        if (!save_preferences({{"saved_replies", replies}})) return;
        settings_page(SettingsPage::replies);
        feedback(L"Reply removed.");
    }
    void use_saved_reply() {
        if (!reply_edit_) return;
        const auto replies = saved_replies();
        std::vector<MenuItem> items;
        for (std::size_t i = 0; i < replies.size(); ++i)
            items.push_back({static_cast<UINT>(i + 1), reply_caption(replies[i])});
        if (items.empty()) items.push_back({0, L"No saved replies yet", false});
        items.push_back({0, L"", false, true});
        items.push_back({100, L"Manage saved replies..."});
        RECT bounds{};
        GetWindowRect(reply_edit_, &bounds);
        const int selected = popup_menu(POINT{bounds.left, bounds.top}, items);
        if (selected == 100) {
            open_settings();
            if (settings_open_) settings_page(SettingsPage::replies);
        } else if (selected > 0 && static_cast<std::size_t>(selected) <= replies.size() && IsWindow(reply_edit_)) {
            SetWindowTextW(reply_edit_, widen(replies[static_cast<std::size_t>(selected - 1)]).c_str());
            drafts_[reply_id_] = replies[static_cast<std::size_t>(selected - 1)];
            SetFocus(reply_edit_);
            SendMessageW(reply_edit_, EM_SETSEL, -1, -1);
        }
    }
    void apply_trash_retention() {
        const auto value = edit_text(settings_retention_edit_);
        if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) {
            feedback(L"Enter a retention period from 1 to 365 days."); return;
        }
        const int days = std::stoi(value);
        if (days < 1 || days > 365) { feedback(L"Keep deleted notes for 1 to 365 days."); return; }
        if (save_preferences({{"trash_retention_days", days}})) feedback(L"Trash retention saved.");
    }
    bool settings_keys(HWND target, WPARAM key) {
        if (!settings_open_ || key != VK_RETURN) return false;
        if (target == settings_width_edit_ || target == settings_height_edit_) apply_window_size();
        else if (target == settings_retention_edit_) apply_trash_retention();
        else if (target == settings_reply_edit_ && GetKeyState(VK_CONTROL) < 0) save_saved_reply();
        else return false;
        return true;
    }
    void configure_hotkey() {
        const bool previous_menu = context_menu_;
        context_menu_ = true;
        hotkey_.configure(window_);
        context_menu_ = previous_menu;
        if (!hotkey_.error().empty()) feedback(hotkey_.error());
        layout();
    }
    void apply_window_size() {
        if (!settings_open_) return;
        auto dimension = [](HWND edit) {
            auto value = edit_text(edit);
            if (value.empty() || value.size() > 4 ||
                value.find_first_not_of(L"0123456789") != std::wstring::npos) return 0;
            return std::stoi(value);
        };
        WindowSize size{dimension(settings_width_edit_), dimension(settings_height_edit_)};
        if (!window_settings_.apply(size)) { feedback(window_settings_.error()); return; }
        position_popup();
        layout();
        feedback(L"Window size saved.");
    }
    void reset_window_size() {
        SetWindowTextW(settings_width_edit_, L"1720");
        SetWindowTextW(settings_height_edit_, L"380");
        apply_window_size();
    }

    HWND make_edit(HWND parent, int id, bool multiline, const wchar_t* cue) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (multiline
            ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN : ES_AUTOHSCROLL);
        HWND edit = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_), TRUE);
        SendMessageW(edit, EM_SETLIMITTEXT, multiline ? 64000 : 160, 0);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(5), px(5)));
        if (cue) SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
        SetWindowSubclass(edit, edit_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        return edit;
    }
    void destroy_detail_edits() {
        reset_tag_picker();
        for (HWND* edit : {&title_edit_, &body_edit_, &operator_edit_, &project_edit_, &composer_edit_, &note_find_edit_}) {
            if (*edit) DestroyWindow(*edit);
            *edit = nullptr;
        }
        detail_dirty_ = false;
        note_metadata_open_ = false;
        note_edit_.reset();
        note_find_open_ = false;
        note_find_pending_ = false;
        note_find_results_.clear();
        note_highlight_id_.clear();
        note_rows_.clear();
        note_image_cache_.clear();
        KillTimer(window_, 4);
    }
    void begin_detail(Detail kind, const Json& item) {
        if (!save_detail()) return;
        detail_ = kind;
        detail_id_ = get_string(item, "id");
        detail_original_title_ = get_string(item, "title", "Untitled");
        detail_status_ = get_string(item, "status", "todo");
        detail_tags_ = tags_of(item);
        destroy_detail_edits();
        if (kind == Detail::note) {
            note_offset_ = item.value("message_count", std::size_t(0));
            note_offset_ = note_offset_ > 100 ? note_offset_ - 100 : 0;
            note_scroll_ = 0;
            note_history_height_ = 0;
            if (!load_note_history()) { detail_ = Detail::none; layout(); return; }
            load_note_draft();
            populating_ = true;
            composer_edit_ = make_edit(content_, 37, true, L"Write something, or paste an image...");
            SetWindowTextW(composer_edit_, widen(composer_draft().text).c_str());
            populating_ = false;
            layout();
            scroll_note_to_end();
            SetFocus(composer_edit_);
            return;
        }
        populating_ = true;
        title_edit_ = make_edit(content_, 31, false, L"Title");
        body_edit_ = make_edit(content_, 32, true, kind == Detail::note ? L"Write your note..." : L"Details...");
        SetWindowTextW(title_edit_, widen(get_string(item, "title")).c_str());
        SetWindowTextW(body_edit_, widen(get_string(item, "body")).c_str());
        if (kind == Detail::task) {
            operator_edit_ = make_edit(content_, 33, false, L"Operator");
            project_edit_ = make_edit(content_, 34, false, L"Project");
            SendMessageW(operator_edit_, EM_SETLIMITTEXT, 64, 0);
            SetWindowTextW(operator_edit_, widen(get_string(item, "operator", "ME")).c_str());
            SetWindowTextW(project_edit_, widen(get_string(item, "project")).c_str());
        }
        populating_ = false;
        detail_dirty_ = false;
        layout();
        SetFocus(body_edit_);
    }
    bool save_detail(SaveIntent intent = SaveIntent::explicit_action) {
        if (detail_ == Detail::note && !save_note_draft()) return false;
        if (detail_ == Detail::none || !detail_dirty_) return true;
        auto title = trim(narrow(edit_text(title_edit_)));
        if (title.empty()) {
            KillTimer(window_, 3);
            // A title can be temporarily empty while it is being replaced.
            // Keep the edits dirty so an incoming update cannot restore it.
            if (intent == SaveIntent::background) return true;
            feedback(L"Add a title before saving.");
            SetFocus(title_edit_);
            return false;
        }
        Json command{{"op", detail_ == Detail::note ? "note.update" : "task.update"},
            {"id", detail_id_}, {"title", title}, {"body", narrow(edit_text(body_edit_))}};
        if (detail_ == Detail::task) {
            command["operator"] = narrow(edit_text(operator_edit_));
            command["project"] = narrow(edit_text(project_edit_));
            command["status"] = detail_status_;
        }
        if (!mutate(command).value("ok", false)) return false;
        detail_original_title_ = title;
        detail_dirty_ = false;
        KillTimer(window_, 3);
        return true;
    }
    void leave_detail() {
        if (!save_detail()) return;
        detail_ = Detail::none;
        destroy_detail_edits();
        layout();
        SetFocus(note_page_visible() ? note_query_edit_ : window_);
    }
    void delete_detail() {
        if (detail_ == Detail::note) {
            if (!save_note_draft() || !trash_note(note_record_)) return;
            detail_dirty_ = false;
            detail_ = Detail::none;
            destroy_detail_edits();
            layout();
            return;
        }
        context_menu_ = true;
        int response = MessageBoxW(window_, L"Delete this task?", L"Knock", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        context_menu_ = false;
        if (response != IDYES) return;
        auto result = mutate({{"op", detail_ == Detail::note ? "note.delete" : "task.delete"}, {"id", detail_id_}});
        if (result.value("ok", false)) {
            detail_dirty_ = false;
            detail_ = Detail::none;
            destroy_detail_edits();
            layout();
        }
    }
    void open_task(const Json& item) {
        if (settings_open_) close_settings();
        if (settings_open_) return;
        reset_tag_picker();
        begin_detail(Detail::task, item);
    }
    void open_note(const Json& item) {
        if (settings_open_) close_settings();
        if (settings_open_) return;
        reset_tag_picker();
        begin_detail(Detail::note, item);
    }
    void add_task() {
        std::string title = trim(narrow(edit_text(task_entry_edit_)));
        if (title.empty()) { SetFocus(task_entry_edit_); return; }
        if (!mutate({{"op", "task.add"}, {"title", title}}).value("ok", false)) return;
        SetWindowTextW(task_entry_edit_, L"");
        tasks_scroll_ = 0;
        layout();
    }
    std::vector<Json> visible_notes() const {
        std::string query;
        if (footer_edit_) {
            try { query = tag_key(trim(narrow(edit_text(footer_edit_)))); } catch (const std::exception&) {}
        }
        if (!query.empty() && query.front() == '/') query.clear();
        std::vector<Json> result;
        for (auto note = snapshot_["notes"].rbegin(); note != snapshot_["notes"].rend(); ++note) {
            if (!note_matches(*note)) continue;
            if (!query.empty()) {
                auto haystack = tag_key(get_string(*note, "title") + "\n" + get_string(*note, "body"));
                for (const auto& tag : tags_of(*note)) haystack += "\n" + tag_key(tag);
                if (haystack.find(query) == std::string::npos) continue;
            }
            result.push_back(*note);
        }
        return result;
    }
    void create_note(std::string title) {
        auto command = Json{{"op", "note.add"}, {"title", title}};
        if (!note_filter_.empty()) command["tags"] = Json::array({note_filter_});
        auto result = mutate(command);
        if (!result.value("ok", false)) return;
        SetWindowTextW(footer_edit_, L"");
        notes_scroll_ = 0;
        snapshot_ = store_.snapshot();
        open_note(result["note"]);
    }
    // Enter in the notes search: run a / command, open the best match, or
    // create a note with that title when nothing matches.
    void notes_enter() {
        std::string value = trim(narrow(edit_text(footer_edit_)));
        if (value.empty()) { SetFocus(footer_edit_); return; }
        if (value.front() == '/') { run_note_command(value); return; }
        const auto notes = visible_notes();
        auto exact = std::find_if(notes.begin(), notes.end(), [&](const Json& note) {
            return tag_key(get_string(note, "title")) == tag_key(value);
        });
        if (exact != notes.end()) open_note(*exact);
        else if (!notes.empty()) open_note(notes.front());
        else create_note(value);
    }
    bool note_page_visible() const {
        return !settings_open_ && detail_ == Detail::none && note_page_ != NotePage::list;
    }
    static const char* note_command_name(NotePage page) {
        switch (page) {
        case NotePage::remove: return "/delete-note";
        case NotePage::trash: return "/trash";
        case NotePage::search: return "/search-notes";
        default: return "/find-note";
        }
    }
    static const wchar_t* note_command_description(NotePage page) {
        switch (page) {
        case NotePage::remove: return L"  Delete notes with a click";
        case NotePage::trash: return L"  Restore deleted notes";
        case NotePage::search: return L"  Search inside notes";
        default: return L"  Find a note by name";
        }
    }
    bool trash_note(const Json& note) {
        const auto id = get_string(note, "id");
        if (!mutate({{"op", "note.delete"}, {"id", id}}).value("ok", false)) return false;
        note_undo_id_ = id;
        note_undo_title_ = get_string(note, "title");
        note_undo_until_ = GetTickCount64() + 10000;
        snapshot_ = store_.snapshot();
        return true;
    }
    void restore_note(const std::string& id) {
        if (!mutate({{"op", "note.restore"}, {"id", id}}).value("ok", false)) return;
        if (note_undo_id_ == id) note_undo_id_.clear();
        snapshot_ = store_.snapshot();
        feedback(L"Note restored.");
        layout();
        SetFocus(note_page_visible() ? note_query_edit_ : window_);
    }
    void purge_note(const Json& note) {
        if (GetTickCount64() < note_delete_click_until_) return;
        const bool previous_menu = context_menu_;
        context_menu_ = true;
        const auto question = L"Permanently delete \"" + widen(get_string(note, "title")) +
            L"\"?\nThis cannot be undone.";
        const int answer = MessageBoxW(window_, question.c_str(), L"Knock", MB_YESNO | MB_DEFBUTTON2);
        context_menu_ = previous_menu;
        if (answer != IDYES) return;
        const auto id = get_string(note, "id");
        if (!mutate({{"op", "note.purge"}, {"id", id}}).value("ok", false)) return;
        if (note_undo_id_ == id) note_undo_id_.clear();
        note_delete_click_until_ = GetTickCount64() + GetDoubleClickTime();
        snapshot_ = store_.snapshot();
        feedback(L"Note permanently deleted.");
        layout();
        SetFocus(note_query_edit_);
    }
    void open_note_page(NotePage page, const std::string& query = {}) {
        if (!save_detail()) return;
        feedback_.clear();
        reset_tag_picker();
        note_page_ = page;
        note_delete_click_until_ = 0;
        note_results_dirty_ = true;
        note_search_pending_ = false;
        note_search_page_offset_ = 0;
        note_search_total_ = 0;
        note_search_next_.reset();
        note_result_offset_ = 0;
        note_selected_id_.clear();
        if (!note_query_edit_) note_query_edit_ = make_edit(content_, 51, false, L"Search...");
        SendMessageW(note_query_edit_, EM_SETLIMITTEXT, page == NotePage::search ? 128 : 512, 0);
        SendMessageW(note_query_edit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(
            page == NotePage::search ? L"Search conversation text..." : L"Find a note by name..."));
        populating_ = true;
        SetWindowTextW(note_query_edit_, widen(query).c_str());
        if (footer_edit_) SetWindowTextW(footer_edit_, L"");
        populating_ = false;
        layout();
        SetFocus(note_query_edit_);
        SendMessageW(note_query_edit_, EM_SETSEL, query.empty() ? 0 : -1, -1);
    }
    void close_note_page() {
        note_page_ = NotePage::list;
        note_results_.clear();
        note_selected_id_.clear();
        note_search_pending_ = false;
        if (note_query_edit_) DestroyWindow(note_query_edit_);
        note_query_edit_ = nullptr;
        layout();
        SetFocus(footer_edit_ ? footer_edit_ : window_);
    }
    void run_note_command(const std::string& value) {
        update_note_command_choices();
        const auto split = value.find_first_of(" \t");
        const auto name = tag_key(value.substr(0, split));
        const auto query = split == std::string::npos ? std::string() : trim(value.substr(split + 1));
        for (NotePage page : {NotePage::find, NotePage::search, NotePage::remove, NotePage::trash}) {
            if (name == note_command_name(page)) { open_note_page(page, query); return; }
        }
        if (split == std::string::npos && !note_command_choices_.empty()) {
            open_note_page(note_command_choices_[note_command_selection_]);
            return;
        }
        feedback(L"Unknown note command. Type / to see the available commands.");
        SetFocus(footer_edit_);
    }
    void update_note_command_choices() {
        note_command_choices_.clear();
        if (settings_open_ || detail_ != Detail::none || note_page_ != NotePage::list || !footer_edit_) return;
        const auto value = tag_key(trim(narrow(edit_text(footer_edit_))));
        if (value.empty() || value.front() != '/') return;
        const auto split = value.find_first_of(" \t");
        const auto prefix = value.substr(0, split);
        for (NotePage page : {NotePage::find, NotePage::search, NotePage::remove, NotePage::trash})
            if (std::string(note_command_name(page)).starts_with(prefix)) note_command_choices_.push_back(page);
        note_command_selection_ = std::min(note_command_selection_, note_command_choices_.empty() ? 0 : note_command_choices_.size() - 1);
    }
    void update_note_results() {
        if (note_page_ == NotePage::search) {
            if (!note_results_dirty_ || note_search_pending_) return;
            note_results_dirty_ = false;
            note_results_.clear();
            note_search_total_ = 0;
            note_search_next_.reset();
            const auto query = trim(narrow(edit_text(note_query_edit_)));
            if (!query.empty()) {
                auto result = mutate({{"op", "note.search"}, {"query", query},
                    {"offset", note_search_page_offset_}, {"limit", 100}});
                if (result.value("ok", false)) {
                    note_search_total_ = result.at("total").get<std::size_t>();
                    if (note_search_page_offset_ && note_search_page_offset_ >= note_search_total_) {
                        note_search_page_offset_ = note_search_total_ ? ((note_search_total_ - 1) / 100) * 100 : 0;
                        note_results_dirty_ = true;
                        update_note_results();
                        return;
                    }
                    for (auto& item : result.at("items")) note_results_.push_back(std::move(item));
                    if (!result.at("next_offset").is_null()) note_search_next_ = result.at("next_offset").get<std::size_t>();
                }
            }
            if (std::none_of(note_results_.begin(), note_results_.end(), [this](const auto& item) {
                    return note_result_id(item) == note_selected_id_;
                })) note_selected_id_ = note_results_.empty() ? std::string() : note_result_id(note_results_.front());
            return;
        }
        const auto query = widen(trim(narrow(edit_text(note_query_edit_))));
        struct Match { Json item; int rank; };
        std::vector<Match> matches;
        const auto records = snapshot_.value(note_page_ == NotePage::trash ? "trash" : "notes", Json::array());
        for (const auto& item : records) {
            const auto name = widen(get_string(item, "title"));
            const int position = query.empty() ? 0 : FindStringOrdinal(FIND_FROMSTART,
                name.c_str(), static_cast<int>(name.size()), query.c_str(), static_cast<int>(query.size()), TRUE);
            if (position >= 0) matches.push_back({item, position == 0 ? (name.size() == query.size() ? 0 : 1) : 2});
        }
        std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.item.value("updated_at", std::int64_t(0)) > b.item.value("updated_at", std::int64_t(0));
        });
        note_results_.clear();
        for (auto& match : matches) note_results_.push_back(std::move(match.item));
        const bool selected_exists = std::any_of(note_results_.begin(), note_results_.end(), [this](const Json& item) {
            return get_string(item, "id") == note_selected_id_;
        });
        if (!selected_exists) note_selected_id_ = note_results_.empty() ? std::string() : get_string(note_results_.front(), "id");
    }
    std::string note_result_id(const Json& item) const {
        return note_page_ == NotePage::search ? get_string(item, "note_id") + ":" + get_string(item, "message_id") :
            get_string(item, "id");
    }
    void change_search_page(std::size_t offset, bool at_end = false) {
        note_search_page_offset_ = offset;
        note_results_dirty_ = true;
        note_selected_id_.clear();
        update_note_results();
        note_result_offset_ = at_end ? std::max(0, static_cast<int>(note_results_.size()) - note_result_rows_) : 0;
        if (at_end && !note_results_.empty()) note_selected_id_ = note_result_id(note_results_.back());
        layout();
    }
    void scroll_note_results(int rows) {
        if (note_page_ == NotePage::search) {
            const int end = std::max(0, static_cast<int>(note_results_.size()) - note_result_rows_);
            if (rows > 0 && note_result_offset_ >= end && note_search_next_) {
                change_search_page(*note_search_next_); return;
            }
            if (rows < 0 && note_result_offset_ == 0 && note_search_page_offset_) {
                change_search_page(note_search_page_offset_ > 100 ? note_search_page_offset_ - 100 : 0, true); return;
            }
        }
        const int offset = std::clamp(note_result_offset_ + rows, 0,
            std::max(0, static_cast<int>(note_results_.size()) - note_result_rows_));
        if (offset == note_result_offset_) return;
        note_result_offset_ = offset;
        layout();
    }
    void move_note_selection(int amount) {
        if (note_results_.empty()) return;
        auto selected = std::find_if(note_results_.begin(), note_results_.end(), [this](const Json& item) {
            return note_result_id(item) == note_selected_id_;
        });
        if (note_page_ == NotePage::search && selected != note_results_.end()) {
            if (amount > 0 && std::next(selected) == note_results_.end() && note_search_next_) {
                change_search_page(*note_search_next_); return;
            }
            if (amount < 0 && selected == note_results_.begin() && note_search_page_offset_) {
                change_search_page(note_search_page_offset_ > 100 ? note_search_page_offset_ - 100 : 0, true); return;
            }
        }
        const int index = std::clamp(static_cast<int>(selected - note_results_.begin()) + amount,
            0, static_cast<int>(note_results_.size()) - 1);
        note_selected_id_ = note_result_id(note_results_[static_cast<std::size_t>(index)]);
        if (index < note_result_offset_) note_result_offset_ = index;
        if (index >= note_result_offset_ + note_result_rows_) note_result_offset_ = index - note_result_rows_ + 1;
        layout();
    }
    void activate_note_result(const Json& item) {
        if (note_page_ == NotePage::search) {
            note_selected_id_ = note_result_id(item);
            const auto id = get_string(item, "note_id");
            const auto& notes = snapshot_["notes"];
            const auto note = std::find_if(notes.begin(), notes.end(), [&id](const auto& value) {
                return get_string(value, "id") == id;
            });
            if (note == notes.end()) { feedback(L"This note is no longer available."); return; }
            begin_detail(Detail::note, *note);
            if (detail_ == Detail::note && detail_id_ == id) jump_to_note_message(get_string(item, "message_id"));
            return;
        }
        const auto id = get_string(item, "id");
        if (note_page_ == NotePage::find) {
            note_selected_id_ = id;
            begin_detail(Detail::note, item);
            return;
        }
        if (note_page_ == NotePage::trash) {
            if (GetTickCount64() < note_delete_click_until_) return;
            restore_note(id);
            note_delete_click_until_ = GetTickCount64() + GetDoubleClickTime();
            return;
        }
        if (note_page_ != NotePage::remove || GetTickCount64() < note_delete_click_until_) return;
        if (!trash_note(item)) return;
        // Rows move after deletion; ignore the second half of a double-click.
        note_delete_click_until_ = GetTickCount64() + GetDoubleClickTime();
        layout();
        SetFocus(note_query_edit_);
    }
    bool note_keys(HWND target, WPARAM key) {
        if (!settings_open_ && detail_ == Detail::note) {
            if (key == 'F' && GetKeyState(VK_CONTROL) < 0) { open_note_find(); return true; }
            if (note_find_open_ && (key == VK_F3 || (target == note_find_edit_ && key == VK_RETURN))) {
                move_note_find(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return true;
            }
        }
        if (!settings_open_ && detail_ == Detail::note && !note_metadata_open_ && !tag_picker_open_ &&
            target == composer_edit_ && key == VK_UP && GetKeyState(VK_CONTROL) >= 0 &&
            GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_MENU) >= 0 && !note_edit_ &&
            edit_text(composer_edit_).empty() && note_draft_.images.empty() && !note_messages_.empty()) {
            begin_note_message_edit(note_messages_.back());
            return true;
        }
        if (target == footer_edit_ && !note_command_choices_.empty() && (key == VK_UP || key == VK_DOWN)) {
            if (key == VK_UP) note_command_selection_ = (note_command_selection_ + note_command_choices_.size() - 1) % note_command_choices_.size();
            else note_command_selection_ = (note_command_selection_ + 1) % note_command_choices_.size();
            layout();
            return true;
        }
        if (target != note_query_edit_ || !note_page_visible()) return false;
        if (key == VK_UP || key == VK_DOWN) { move_note_selection(key == VK_UP ? -1 : 1); return true; }
        if (key == VK_RETURN) {
            // Deletion is an explicit row action, never a side effect of typing a query.
            if (note_page_ == NotePage::find || note_page_ == NotePage::search) {
                auto selected = std::find_if(note_results_.begin(), note_results_.end(), [this](const Json& item) {
                    return note_result_id(item) == note_selected_id_;
                });
                if (selected != note_results_.end()) activate_note_result(*selected);
            }
            return true;
        }
        return false;
    }
    bool note_matches(const Json& note) const {
        const auto tags = tags_of(note);
        if (untagged_filter_) return tags.empty();
        if (note_filter_.empty()) return true;
        return std::any_of(tags.begin(), tags.end(), [this](const auto& tag) {
            return tag_key(tag) == tag_key(note_filter_);
        });
    }
    std::vector<std::string> available_tags() const {
        std::map<std::string, std::string> unique;
        for (const auto& note : snapshot_["notes"])
            for (const auto& tag : tags_of(note)) unique.try_emplace(tag_key(tag), tag);
        if (detail_ == Detail::note) {
            for (const auto& tag : detail_tags_) unique.try_emplace(tag_key(tag), tag);
            for (const auto* tag : {"Personal", "Ideas", "Projects", "Reference"})
                unique.try_emplace(tag_key(tag), tag);
        }
        std::vector<std::string> result;
        for (const auto& [key, value] : unique) { (void)key; result.push_back(value); }
        return result;
    }
    bool has_detail_tag(const std::string& value) const {
        return std::any_of(detail_tags_.begin(), detail_tags_.end(), [&](const auto& tag) {
            return tag_key(tag) == tag_key(value);
        });
    }
    void reset_tag_picker() {
        tag_picker_open_ = false;
        tag_offset_ = 0;
        tag_choices_.clear();
        if (tag_query_edit_) DestroyWindow(tag_query_edit_);
        tag_query_edit_ = nullptr;
    }
    void close_tag_picker() {
        reset_tag_picker();
        layout();
        SetFocus(detail_ == Detail::note ? (note_metadata_open_ ? body_edit_ : composer_edit_) : window_);
    }
    void open_tag_picker() {
        if (tag_picker_open_) { close_tag_picker(); return; }
        if (settings_open_ && detail_ == Detail::none) close_settings();
        if (detail_ == Detail::none && note_page_ != NotePage::list) close_note_page();
        tag_picker_open_ = true;
        tag_offset_ = 0;
        tag_query_edit_ = make_edit(content_, 36, false,
            detail_ == Detail::note ? L"Find or create a tag..." : L"Find a tag...");
        SendMessageW(tag_query_edit_, EM_SETLIMITTEXT, 128, 0);
        layout();
        SetFocus(tag_query_edit_);
    }
    void select_note_filter(std::string tag, bool untagged = false) {
        note_filter_ = std::move(tag);
        untagged_filter_ = untagged;
        notes_scroll_ = 0;
        close_tag_picker();
    }
    bool set_note_tags(const std::vector<std::string>& tags) {
        if (tags.size() > 12) { feedback(L"A note can have up to 12 tags."); return false; }
        for (const auto& tag : tags) {
            auto characters = std::count_if(tag.begin(), tag.end(), [](unsigned char ch) { return (ch & 0xc0) != 0x80; });
            if (tag.size() > 128 || characters > 32) {
                feedback(L"Tag names can contain up to 32 characters."); return false;
            }
        }
        // Persist the selection without overwriting text still being edited.
        auto result = mutate({{"op", "note.update"}, {"id", detail_id_}, {"tags", tags}});
        if (!result.value("ok", false)) return false;
        detail_tags_ = tags_of(result.at("note"));
        return true;
    }
    void toggle_note_tag(const std::string& tag) {
        auto tags = detail_tags_;
        auto found = std::find_if(tags.begin(), tags.end(), [&](const auto& value) {
            return tag_key(value) == tag_key(tag);
        });
        if (found == tags.end()) tags.push_back(tag); else tags.erase(found);
        if (set_note_tags(tags)) layout();
    }
    void create_note_tag() {
        if (!tag_query_edit_ || detail_ != Detail::note) return;
        auto value = trim(narrow(edit_text(tag_query_edit_)));
        if (value.empty()) return;
        auto tags = detail_tags_;
        if (!has_detail_tag(value)) tags.push_back(value);
        if (!set_note_tags(tags)) return;
        SetWindowTextW(tag_query_edit_, L"");
        tag_offset_ = 0;
        layout();
        SetFocus(tag_query_edit_);
    }
    void activate_tag_query() {
        if (!tag_picker_open_ || !tag_query_edit_) return;
        auto query = trim(narrow(edit_text(tag_query_edit_)));
        if (detail_ == Detail::note) {
            if (query.empty()) return;
            auto available = available_tags();
            auto exact = std::find_if(available.begin(), available.end(), [&](const auto& tag) {
                return tag_key(tag) == tag_key(query);
            });
            if (exact != available.end()) toggle_note_tag(*exact); else create_note_tag();
        } else if (query.empty()) select_note_filter({});
        else if (!tag_choices_.empty()) select_note_filter(tag_choices_.front());
    }
    void scroll_tags(int amount) {
        tag_offset_ = std::clamp(tag_offset_ + amount, 0,
            std::max(0, static_cast<int>(tag_choices_.size()) - tag_visible_rows_));
        layout();
    }
    void layout_tag_picker(RECT area) {
        const bool editing = detail_ == Detail::note;
        int top = editing ? px(37) : px(3);
        int bottom = std::min(static_cast<int>(area.bottom) - px(8), top + px(360));
        tag_panel_ = RECT{0, top, area.right, bottom};
        MoveWindow(tag_query_edit_, px(7), top + px(29), area.right - px(14), px(23), FALSE);
        ShowWindow(tag_query_edit_, SW_SHOW);
        const auto query = tag_key(trim(narrow(edit_text(tag_query_edit_))));
        tag_choices_.clear();
        for (const auto& tag : available_tags())
            if (query.empty() || tag_key(tag).find(query) != std::string::npos) tag_choices_.push_back(tag);
        int y = top + px(63);
        if (!editing && query.empty()) {
            button("tag-all", content_, L"All notes", rect(px(7), y, area.right - px(14), px(22)),
                [this] { select_note_filter({}); }, true, false, note_filter_.empty() && !untagged_filter_);
            y += px(26);
            button("tag-untagged", content_, L"Untagged", rect(px(7), y, area.right - px(14), px(22)),
                [this] { select_note_filter({}, true); }, true, false, untagged_filter_);
            y += px(26);
        }
        tag_visible_rows_ = std::max(1, (bottom - px(38) - y) / px(26));
        tag_offset_ = std::clamp(tag_offset_, 0,
            std::max(0, static_cast<int>(tag_choices_.size()) - tag_visible_rows_));
        int end = std::min(static_cast<int>(tag_choices_.size()), tag_offset_ + tag_visible_rows_);
        for (int i = tag_offset_; i < end; ++i) {
            const auto tag = tag_choices_[static_cast<std::size_t>(i)];
            const bool selected = editing ? has_detail_tag(tag) : tag_key(note_filter_) == tag_key(tag);
            auto label = (editing ? (selected ? L"[x] " : L"[ ] ") : L"# ") + widen(tag);
            button("tag-choice:" + tag_key(tag), content_, label, rect(px(7), y, area.right - px(14), px(22)),
                [this, editing, tag] { if (editing) toggle_note_tag(tag); else select_note_filter(tag); },
                true, false, selected);
            y += px(26);
        }
        if (editing) {
            auto choices = available_tags();
            bool exists = std::any_of(choices.begin(), choices.end(), [&](const auto& tag) { return tag_key(tag) == query; });
            button("tag-create", content_, L"CREATE TAG", rect(px(7), bottom - px(29), px(116), px(22)),
                [this] { create_note_tag(); }, !query.empty() && !exists);
        }
        if (static_cast<int>(tag_choices_.size()) > tag_visible_rows_) {
            int x = editing ? px(133) : px(7);
            button("tag-up", content_, L"UP", rect(x, bottom - px(29), px(39), px(22)),
                [this] { scroll_tags(-tag_visible_rows_); }, tag_offset_ > 0);
            button("tag-down", content_, L"DOWN", rect(x + px(45), bottom - px(29), px(45), px(22)),
                [this] { scroll_tags(tag_visible_rows_); }, end < static_cast<int>(tag_choices_.size()));
        }
        button("tag-done", content_, L"DONE", rect(area.right - px(69), bottom - px(29), px(62), px(22)),
            [this] { close_tag_picker(); });
    }
    void draw_tag_picker(HDC dc) const {
        fill(dc, tag_panel_, background);
        outline(dc, tag_panel_, border);
        const bool editing = detail_ == Detail::note;
        text(dc, editing ? L"CHOOSE TAGS" : L"FILTER NOTES",
            RECT{px(7), tag_panel_.top + px(7), tag_panel_.right - px(91), tag_panel_.top + px(23)}, small_font_, muted);
        if (editing) text(dc, std::to_wstring(detail_tags_.size()) + L" / 12",
            RECT{tag_panel_.right - px(85), tag_panel_.top + px(7), tag_panel_.right - px(7), tag_panel_.top + px(23)},
            small_font_, muted, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        outline(dc, RECT{px(6), tag_panel_.top + px(28), tag_panel_.right - px(6), tag_panel_.top + px(53)}, border);
        if (tag_choices_.empty() && tag_query_edit_ && !edit_text(tag_query_edit_).empty())
            text(dc, editing ? L"Create a tag with this name below." : L"No matching tags.",
                RECT{px(9), tag_panel_.top + px(70), tag_panel_.right - px(9), tag_panel_.top + px(105)}, small_font_, muted);
    }

    std::filesystem::path note_draft_path() const {
        return data_directory() / L"drafts" / (detail_id_ + ".conversation.json");
    }
    NoteDraft& composer_draft() { return note_edit_ ? note_edit_->draft : note_draft_; }
    const NoteDraft& composer_draft() const { return note_edit_ ? note_edit_->draft : note_draft_; }
    void load_note_draft() {
        note_draft_ = {};
        note_edit_.reset();
        note_draft_dirty_ = false;
        auto path = note_draft_path();
        if (!std::filesystem::exists(path)) return;
        try {
            if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Draft too large");
            std::ifstream input(path, std::ios::binary);
            auto saved = Json::parse(input);
            input.close();
            auto value = saved.value("text", std::string());
            auto images = saved.value("images", std::vector<std::string>());
            if (value.size() > 256 * 1024 || images.size() > 8 ||
                std::any_of(images.begin(), images.end(), [](const auto& name) { return !valid_note_image_name(name); }))
                throw std::runtime_error("Invalid note draft");
            note_draft_ = {std::move(value), std::move(images), saved.value("client_id", std::string())};
            if (saved.contains("edit")) {
                const auto& edit = saved.at("edit");
                auto id = edit.at("message_id").get<std::string>();
                auto text = edit.at("text").get<std::string>();
                auto attachments = edit.at("images").get<std::vector<std::string>>();
                const auto revision = edit.at("revision").get<std::int64_t>();
                if (id.empty() || id.size() > 128 || text.size() > 256 * 1024 || revision < 0 ||
                    attachments.size() > 8 || std::any_of(attachments.begin(), attachments.end(),
                        [](const auto& name) { return !valid_note_image_name(name); }))
                    throw std::runtime_error("Invalid entry edit draft");
                note_edit_ = NoteEdit{std::move(id), revision, {std::move(text), std::move(attachments), {}}};
                const auto current = store_.handle({{"op", "note.message.get"}, {"id", detail_id_},
                    {"message_id", note_edit_->message_id}});
                if (current.value("ok", false)) {
                    const auto& entry = current.at("message");
                    // The edit may have committed before its draft was cleared.
                    if (entry.value("revision", std::int64_t(0)) > revision &&
                        get_string(entry, "text") == note_edit_->draft.text &&
                        entry.at("images") == Json(note_edit_->draft.images)) {
                        note_edit_.reset();
                        note_draft_dirty_ = true;
                    }
                } else feedback(L"This entry was removed. Your edit is kept in the composer.");
            }
            if (!note_draft_.client_id.empty()) {
                auto result = mutate({{"op", "note.find_message"}, {"id", detail_id_}, {"client_id", note_draft_.client_id}});
                if (result.value("ok", false) && !result.at("message").is_null()) {
                    note_draft_ = {};
                    note_draft_dirty_ = true;
                }
            }
            if (note_draft_dirty_) save_note_draft();
        } catch (const std::exception&) {
            auto preserved = path;
            preserved += ".unreadable." + std::to_string(now_ms());
            std::filesystem::rename(path, preserved);
            feedback(L"The unreadable draft was kept in the drafts folder.");
        }
    }
    bool save_note_draft() {
        if (detail_ != Detail::note || !note_draft_dirty_) return true;
        if (composer_edit_) composer_draft().text = narrow(edit_text(composer_edit_));
        try {
            auto path = note_draft_path();
            std::filesystem::create_directories(path.parent_path());
            if (note_draft_.text.empty() && note_draft_.images.empty() && !note_edit_) {
                std::filesystem::remove(path);
            } else {
                auto temporary = path;
                temporary += ".tmp";
                {
                    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                    Json draft{{"note_id", detail_id_}, {"title", detail_original_title_},
                               {"text", note_draft_.text}, {"images", note_draft_.images},
                               {"client_id", note_draft_.client_id}};
                    if (note_edit_) draft["edit"] = {{"message_id", note_edit_->message_id},
                        {"revision", note_edit_->revision}, {"text", note_edit_->draft.text},
                        {"images", note_edit_->draft.images}};
                    output << draft.dump(2);
                    output.flush();
                    if (!output) throw std::runtime_error("Could not write the note draft");
                }
                if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("Could not save the note draft");
            }
            note_draft_dirty_ = false;
            KillTimer(window_, 4);
            return true;
        } catch (const std::exception& error) {
            feedback(widen(error.what()));
            return false;
        }
    }
    bool load_note_history() {
        Json messages = Json::array();
        std::size_t offset = note_offset_;
        bool first = true;
        // A page also has a byte budget, so large entries may need more than
        // one read even when the message-count limit has not been reached.
        for (;;) {
            auto result = mutate({{"op", "note.get"}, {"id", detail_id_}, {"offset", offset}, {"limit", 100}});
            if (!result.value("ok", false)) return false;
            auto& note = result.at("note");
            note_message_count_ = result.at("total_messages").get<std::size_t>();
            if (first) {
                note_record_ = note;
                note_record_.erase("messages");
                detail_tags_ = tags_of(note);
                if (!detail_dirty_) detail_original_title_ = get_string(note, "title");
                first = false;
            }
            for (auto& item : note.at("messages")) messages.push_back(std::move(item));
            if (result.at("next_offset").is_null()) break;
            const auto next = result.at("next_offset").get<std::size_t>();
            if (next <= offset) throw std::runtime_error("Note history did not advance");
            offset = next;
        }
        note_messages_ = std::move(messages);
        note_layout_dirty_ = true;
        return true;
    }
    int note_scroll_limit() const {
        return std::max(0, note_history_height_ - static_cast<int>(note_history_bounds_.bottom - note_history_bounds_.top));
    }
    void scroll_note_to_end() {
        note_scroll_ = note_scroll_limit();
        layout();
    }
    void load_earlier_messages() {
        const int previous_height = note_history_height_;
        const int previous_scroll = note_scroll_;
        note_offset_ = note_offset_ > 100 ? note_offset_ - 100 : 0;
        if (!load_note_history()) return;
        layout();
        note_scroll_ = std::clamp(previous_scroll + note_history_height_ - previous_height, 0, note_scroll_limit());
        layout();
    }
    void jump_to_note_message(const std::string& id) {
        auto result = store_.handle({{"op", "note.message.get"}, {"id", detail_id_}, {"message_id", id}});
        if (!result.value("ok", false)) {
            feedback(L"This entry is no longer available.");
            return;
        }
        const auto index = result.at("message_index").get<std::size_t>();
        const bool loaded = std::any_of(note_messages_.begin(), note_messages_.end(), [&id](const auto& message) {
            return get_string(message, "id") == id;
        });
        if (!loaded || index < note_offset_) {
            note_offset_ = index > 10 ? index - 10 : 0;
            if (!load_note_history()) return;
        }
        layout();
        for (const auto& row : note_rows_) {
            if (get_string(note_messages_[row.index], "id") != id) continue;
            note_scroll_ = std::clamp(row.top - px(8), 0, note_scroll_limit());
            note_highlight_id_ = id;
            note_highlight_until_ = GetTickCount64() + 5000;
            layout();
            return;
        }
        feedback(L"This entry changed while the search was open. Search again.");
    }
    void open_note_find() {
        if (detail_ != Detail::note) return;
        if (note_metadata_open_) {
            open_note_metadata();
            if (note_metadata_open_) return;
        }
        if (tag_picker_open_) close_tag_picker();
        if (!note_find_edit_) {
            note_find_edit_ = make_edit(content_, 52, false, L"Find in this conversation...");
            SendMessageW(note_find_edit_, EM_SETLIMITTEXT, 128, 0);
        }
        const bool newly_opened = !note_find_open_;
        note_find_open_ = true;
        if (newly_opened && !edit_text(note_find_edit_).empty()) refresh_note_find();
        layout();
        SetFocus(note_find_edit_);
        SendMessageW(note_find_edit_, EM_SETSEL, 0, -1);
    }
    void close_note_find() {
        note_find_open_ = false;
        note_find_pending_ = false;
        layout();
        SetFocus(composer_edit_);
    }
    void refresh_note_find(bool jump = true) {
        if (!note_find_open_ || detail_ != Detail::note || !note_find_edit_) return;
        const auto previous_id = note_find_selection_ < note_find_results_.size() ?
            get_string(note_find_results_[note_find_selection_], "message_id") : std::string();
        note_find_results_.clear();
        note_find_selection_ = 0;
        note_find_pending_ = false;
        const auto query = trim(narrow(edit_text(note_find_edit_)));
        if (query.empty()) { layout(); return; }
        auto result = mutate({{"op", "note.search_locations"}, {"id", detail_id_}, {"query", query}});
        if (!result.value("ok", false)) { layout(); return; }
        for (auto& item : result.at("items")) note_find_results_.push_back(std::move(item));
        const auto previous = std::find_if(note_find_results_.begin(), note_find_results_.end(), [&previous_id](const auto& item) {
            return get_string(item, "message_id") == previous_id;
        });
        if (previous != note_find_results_.end()) note_find_selection_ = static_cast<std::size_t>(previous - note_find_results_.begin());
        if (jump && !note_find_results_.empty()) jump_to_note_message(get_string(note_find_results_[note_find_selection_], "message_id"));
        else layout();
    }
    void move_note_find(int direction) {
        if (note_find_pending_) { refresh_note_find(); return; }
        if (note_find_results_.empty()) return;
        if (direction < 0) note_find_selection_ = (note_find_selection_ + note_find_results_.size() - 1) % note_find_results_.size();
        else note_find_selection_ = (note_find_selection_ + 1) % note_find_results_.size();
        jump_to_note_message(get_string(note_find_results_[note_find_selection_], "message_id"));
    }
    void open_note_metadata() {
        if (note_metadata_open_) {
            if (!save_detail()) return;
            note_metadata_open_ = false;
            for (HWND* edit : {&title_edit_, &body_edit_}) {
                if (*edit) DestroyWindow(*edit);
                *edit = nullptr;
            }
            layout();
            SetFocus(composer_edit_);
            return;
        }
        reset_tag_picker();
        note_metadata_open_ = true;
        populating_ = true;
        title_edit_ = make_edit(content_, 31, false, L"Note title");
        body_edit_ = make_edit(content_, 32, true, L"Description (optional)");
        SetWindowTextW(title_edit_, widen(get_string(note_record_, "title")).c_str());
        SetWindowTextW(body_edit_, widen(get_string(note_record_, "body")).c_str());
        populating_ = false;
        detail_dirty_ = false;
        layout();
        SetFocus(title_edit_);
    }
    void begin_note_message_edit(const Json& entry) {
        if (note_edit_) {
            feedback(L"Save or cancel the current entry edit first.");
            SetFocus(composer_edit_);
            return;
        }
        if (!save_note_draft()) return;
        feedback_.clear();
        note_edit_ = NoteEdit{get_string(entry, "id"), entry.value("revision", std::int64_t(0)),
            {get_string(entry, "text"), entry.at("images").get<std::vector<std::string>>(), {}}};
        populating_ = true;
        SetWindowTextW(composer_edit_, widen(note_edit_->draft.text).c_str());
        populating_ = false;
        note_draft_dirty_ = true;
        save_note_draft();
        layout();
        SetFocus(composer_edit_);
        SendMessageW(composer_edit_, EM_SETSEL, -1, -1);
    }
    void cancel_note_message_edit() {
        if (!note_edit_) return;
        note_edit_->draft.text = narrow(edit_text(composer_edit_));
        const auto previous = note_edit_;
        note_edit_.reset();
        populating_ = true;
        SetWindowTextW(composer_edit_, widen(note_draft_.text).c_str());
        populating_ = false;
        note_draft_dirty_ = true;
        if (!save_note_draft()) {
            note_edit_ = previous;
            populating_ = true;
            SetWindowTextW(composer_edit_, widen(note_edit_->draft.text).c_str());
            populating_ = false;
        } else load_note_history();
        layout();
        SetFocus(composer_edit_);
    }
    void save_note_message_edit() {
        if (!note_edit_) return;
        note_draft_dirty_ = true;
        if (!save_note_draft()) return;
        auto result = store_.handle({{"op", "note.message.edit"}, {"id", detail_id_},
            {"message_id", note_edit_->message_id}, {"expected_revision", note_edit_->revision},
            {"text", note_edit_->draft.text}, {"images", note_edit_->draft.images}}, true);
        if (!result.value("ok", false)) {
            const auto error = get_string(result, "error");
            feedback(error == "message_edit_conflict" ?
                L"This entry changed elsewhere. Your edit is kept; cancel to reload the latest version." : widen(error));
            return;
        }
        note_edit_.reset();
        populating_ = true;
        SetWindowTextW(composer_edit_, widen(note_draft_.text).c_str());
        populating_ = false;
        note_draft_dirty_ = true;
        save_note_draft();
        load_note_history();
        layout();
        feedback(L"Entry updated.");
        SetFocus(composer_edit_);
    }
    bool paste_note_attachment() {
        auto pasted = paste_note_image(window_, data_directory());
        if (pasted.status == ClipboardImageStatus::no_image) return false;
        if (pasted.status == ClipboardImageStatus::error) { feedback(pasted.error); return true; }
        auto& draft = composer_draft();
        if (draft.images.size() >= 8) {
            remove_note_image(data_directory(), pasted.filename);
            feedback(L"You can add up to eight images to one entry.");
            return true;
        }
        draft.images.push_back(std::move(pasted.filename));
        draft.client_id.clear();
        note_draft_dirty_ = true;
        save_note_draft();
        layout();
        SetFocus(composer_edit_);
        return true;
    }
    void remove_pending_image(const std::string& filename) {
        auto& draft = composer_draft();
        auto previous = draft.images;
        std::erase(draft.images, filename);
        draft.client_id.clear();
        note_draft_dirty_ = true;
        if (!save_note_draft()) { draft.images = std::move(previous); return; }
        // Keep imported files when detaching them. An earlier successful send
        // may still reference one after a failed draft cleanup or crash.
        note_image_cache_.erase(filename);
        layout();
        SetFocus(composer_edit_);
    }
    void send_note_message() {
        if (!composer_edit_ || detail_ != Detail::note || tag_picker_open_ || note_metadata_open_) return;
        if (note_edit_) { save_note_message_edit(); return; }
        auto value = narrow(edit_text(composer_edit_));
        if (trim(value).empty() && note_draft_.images.empty()) { SetFocus(composer_edit_); return; }
        if (note_draft_.client_id.empty()) note_draft_.client_id = send_token();
        note_draft_dirty_ = true;
        if (!save_note_draft()) return;
        auto result = mutate({{"op", "note.append"}, {"id", detail_id_}, {"text", value}, {"images", note_draft_.images},
                              {"client_id", note_draft_.client_id}});
        if (!result.value("ok", false)) return;
        populating_ = true;
        SetWindowTextW(composer_edit_, L"");
        populating_ = false;
        note_draft_ = {};
        note_draft_dirty_ = true;
        save_note_draft();
        if (load_note_history()) { layout(); scroll_note_to_end(); }
        SetFocus(composer_edit_);
    }
    void cache_note_images() {
        const auto& images = composer_draft().images;
        std::set<std::string> visible(images.begin(), images.end());
        const int height = note_history_bounds_.bottom - note_history_bounds_.top;
        for (const auto& row : note_rows_) {
            if (row.top + row.height < note_scroll_ || row.top > note_scroll_ + height) continue;
            for (const auto& name : note_messages_[row.index].at("images")) visible.insert(name.get<std::string>());
        }
        for (const auto& name : visible) {
            if (note_image_cache_.contains(name)) continue;
            auto loaded = std::make_unique<NoteImage>();
            if (!loaded->load(data_directory(), name)) loaded.reset();
            note_image_cache_.emplace(name, std::move(loaded));
        }
        for (auto it = note_image_cache_.begin(); it != note_image_cache_.end() && note_image_cache_.size() > 24;) {
            if (!visible.contains(it->first)) it = note_image_cache_.erase(it); else ++it;
        }
    }
    void draw_note_image(HDC dc, const std::string& filename, RECT bounds) const {
        fill(dc, bounds, RGB(15, 15, 15));
        auto found = note_image_cache_.find(filename);
        if (found != note_image_cache_.end() && found->second) found->second->draw(dc, bounds);
        else text(dc, L"Image unavailable", bounds, small_font_, muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        outline(dc, bounds, border);
    }
    void open_note_image(const std::string& filename) {
        if (!valid_note_image_name(filename)) return;
        auto path = data_directory() / L"note-images" / filename;
        if (!std::filesystem::is_regular_file(path)) { feedback(L"This image is no longer available."); return; }
        ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    std::string note_image_at(int x, int y) const {
        if (x < px(10) || x > note_history_bounds_.right - px(12)) return {};
        if (y < note_history_bounds_.top || y >= note_history_bounds_.bottom) return {};
        const int position = y - note_history_bounds_.top + note_scroll_;
        for (const auto& row : note_rows_) {
            int image_y = row.top + px(23) + row.text_height;
            for (const auto& image : note_messages_[row.index].at("images")) {
                if (position >= image_y && position < image_y + px(150)) return image.get<std::string>();
                image_y += px(160);
            }
        }
        return {};
    }
    void app_context_menu(HWND source, LPARAM position) {
        POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
        if (point.x == -1 && point.y == -1) {
            RECT bounds{};
            GetWindowRect(source, &bounds);
            point = POINT{bounds.left + px(12), bounds.top + px(12)};
        }
        std::vector<MenuItem> items{{1, L"Settings...", !settings_open_}, {2, L"Hide Knock"},
            {3, busy() ? busy_caption() : L"Quiet notifications..."}, {4, L"Status..."}};
        if (!settings_open_ && detail_ == Detail::note) items.push_back({24, L"Find in note...  Ctrl+F"});
        wchar_t class_name[16]{};
        GetClassNameW(source, class_name, 16);
        const bool editor = lstrcmpiW(class_name, L"Edit") == 0;
        Json entry;
        std::string filename;
        const auto note_id = detail_id_;
        if (editor) {
            DWORD first = 0, last = 0;
            SendMessageW(source, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
            const bool selection = first != last;
            const bool paste = IsClipboardFormatAvailable(CF_UNICODETEXT) ||
                (source == composer_edit_ && (IsClipboardFormatAvailable(CF_DIB) ||
                  IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_BITMAP) ||
                  IsClipboardFormatAvailable(CF_HDROP)));
            items.insert(items.end(), {{0, L"", false, true},
                {10, L"Undo", SendMessageW(source, EM_CANUNDO, 0, 0) != 0},
                {11, L"Cut", selection}, {12, L"Copy", selection}, {13, L"Paste", paste},
                {14, L"Select all", GetWindowTextLengthW(source) > 0}});
        } else if (!settings_open_ && source == content_ && detail_ == Detail::note &&
                   !tag_picker_open_ && !note_metadata_open_) {
            POINT local = point;
            ScreenToClient(content_, &local);
            if (local.y >= note_history_bounds_.top && local.y < note_history_bounds_.bottom) {
                const int y = local.y - note_history_bounds_.top + note_scroll_;
                auto found = std::find_if(note_rows_.begin(), note_rows_.end(), [&](const auto& row) {
                    return y >= row.top && y < row.top + row.height;
                });
                if (found != note_rows_.end()) {
                    entry = note_messages_[found->index];
                    filename = note_image_at(local.x, local.y);
                    items.push_back({0, L"", false, true});
                    items.push_back({20, L"Copy entry", !get_string(entry, "text").empty()});
                    items.push_back({23, L"Edit entry", !note_edit_});
                    if (!filename.empty()) items.push_back({21, L"Open image"});
                    items.push_back({22, L"Delete entry"});
                }
            }
        }
        const int selected = popup_menu(point, items);
        if (selected == 1) open_settings();
        if (selected == 2) hide();
        if (selected == 3) busy_menu(point);
        if (selected == 4) open_presence();
        if (selected >= 10 && selected <= 14 && IsWindow(source)) {
            SetFocus(source);
            const UINT commands[]{EM_UNDO, WM_CUT, WM_COPY, WM_PASTE, EM_SETSEL};
            SendMessageW(source, commands[selected - 10], 0, selected == 14 ? -1 : 0);
        }
        if (selected == 20) { copy(get_string(entry, "text")); feedback(L"Entry copied."); }
        if (selected == 21) open_note_image(filename);
        if (selected == 23) begin_note_message_edit(entry);
        if (selected == 24) open_note_find();
        if (selected == 22) {
            const bool previous_menu = context_menu_;
            context_menu_ = true;
            const int answer = MessageBoxW(window_, L"Delete this entry?", L"Knock", MB_YESNO | MB_DEFBUTTON2);
            context_menu_ = previous_menu;
            if (answer == IDYES)
                mutate({{"op", "note.message.remove"}, {"id", note_id}, {"message_id", entry.at("id")}});
        }
    }

    void begin_reply(const std::string& id) {
        if (reply_edit_) {
            drafts_[reply_id_] = narrow(edit_text(reply_edit_));
            DestroyWindow(reply_edit_);
        }
        reply_id_ = id;
        reply_edit_ = make_edit(content_, 35, true, L"Not right now, continue with headless tests...");
        SendMessageW(reply_edit_, EM_SETLIMITTEXT, 16000, 0);
        SetWindowTextW(reply_edit_, widen(drafts_[id]).c_str());
        scroll_to_card(id);
        SetFocus(reply_edit_);
    }
    void cancel_reply(bool keep_draft = true) {
        if (reply_edit_) {
            if (keep_draft) drafts_[reply_id_] = narrow(edit_text(reply_edit_));
            DestroyWindow(reply_edit_);
            reply_edit_ = nullptr;
        }
        reply_id_.clear();
        layout();
        SetFocus(window_);
    }
    void send_reply() {
        if (!reply_edit_) return;
        auto value = narrow(edit_text(reply_edit_));
        if (trim(value).empty()) { feedback(L"Write a reply first."); return; }
        auto result = mutate({{"op", "request.resolve"}, {"id", reply_id_}, {"decision", "custom"}, {"text", value}});
        if (result.value("ok", false)) {
            // Lay out from the answered state, not the stale pending one.
            snapshot_ = store_.snapshot();
            drafts_.erase(reply_id_);
            cancel_reply(false);
        }
    }
    void resolve(const std::string& id, const char* decision) {
        if (!mutate({{"op", "request.resolve"}, {"id", id}, {"decision", decision}}).value("ok", false)) return;
        snapshot_ = store_.snapshot();
        if (reply_id_ == id) cancel_reply(false);
    }
    void copy(const std::string& value, const wchar_t* done = L"Copied. Paste into the named agent chat.") {
        auto wide = widen(value);
        if (!OpenClipboard(window_)) { feedback(L"Clipboard is busy. Try again."); return; }
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
        if (!memory) { CloseClipboard(); feedback(L"Could not copy text."); return; }
        void* target = GlobalLock(memory);
        if (!target) { GlobalFree(memory); CloseClipboard(); return; }
        std::memcpy(target, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(memory);
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        CloseClipboard();
        feedback(done);
    }
    void view_log(const std::string& path) {
        if (path.empty()) return;
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        auto notepad = std::filesystem::path(system) / L"notepad.exe";
        auto argument = quote_argument(widen(path));
        ShellExecuteW(window_, L"open", notepad.c_str(), argument.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void button(const std::string& key, HWND parent, const std::wstring& label, RECT bounds,
                std::function<void()> action, bool enabled = true, bool unused = false,
                bool active = false, const Json& card = Json()) {
        (void)unused;
        int id;
        if (auto found = button_keys_.find(key); found != button_keys_.end()) id = found->second;
        else {
            id = ++button_id_;
            Button model;
            model.handle = CreateWindowExW(0, L"BUTTON", label.c_str(),
                WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, parent,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
            SetWindowSubclass(model.handle, button_proc, 1, reinterpret_cast<DWORD_PTR>(this));
            buttons_.emplace(id, std::move(model));
            button_keys_[key] = id;
        }
        auto& model = buttons_.at(id);
        model.seen = true;
        model.label = label;
        model.action = std::move(action);
        model.active = active;
        model.arrow = key == "notes-toggle";
        model.close = key.starts_with("card-dismiss:");
        model.bare = model.arrow || model.close;
        model.left_aligned = key.starts_with("tag-choice:") || key == "tag-all" || key == "tag-untagged" ||
            key == "settings-startup" || key.starts_with("note-command:");
        model.note_result = key.starts_with("note-result:");
        model.record = card;
        SetWindowTextW(model.handle, label.c_str());
        EnableWindow(model.handle, enabled);
        SetWindowPos(model.handle, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
            bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW);
        InvalidateRect(model.handle, nullptr, FALSE);
    }
    void sweep_buttons() {
        for (auto it = button_keys_.begin(); it != button_keys_.end();) {
            auto model = buttons_.find(it->second);
            if (model != buttons_.end() && !model->second.seen) {
                if (GetFocus() == model->second.handle) SetFocus(window_);
                DestroyWindow(model->second.handle);
                buttons_.erase(model);
                it = button_keys_.erase(it);
            } else ++it;
        }
    }

    void draw_button(const DRAWITEMSTRUCT& item) {
        auto found = buttons_.find(static_cast<int>(item.CtlID));
        if (found == buttons_.end()) return;
        const auto& button = found->second;
        RECT bounds = item.rcItem;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        COLORREF color = (item.itemState & ODS_DISABLED) ? subdued : foreground;
        if (button.bare) {
            fill(item.hDC, bounds, button.arrow ? frame : background);
            const COLORREF ink = button.hover || pressed ? foreground : RGB(112, 112, 112);
            if (button.arrow) {
                // A solid triangle: pointing left collapses the notes, right brings them back.
                const int size = std::max(3, px(7));
                const int cx = bounds.right / 2, cy = bounds.bottom / 2;
                const bool collapse = !window_settings_.notes_collapsed();
                for (int i = 0; i < size; ++i) {
                    const int x = collapse ? cx - size / 2 + i : cx + size / 2 - i;
                    fill(item.hDC, RECT{x, cy - i - 1, x + 1, cy + i + 1}, ink);
                }
            } else {
                text(item.hDC, L"\u00D7", bounds, title_font_, ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        } else {
            COLORREF bg = button.hover || pressed ? hover_fill : well;
            fill(item.hDC, bounds, background);
            HBRUSH brush = CreateSolidBrush(bg);
            HPEN pen = CreatePen(PS_SOLID, 1, button.active ? RGB(120, 120, 116) :
                (item.itemState & ODS_FOCUS) ? muted : bg);
            auto old_brush = SelectObject(item.hDC, brush);
            auto old_pen = SelectObject(item.hDC, pen);
            RoundRect(item.hDC, 0, 0, bounds.right, bounds.bottom, px(4), px(4));
            if (button.active) {
                RECT inner = bounds;
                InflateRect(&inner, -1, -1);
                SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
                RoundRect(item.hDC, inner.left, inner.top, inner.right, inner.bottom, px(4), px(4));
            }
            SelectObject(item.hDC, old_pen);
            SelectObject(item.hDC, old_brush);
            DeleteObject(pen);
            DeleteObject(brush);
        }
        if (button.note_result) {
            const bool deleting = note_page_ == NotePage::remove;
            text(item.hDC, widen(get_string(button.record, "title")),
                RECT{px(8), px(6), bounds.right - px(65), px(24)}, body_font_, foreground,
                DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            text(item.hDC, deleting ? L"DELETE" : note_page_ == NotePage::trash ? L"RESTORE" : L"OPEN",
                RECT{bounds.right - px(61), px(6), bounds.right - px(8), px(24)}, small_font_, deleting ? orange : muted,
                DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            auto caption = note_page_ == NotePage::search ? widen(get_string(button.record, "excerpt")) :
                note_page_ == NotePage::trash ?
                L"Deleted " + display_time(button.record.value("deleted_at", std::int64_t(0))) :
                tag_caption(tags_of(button.record));
            if (caption.empty()) caption = L"Untagged";
            text(item.hDC, caption, RECT{px(8), px(28), bounds.right - px(8), bounds.bottom - px(5)},
                small_font_, muted, (note_page_ == NotePage::search ? DT_WORDBREAK : DT_SINGLELINE) | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else if (!button.bare) {
            if (button.left_aligned) { bounds.left = px(8); bounds.right -= px(8); }
            else InflateRect(&bounds, -px(3), 0);
            text(item.hDC, button.label, bounds, body_font_, color,
                 (button.left_aligned ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT) && button.bare) {
            RECT focus = item.rcItem;
            InflateRect(&focus, -px(2), -px(2));
            DrawFocusRect(item.hDC, &focus);
        }
    }

    // ------------------------------------------------------------ pixel text and characters
    int pixel_scale(double logical) const {
        return std::max(1, static_cast<int>(std::lround(logical * static_cast<double>(dpi_) / 96.0)));
    }
    static bool pixel_printable(const std::wstring& value) {
        return std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= 32 && c < 127; });
    }
    static int pixel_width(const std::wstring& value, int scale) {
        return value.empty() ? 0 : static_cast<int>(value.size()) * (pixel_glyph_width + 1) * scale - scale;
    }
    // Draws a heading in the bitmap font, shortened with ".." to fit. Text the
    // font cannot show (non-ASCII job names) falls back to the title font.
    int pixel_text(HDC dc, std::wstring value, RECT box, int scale, COLORREF color, bool center) const {
        if (!pixel_printable(value)) {
            text(dc, value, box, title_font_, color, (center ? DT_CENTER : DT_LEFT) | DT_VCENTER | DT_SINGLELINE |
                 DT_END_ELLIPSIS | DT_NOPREFIX);
            return box.right - box.left;
        }
        const int available = box.right - box.left;
        if (pixel_width(value, scale) > available) {
            while (value.size() > 1 && pixel_width(value + L"..", scale) > available) value.pop_back();
            value += L"..";
        }
        const int width = pixel_width(value, scale);
        const int x0 = center ? box.left + (available - width) / 2 : box.left;
        const int y0 = box.top + ((box.bottom - box.top) - pixel_glyph_height * scale) / 2;
        HBRUSH brush = CreateSolidBrush(color);
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto* glyph = pixel_glyph(static_cast<char32_t>(value[i]));
            if (!glyph) continue;
            for (int row = 0; row < pixel_glyph_height; ++row) {
                for (int column = 0; column < pixel_glyph_width; ++column) {
                    if (!(glyph[row] & (1 << (pixel_glyph_width - 1 - column)))) continue;
                    const int x = x0 + (static_cast<int>(i) * (pixel_glyph_width + 1) + column) * scale;
                    const int y = y0 + row * scale;
                    RECT dot{x, y, x + scale, y + scale};
                    FillRect(dc, &dot, brush);
                }
            }
        }
        DeleteObject(brush);
        return width;
    }
    const Character& character_for(const std::string& chat) {
        auto found = characters_.find(chat);
        if (found == characters_.end()) found = characters_.emplace(chat, make_character(chat)).first;
        return found->second;
    }
    void draw_character(HDC dc, const Character& character, RECT box) const {
        // Fit the drawn pixels, not the whole canvas, so every creature fills its space.
        int left = Character::width, top = Character::height, right = -1, bottom = -1;
        for (int y = 0; y < Character::height; ++y)
            for (int x = 0; x < Character::width; ++x)
                if (character.at(x, y)) {
                    left = std::min(left, x); right = std::max(right, x);
                    top = std::min(top, y); bottom = std::max(bottom, y);
                }
        if (right < 0) return;
        const int columns = right - left + 1, rows = bottom - top + 1;
        const int scale = std::max(1, std::min({static_cast<int>(box.right - box.left) / columns,
                                                static_cast<int>(box.bottom - box.top) / rows, px(9)}));
        const int x0 = box.left + ((box.right - box.left) - columns * scale) / 2 - left * scale;
        const int y0 = box.top + ((box.bottom - box.top) - rows * scale) / 2 - top * scale;
        HBRUSH brushes[std::tuple_size_v<decltype(character.palette)>]{};
        for (std::size_t i = 1; i < character.palette.size(); ++i) {
            const auto color = character.palette[i];
            brushes[i] = CreateSolidBrush(RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
        }
        for (int y = 0; y < Character::height; ++y) {
            for (int x = 0; x < Character::width;) {
                const auto value = character.at(x, y);
                int run = 1;
                while (x + run < Character::width && character.at(x + run, y) == value) ++run;
                if (value) {
                    RECT block{x0 + x * scale, y0 + y * scale, x0 + (x + run) * scale, y0 + (y + 1) * scale};
                    FillRect(dc, &block, brushes[value]);
                }
                x += run;
            }
        }
        for (auto brush : brushes) if (brush) DeleteObject(brush);
    }
    void rounded(HDC dc, RECT box, COLORREF color, int radius) const {
        HBRUSH brush = CreateSolidBrush(color);
        auto old_brush = SelectObject(dc, brush);
        auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, box.left, box.top, box.right + 1, box.bottom + 1, radius, radius);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(brush);
    }

    // Moving and destroying controls can change focus, and focus changes can
    // ask for another layout. Run that afterwards rather than inside this one.
    void layout() {
        if (!window_ || !content_) return;
        if (laying_out_) { relayout_ = true; return; }
        laying_out_ = true;
        try {
            do {
                relayout_ = false;
                layout_once();
            } while (relayout_);
        } catch (...) { laying_out_ = false; throw; }
        laying_out_ = false;
    }
    void layout_once() {
        for (auto& [id, model] : buttons_) { (void)id; model.seen = false; }
        RECT client{};
        GetClientRect(window_, &client);
        const int width = client.right;
        const int height = client.bottom;
        const int full = full_width_ ? full_width_ : width;
        const bool collapsed = window_settings_.notes_collapsed();
        const int margin = px(10), gap = px(14);
        int tasks_w = column_width(full, 17, 220, 320);
        int notes_w = column_width(full, 17, 220, 320);
        int presence_w = column_width(full, 13, 190, 250);
        // On a narrow screen the side columns give way before the job cards do.
        const int fixed = 2 * margin + 3 * gap + px(8) + px(28);
        if (full - fixed - tasks_w - notes_w - presence_w < px(320)) {
            tasks_w = std::max(px(170), (full - fixed - px(320)) * 34 / 100);
            notes_w = collapsed ? notes_w : tasks_w;
            presence_w = std::max(px(150), (full - fixed - px(320)) * 32 / 100);
        }
        arrow_rect_ = RECT{width - px(24), 1, width - 1, height - 1};
        notes_rect_ = collapsed ? RECT{} : RECT{arrow_rect_.left - px(4) - notes_w, margin, arrow_rect_.left - px(4), height - margin};
        const int presence_right = collapsed ? arrow_rect_.left - gap : notes_rect_.left - gap;
        presence_rect_ = RECT{presence_right - presence_w, margin, presence_right, height - margin};
        tasks_rect_ = RECT{margin, margin, margin + tasks_w, height - margin};
        middle_rect_ = RECT{tasks_rect_.right + gap + px(8), margin, presence_rect_.left - gap, height - margin};
        const int header = px(34);

        // Task column: heading, the list and a field for a new task.
        task_entry_bounds_ = RECT{tasks_rect_.left, tasks_rect_.bottom - px(26), tasks_rect_.right, tasks_rect_.bottom};
        MoveWindow(task_entry_edit_, task_entry_bounds_.left + px(4), task_entry_bounds_.top + px(3),
                   tasks_w - px(8), px(20), FALSE);
        MoveWindow(tasks_view_, tasks_rect_.left, tasks_rect_.top + header, tasks_w,
                   std::max(px(20), static_cast<int>(task_entry_bounds_.top) - px(6) - static_cast<int>(tasks_rect_.top + header)), FALSE);
        layout_tasks_panel();

        // Status column: three choices and an optional message.
        const char* states[]{"at_pc", "away", "busy"};
        const wchar_t* labels[]{L"AT PC", L"AWAY", L"BUSY"};
        const int choice_gap = px(6);
        const int choice_w = (presence_w - 2 * choice_gap) / 3;
        const auto current = get_string(presence(), "state", "unknown");
        for (int i = 0; i < 3; ++i) {
            const std::string state = states[i];
            button("presence-state:" + state, window_, labels[i],
                rect(presence_rect_.left + i * (choice_w + choice_gap), presence_rect_.top, choice_w, px(28)),
                [this, state] { set_presence(state); }, true, false, current == state);
        }
        presence_box_ = RECT{presence_rect_.left, presence_rect_.top + px(52), presence_rect_.right,
                             presence_rect_.top + px(52) + std::clamp(height / 4, px(56), px(96))};
        MoveWindow(presence_edit_, presence_box_.left + px(4), presence_box_.top + px(4),
                   presence_w - px(8), presence_box_.bottom - presence_box_.top - px(8), FALSE);
        int line = presence_box_.bottom + px(10);
        lease_line_y_ = 0;
        if (snapshot_.contains("lease") && !snapshot_["lease"].is_null()) {
            lease_line_y_ = line;
            button("release", window_, L"RELEASE", rect(presence_rect_.right - px(76), line, px(76), px(22)), [this] {
                if (!snapshot_["lease"].is_null()) mutate({{"op", "request.release"}, {"id", snapshot_["lease"]["id"]}});
            });
            line += px(46);
        }
        quiet_line_y_ = 0;
        if (busy()) {
            quiet_line_y_ = line;
            button("busy-end", window_, L"END", rect(presence_rect_.right - px(52), line, px(52), px(22)),
                   [this] { set_busy_minutes(0); });
        }

        // Notes column: heading with search, an optional filter line and the list.
        const bool notes_shown = !collapsed;
        ShowWindow(footer_edit_, notes_shown ? SW_SHOW : SW_HIDE);
        filter_line_y_ = 0;
        note_command_bounds_ = {};
        note_undo_bounds_ = {};
        if (notes_shown) {
            const int heading_w = pixel_width(L"NOTES", pixel_scale(2.0)) + px(12);
            search_bounds_ = RECT{notes_rect_.left + heading_w, notes_rect_.top + px(3),
                                  notes_rect_.right - px(54), notes_rect_.top + px(27)};
            MoveWindow(footer_edit_, search_bounds_.left + px(22), search_bounds_.top + px(3),
                       std::max(px(20), static_cast<int>(search_bounds_.right - search_bounds_.left) - px(30)), px(18), FALSE);
            button("notes-filter", window_, L"#", rect(notes_rect_.right - px(50), notes_rect_.top + px(3), px(24), px(24)),
                   [this] { open_tag_picker(); }, true, false, !note_filter_.empty() || untagged_filter_);
            button("notes-new", window_, L"+", rect(notes_rect_.right - px(24), notes_rect_.top + px(3), px(24), px(24)), [this] {
                std::string title;
                try { title = trim(narrow(edit_text(footer_edit_))); } catch (const std::exception&) {}
                create_note(title.empty() || title.front() == '/' ? "New note" : title);
            });
            int list_top = notes_rect_.top + header;
            if (!note_filter_.empty() || untagged_filter_) {
                filter_line_y_ = list_top;
                button("notes-filter-clear", window_, L"CLEAR", rect(notes_rect_.right - px(58), list_top, px(58), px(20)),
                       [this] { note_filter_.clear(); untagged_filter_ = false; notes_scroll_ = 0; layout(); });
                list_top += px(26);
            }
            int list_bottom = notes_rect_.bottom;
            if (dashboard() || detail_ != Detail::note) {
                if (!note_undo_id_.empty() && GetTickCount64() < note_undo_until_) {
                    note_undo_bounds_ = RECT{notes_rect_.left, list_bottom - px(24), notes_rect_.right, list_bottom};
                    const auto id = note_undo_id_;
                    button("note-undo", window_, L"UNDO", rect(notes_rect_.right - px(56), list_bottom - px(24), px(56), px(23)),
                           [this, id] { restore_note(id); });
                    list_bottom -= px(30);
                }
            }
            update_note_command_choices();
            note_command_visible_ = 0;
            if (!note_command_choices_.empty() || (GetFocus() == footer_edit_ && [&] {
                    try { auto v = trim(narrow(edit_text(footer_edit_))); return !v.empty() && v.front() == '/'; }
                    catch (const std::exception&) { return false; } }())) {
                const int available = list_bottom - list_top;
                note_command_visible_ = std::min(note_command_choices_.size(),
                    static_cast<std::size_t>(std::max(1, (available - px(10)) / px(29))));
                if (note_command_selection_ < note_command_first_) note_command_first_ = note_command_selection_;
                if (note_command_visible_ && note_command_selection_ >= note_command_first_ + note_command_visible_)
                    note_command_first_ = note_command_selection_ - note_command_visible_ + 1;
                note_command_first_ = std::min(note_command_first_, note_command_choices_.size() - note_command_visible_);
                const int box_height = note_command_choices_.empty() ? px(49) : px(10 + 29 * static_cast<int>(note_command_visible_));
                note_command_bounds_ = RECT{notes_rect_.left, list_top, notes_rect_.right, list_top + box_height};
                for (std::size_t i = note_command_first_; i < note_command_first_ + note_command_visible_; ++i) {
                    const auto page = note_command_choices_[i];
                    const auto label = widen(note_command_name(page)) + note_command_description(page);
                    button("note-command:" + std::string(note_command_name(page)), window_, label,
                        rect(note_command_bounds_.left + px(4), note_command_bounds_.top + px(4) + static_cast<int>(i - note_command_first_) * px(29),
                             note_command_bounds_.right - note_command_bounds_.left - px(8), px(23)),
                        [this, page] {
                            const auto value = trim(narrow(edit_text(footer_edit_)));
                            const auto split = value.find_first_of(" \t");
                            open_note_page(page, split == std::string::npos ? std::string() : trim(value.substr(split + 1)));
                        }, true, false, i == note_command_selection_);
                }
            }
            MoveWindow(notes_view_, notes_rect_.left, list_top, notes_w, std::max(px(20), list_bottom - list_top), FALSE);
            ShowWindow(notes_view_, note_command_bounds_.bottom > note_command_bounds_.top ? SW_HIDE : SW_SHOW);
            layout_notes_panel();
        } else ShowWindow(notes_view_, SW_HIDE);
        button("notes-toggle", window_, collapsed ? L"SHOW NOTES" : L"HIDE NOTES",
               rect(arrow_rect_.left + 1, arrow_rect_.top, arrow_rect_.right - arrow_rect_.left - 1, arrow_rect_.bottom - arrow_rect_.top),
               [this] { toggle_notes(); });

        // The middle column shows the job cards, or the page opened from a side panel.
        MoveWindow(content_, middle_rect_.left, middle_rect_.top, middle_rect_.right - middle_rect_.left,
                   middle_rect_.bottom - middle_rect_.top, FALSE);
        RECT area{};
        GetClientRect(content_, &area);
        viewport_height_ = area.bottom;
        if (note_query_edit_) ShowWindow(note_query_edit_, note_page_visible() ? SW_SHOW : SW_HIDE);
        if (note_find_edit_) ShowWindow(note_find_edit_, !settings_open_ && detail_ == Detail::note &&
            !note_metadata_open_ && !tag_picker_open_ && note_find_open_ ? SW_SHOW : SW_HIDE);
        if (settings_open_) {
            for (HWND edit : {title_edit_, body_edit_, operator_edit_, project_edit_, reply_edit_,
                              tag_query_edit_, composer_edit_}) if (edit) ShowWindow(edit, SW_HIDE);
            layout_settings(area);
        }
        else if (detail_ != Detail::none) layout_detail(area);
        else if (note_page_visible()) layout_note_page(area);
        else if (tag_picker_open_) { content_height_ = area.bottom; layout_tag_picker(area); }
        else layout_dashboard(area);
        if (reply_edit_ && !dashboard()) ShowWindow(reply_edit_, SW_HIDE);
        sweep_buttons();
        InvalidateRect(window_, nullptr, FALSE);
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    void toggle_notes() {
        if (!window_settings_.set_notes_collapsed(!window_settings_.notes_collapsed()))
            feedback(window_settings_.error());
        position_popup();
        layout();
    }

    // ------------------------------------------------------------ side panels
    int line_height(HFONT font) const { return text_height(L"Ag", px(200), font); }
    // The drawn lists also expose their titles as window text, in order, for
    // screen readers and the native UI tests.
    static void describe_panel(HWND panel, const wchar_t* name, const std::vector<PanelRow>& rows) {
        std::wstring summary = name;
        for (const auto& row : rows) {
            summary += (summary.size() == wcslen(name) ? L": " : L" | ") + widen(get_string(row.item, "title"));
            if (summary.size() > 4000) break;
        }
        SetWindowTextW(panel, summary.c_str());
    }
    void layout_tasks_panel() {
        RECT area{};
        GetClientRect(tasks_view_, &area);
        task_rows_.clear();
        int y = 0;
        const int title_lines = line_height(row_font_);
        for (auto task = snapshot_["tasks"].rbegin(); task != snapshot_["tasks"].rend(); ++task) {
            const int title = std::min(text_height(widen(get_string(*task, "title")), area.right - px(18), row_font_, title_lines),
                                       title_lines * 2);
            const int height = px(10) + title + px(6) + line_height(small_font_) + px(8);
            task_rows_.push_back({*task, y, height});
            y += height + px(6);
        }
        tasks_extent_ = y;
        describe_panel(tasks_view_, L"Tasks", task_rows_);
        tasks_scroll_ = std::clamp(tasks_scroll_, 0, std::max(0, tasks_extent_ - static_cast<int>(area.bottom)));
        InvalidateRect(tasks_view_, nullptr, FALSE);
    }
    void layout_notes_panel() {
        RECT area{};
        GetClientRect(notes_view_, &area);
        note_list_rows_.clear();
        int y = 0;
        const int title_lines = line_height(row_font_);
        for (const auto& note : visible_notes()) {
            const int title = std::min(text_height(widen(get_string(note, "title")), area.right - px(30), row_font_, title_lines),
                                       title_lines * 2);
            const int height = px(8) + title + px(8) + line_height(small_font_) + px(8);
            note_list_rows_.push_back({note, y, height});
            y += height;
        }
        notes_extent_ = y;
        describe_panel(notes_view_, L"Notes", note_list_rows_);
        notes_scroll_ = std::clamp(notes_scroll_, 0, std::max(0, notes_extent_ - static_cast<int>(area.bottom)));
        InvalidateRect(notes_view_, nullptr, FALSE);
    }
    void draw_panel_scroll(HDC dc, RECT area, int extent, int offset) const {
        if (extent <= area.bottom) return;
        const int thumb = std::max(px(20), static_cast<int>(area.bottom) * static_cast<int>(area.bottom) / extent);
        const int top = (area.bottom - thumb) * offset / std::max(1, extent - static_cast<int>(area.bottom));
        fill(dc, RECT{area.right - std::max(2, px(2)), top, area.right, top + thumb}, RGB(70, 70, 70));
    }
    void draw_tasks_panel(HDC dc) {
        RECT area{};
        GetClientRect(tasks_view_, &area);
        fill(dc, area, frame);
        if (task_rows_.empty()) {
            text(dc, L"No tasks yet.", RECT{px(4), px(6), area.right, px(30)}, row_font_, foreground);
            text(dc, L"Add one below, or let an agent add it with knock tasks add.",
                 RECT{px(4), px(32), area.right - px(4), px(90)}, small_font_, muted);
            return;
        }
        const int title_lines = line_height(row_font_);
        for (std::size_t i = 0; i < task_rows_.size(); ++i) {
            const auto& row = task_rows_[i];
            const int top = row.top - tasks_scroll_;
            if (top + row.height < 0 || top > area.bottom) continue;
            const RECT box{0, top, area.right - px(6), top + row.height};
            fill(dc, box, static_cast<int>(i) == hover_task_ ? RGB(31, 31, 31) : well);
            text(dc, widen(get_string(row.item, "title")),
                 RECT{px(9), top + px(10), box.right - px(9), top + px(10) + std::min(row.height - px(24), title_lines * 2)},
                 row_font_, foreground, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
            const auto status = get_string(row.item, "status", "todo");
            const COLORREF dot = status == "done" ? green : status == "in_progress" ? blue : orange;
            std::wstring meta = status == "done" ? L"DONE" : status == "in_progress" ? L"IN-PROGRESS" : L"TO-DO";
            meta += L"  OPERATOR:" + widen(get_string(row.item, "operator", "ME"));
            const auto project = get_string(row.item, "project");
            if (!project.empty()) meta += L"  PROJECT: " + widen(project);
            const int meta_y = box.bottom - px(8) - line_height(small_font_);
            fill(dc, rect(px(9), meta_y + line_height(small_font_) / 2 - px(2), px(4), px(4)), dot);
            text(dc, meta, RECT{px(17), meta_y, box.right - px(6), box.bottom - px(6)}, small_font_, subdued,
                 DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        draw_panel_scroll(dc, area, tasks_extent_, tasks_scroll_);
    }
    void draw_notes_panel(HDC dc) {
        RECT area{};
        GetClientRect(notes_view_, &area);
        fill(dc, area, background);
        if (note_list_rows_.empty()) {
            const bool any = !snapshot_["notes"].empty();
            text(dc, any ? L"No matching notes." : L"No notes yet.", RECT{px(4), px(6), area.right, px(30)}, row_font_, foreground);
            text(dc, any ? L"Press Enter in search to create a note with that title." :
                 L"Type a title in search and press Enter, or use +.",
                 RECT{px(4), px(32), area.right - px(4), px(90)}, small_font_, muted);
            return;
        }
        const int title_lines = line_height(row_font_);
        for (std::size_t i = 0; i < note_list_rows_.size(); ++i) {
            const auto& row = note_list_rows_[i];
            const int top = row.top - notes_scroll_;
            if (top + row.height < 0 || top > area.bottom) continue;
            const RECT box{0, top, area.right - px(4), top + row.height};
            if (static_cast<int>(i) == hover_note_) fill(dc, box, RGB(38, 38, 38));
            text(dc, widen(get_string(row.item, "title")),
                 RECT{px(4), top + px(8), box.right - px(22), top + px(8) + std::min(row.height - px(24), title_lines * 2)},
                 row_font_, RGB(200, 200, 194), DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
            auto tags = tag_caption(tags_of(row.item));
            if (tags.empty()) tags = L"Untagged";
            const int meta_y = box.bottom - px(8) - line_height(small_font_);
            text(dc, tags, RECT{px(4), meta_y, box.right - px(110), box.bottom}, small_font_, subdued,
                 DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            std::time_t time = static_cast<std::time_t>(row.item.value("updated_at", std::int64_t(0)) / 1000);
            std::tm local{};
            localtime_s(&local, &time);
            wchar_t date[32]{};
            std::wcsftime(date, 32, L"%d %b, %H:%M", &local);
            text(dc, date, RECT{box.right - px(110), meta_y, box.right - px(22), box.bottom}, small_font_, subdued,
                 DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
            text(dc, L">", RECT{box.right - px(18), top, box.right - px(4), box.bottom}, row_font_, subdued,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            fill(dc, RECT{0, box.bottom - 1, box.right, box.bottom}, divider);
        }
        draw_panel_scroll(dc, area, notes_extent_, notes_scroll_);
    }
    int panel_row_at(HWND panel, int y) const {
        const auto& rows = panel == tasks_view_ ? task_rows_ : note_list_rows_;
        const int offset = y + (panel == tasks_view_ ? tasks_scroll_ : notes_scroll_);
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (offset >= rows[i].top && offset < rows[i].top + rows[i].height) return static_cast<int>(i);
        return -1;
    }
    LRESULT panel_message(HWND target, UINT message, WPARAM wparam, LPARAM lparam) {
        const bool tasks = target == tasks_view_;
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT state{};
            HDC dc = BeginPaint(target, &state);
            RECT area{};
            GetClientRect(target, &area);
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, area.right), std::max(1L, area.bottom));
            auto old = SelectObject(buffer, bitmap);
            if (tasks) draw_tasks_panel(buffer); else draw_notes_panel(buffer);
            BitBlt(dc, 0, 0, area.right, area.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, old);
            DeleteObject(bitmap);
            DeleteDC(buffer);
            EndPaint(target, &state);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEWHEEL: {
            RECT area{};
            GetClientRect(target, &area);
            int& offset = tasks ? tasks_scroll_ : notes_scroll_;
            const int extent = tasks ? tasks_extent_ : notes_extent_;
            offset = std::clamp(offset - GET_WHEEL_DELTA_WPARAM(wparam) * px(48) / WHEEL_DELTA, 0,
                                std::max(0, extent - static_cast<int>(area.bottom)));
            InvalidateRect(target, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int& hover = tasks ? hover_task_ : hover_note_;
            const int row = panel_row_at(target, GET_Y_LPARAM(lparam));
            if (row != hover) {
                hover = row;
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, target, 0};
                TrackMouseEvent(&track);
                InvalidateRect(target, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            (tasks ? hover_task_ : hover_note_) = -1;
            InvalidateRect(target, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int row = panel_row_at(target, GET_Y_LPARAM(lparam));
            if (row < 0) return 0;
            const auto item = (tasks ? task_rows_ : note_list_rows_)[static_cast<std::size_t>(row)].item;
            if (tasks) open_task(item); else open_note(item);
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_CONTEXTMENU: app_context_menu(target, lparam); return 0;
        case WM_KEYDOWN: if (wparam == VK_ESCAPE) { escape(); return 0; } break;
        }
        return DefWindowProcW(target, message, wparam, lparam);
    }
    void layout_settings(RECT area) {
        content_height_ = area.bottom;
        for (HWND edit : {settings_width_edit_, settings_height_edit_})
            if (edit) ShowWindow(edit, settings_page_ == SettingsPage::general ? SW_SHOW : SW_HIDE);
        if (settings_retention_edit_) ShowWindow(settings_retention_edit_, settings_page_ == SettingsPage::preferences ? SW_SHOW : SW_HIDE);
        if (settings_reply_edit_) ShowWindow(settings_reply_edit_, settings_page_ == SettingsPage::reply ? SW_SHOW : SW_HIDE);
        button("settings-back", content_, L"< BACK", rect(0, px(2), px(68), px(22)), [this] { settings_back(); });
        if (settings_page_ == SettingsPage::preferences) {
            button("settings-replies", content_, L"MANAGE", rect(area.right - px(82), px(53), px(82), px(23)),
                   [this] { settings_page(SettingsPage::replies); });
            button("settings-busy", content_, busy() ? L"CHANGE" : L"SET TIMER", rect(area.right - px(82), px(111), px(82), px(23)),
                   [this] { open_busy_menu(); });
            MoveWindow(settings_retention_edit_, px(1), px(196), px(63), px(24), FALSE);
            ShowWindow(settings_retention_edit_, SW_SHOW);
            button("settings-retention", content_, L"APPLY", rect(area.right - px(82), px(195), px(82), px(25)),
                   [this] { apply_trash_retention(); });
            return;
        }
        if (settings_page_ == SettingsPage::replies) {
            const auto replies = saved_replies();
            button("settings-reply-add", content_, L"ADD", rect(area.right - px(60), px(2), px(60), px(22)),
                   [this] { edit_saved_reply(-1); }, replies.size() < 12);
            const int visible = std::max(1, (static_cast<int>(area.bottom) - px(113)) / px(38));
            saved_reply_offset_ = std::clamp(saved_reply_offset_, 0, std::max(0, static_cast<int>(replies.size()) - visible));
            for (int row = 0; row < visible && row + saved_reply_offset_ < static_cast<int>(replies.size()); ++row) {
                const int index = row + saved_reply_offset_;
                const auto key = "settings-reply:" + std::to_string(index);
                button(key, content_, reply_caption(replies[static_cast<std::size_t>(index)], 160),
                    rect(0, px(69) + row * px(38), area.right, px(31)), [this, index] { edit_saved_reply(index); });
                buttons_.at(button_keys_.at(key)).left_aligned = true;
            }
            button("settings-reply-previous", content_, L"< PREV", rect(0, area.bottom - px(29), px(74), px(23)),
                   [this, visible] { saved_reply_offset_ -= visible; layout(); }, saved_reply_offset_ > 0);
            button("settings-reply-next", content_, L"NEXT >", rect(area.right - px(74), area.bottom - px(29), px(74), px(23)),
                   [this, visible] { saved_reply_offset_ += visible; layout(); }, saved_reply_offset_ + visible < static_cast<int>(replies.size()));
            return;
        }
        if (settings_page_ == SettingsPage::reply) {
            MoveWindow(settings_reply_edit_, px(2), px(67), area.right - px(12), std::max(px(64), static_cast<int>(area.bottom) - px(136)), FALSE);
            ShowWindow(settings_reply_edit_, SW_SHOW);
            button("settings-reply-save", content_, L"SAVE", rect(0, area.bottom - px(29), px(62), px(23)),
                   [this] { save_saved_reply(); });
            button("settings-reply-cancel", content_, L"CANCEL", rect(px(70), area.bottom - px(29), px(72), px(23)),
                   [this] { settings_page(SettingsPage::replies); });
            if (saved_reply_index_ >= 0) button("settings-reply-delete", content_, L"DELETE", rect(area.right - px(72), area.bottom - px(29), px(72), px(23)),
                   [this] { delete_saved_reply(); });
            return;
        }
        button("settings-more", content_, L"MORE >", rect(area.right - px(76), px(2), px(76), px(22)),
               [this] { settings_page(SettingsPage::preferences); });
        button("settings-hotkey", content_, L"CHANGE", rect(area.right - px(82), px(62), px(82), px(23)),
               [this] { configure_hotkey(); });
        const int field_width = (area.right - px(104)) / 2;
        MoveWindow(settings_width_edit_, px(1), px(176), field_width - px(2), px(25), FALSE);
        MoveWindow(settings_height_edit_, field_width + px(13), px(176), field_width - px(2), px(25), FALSE);
        ShowWindow(settings_width_edit_, SW_SHOW);
        ShowWindow(settings_height_edit_, SW_SHOW);
        button("settings-apply", content_, L"APPLY", rect(area.right - px(82), px(176), px(82), px(25)),
               [this] { apply_window_size(); });
        button("settings-reset", content_, L"RESET", rect(area.right - px(82), px(210), px(82), px(23)),
               [this] { reset_window_size(); });
        button("settings-startup", content_, startup_enabled() ? L"[x] Start with Windows" : L"[ ] Start with Windows",
               rect(0, px(252), area.right, px(23)), [this] { toggle_startup(); layout(); });
    }
    void draw_settings(HDC dc, RECT area) {
        const auto heading = settings_page_ == SettingsPage::general ? L"SETTINGS" :
            settings_page_ == SettingsPage::preferences ? L"PREFERENCES" :
            settings_page_ == SettingsPage::replies ? L"SAVED REPLIES" : saved_reply_index_ < 0 ? L"NEW REPLY" : L"EDIT REPLY";
        text(dc, heading, RECT{px(82), px(2), area.right - px(settings_page_ == SettingsPage::general ||
            settings_page_ == SettingsPage::preferences ? 82 : settings_page_ == SettingsPage::replies ? 66 : 0), px(24)}, title_font_, foreground,
             DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        fill(dc, RECT{0, px(34), area.right, px(34) + 1}, border);
        if (settings_page_ == SettingsPage::preferences) {
            text(dc, L"SAVED REPLIES", RECT{0, px(43), area.right - px(94), px(62)}, small_font_, muted, DT_SINGLELINE);
            text(dc, std::to_wstring(saved_replies().size()) + L" replies", RECT{0, px(65), area.right - px(94), px(84)}, body_font_, foreground, DT_SINGLELINE);
            text(dc, L"QUIET TIMER", RECT{0, px(101), area.right - px(94), px(120)}, small_font_, muted, DT_SINGLELINE);
            text(dc, busy() ? busy_caption() : L"Banners are on", RECT{0, px(124), area.right - px(94), px(144)}, small_font_, foreground, DT_SINGLELINE | DT_END_ELLIPSIS);
            text(dc, L"KEEP DELETED NOTES", RECT{0, px(169), area.right, px(187)}, small_font_, muted, DT_SINGLELINE);
            outline(dc, rect(0, px(195), px(66), px(27)), border);
            text(dc, L"days (1-365)", RECT{px(76), px(196), area.right - px(93), px(221)}, small_font_, subdued, DT_SINGLELINE | DT_VCENTER);
            const bool showing_feedback = !feedback_.empty() && GetTickCount64() < feedback_until_;
            text(dc, showing_feedback ? feedback_ : L"Older Trash entries are removed when applied.",
                 RECT{0, px(237), area.right, area.bottom}, small_font_, showing_feedback ? orange : subdued,
                 DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
            return;
        }
        if (settings_page_ == SettingsPage::replies) {
            const bool showing_feedback = !feedback_.empty() && GetTickCount64() < feedback_until_;
            text(dc, showing_feedback ? feedback_ : L"Click a reply to edit it. Up to 12 replies.",
                 RECT{0, px(43), area.right, px(64)}, small_font_, showing_feedback ? orange : muted,
                 DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (saved_replies().empty()) text(dc, L"No saved replies yet. Choose ADD to create one.",
                RECT{px(6), px(85), area.right - px(6), px(143)}, body_font_, muted);
            return;
        }
        if (settings_page_ == SettingsPage::reply) {
            text(dc, L"This fills the reply box; SEND submits it.", RECT{0, px(43), area.right, px(63)}, small_font_, muted,
                 DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            outline(dc, RECT{0, px(65), area.right, area.bottom - px(67)}, border);
            draw_editor_scroll(dc, settings_reply_edit_);
            const bool showing_feedback = !feedback_.empty() && GetTickCount64() < feedback_until_;
            text(dc, showing_feedback ? feedback_ : L"Ctrl+Enter to save", RECT{0, area.bottom - px(57), area.right, area.bottom - px(34)},
                 small_font_, showing_feedback ? orange : subdued, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            return;
        }
        for (int y : {34, 126, 242}) fill(dc, RECT{0, px(y), area.right, px(y) + 1}, border);
        text(dc, L"HOTKEY", RECT{0, px(43), area.right, px(60)}, small_font_, muted, DT_SINGLELINE | DT_NOPREFIX);
        text(dc, hotkey_.label(), RECT{0, px(62), area.right - px(94), px(85)}, body_font_, foreground,
             DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (feedback_.empty() || GetTickCount64() >= feedback_until_)
            text(dc, L"Hotkey stays open until toggled.", RECT{0, px(96), area.right, px(121)}, small_font_, subdued,
                 DT_WORDBREAK | DT_NOPREFIX);
        else text(dc, feedback_, RECT{0, px(91), area.right, px(123)}, small_font_, orange,
                  DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        text(dc, L"WINDOW SIZE", RECT{0, px(139), area.right, px(157)}, small_font_, muted, DT_SINGLELINE | DT_NOPREFIX);
        const int field_width = (area.right - px(104)) / 2;
        text(dc, L"WIDTH", RECT{0, px(160), field_width, px(175)}, small_font_, subdued, DT_SINGLELINE | DT_NOPREFIX);
        text(dc, L"HEIGHT", RECT{field_width + px(12), px(160), area.right - px(92), px(175)}, small_font_, subdued,
             DT_SINGLELINE | DT_NOPREFIX);
        outline(dc, rect(0, px(175), field_width, px(28)), border);
        outline(dc, rect(field_width + px(12), px(175), field_width, px(28)), border);
        text(dc, L"900-3840 x 300-800", RECT{0, px(210), area.right - px(94), px(233)}, small_font_, subdued,
             DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    void layout_note_page(RECT area) {
        content_height_ = area.bottom;
        update_note_results();
        button("note-command-back", content_, L"< BACK", rect(0, px(2), px(64), px(22)), [this] { close_note_page(); });
        MoveWindow(note_query_edit_, px(5), px(39), area.right - px(10), px(22), FALSE);
        const int row_height = px(note_page_ == NotePage::search ? 74 : 58);
        note_result_rows_ = std::max(1, (static_cast<int>(area.bottom) - px(131)) / row_height);
        note_result_offset_ = std::clamp(note_result_offset_, 0,
            std::max(0, static_cast<int>(note_results_.size()) - note_result_rows_));
        note_results_bounds_ = RECT{0, px(96), area.right, px(96) + note_result_rows_ * row_height - px(8)};
        const int end = std::min(static_cast<int>(note_results_.size()), note_result_offset_ + note_result_rows_);
        for (int i = note_result_offset_; i < end; ++i) {
            const auto item = note_results_[static_cast<std::size_t>(i)];
            const auto id = note_result_id(item);
            const auto label = (note_page_ == NotePage::remove ? L"DELETE " :
                note_page_ == NotePage::trash ? L"RESTORE " : L"") + widen(get_string(item, "title"));
            const int row_top = note_results_bounds_.top + (i - note_result_offset_) * row_height;
            const int action_width = note_page_ == NotePage::trash ? px(115) : 0;
            button("note-result:" + id, content_, label,
                rect(0, row_top, area.right - px(8) - action_width, row_height - px(8)),
                [this, item] { activate_note_result(item); }, true, false, id == note_selected_id_, item);
            if (note_page_ == NotePage::trash)
                button("note-purge:" + id, content_, L"DELETE FOREVER",
                       rect(area.right - px(117), row_top, px(109), px(50)), [this, item] { purge_note(item); });
        }
        if (static_cast<int>(note_results_.size()) > note_result_rows_ ||
            (note_page_ == NotePage::search && (note_search_page_offset_ || note_search_next_))) {
            button("note-page-up", content_, L"UP", rect(area.right - px(110), area.bottom - px(24), px(45), px(22)),
                [this] { scroll_note_results(-note_result_rows_); }, note_result_offset_ > 0 ||
                    (note_page_ == NotePage::search && note_search_page_offset_));
            button("note-page-down", content_, L"DOWN", rect(area.right - px(58), area.bottom - px(24), px(58), px(22)),
                [this] { scroll_note_results(note_result_rows_); }, end < static_cast<int>(note_results_.size()) ||
                    (note_page_ == NotePage::search && note_search_next_.has_value()));
        }
    }
    void layout_detail(RECT area) {
        if (detail_ == Detail::note) { layout_note(area); return; }
        content_height_ = area.bottom;
        button("back", content_, L"< BACK", rect(0, px(2), px(68), px(22)), [this] { leave_detail(); });
        button("delete", content_, L"DELETE", rect(area.right - px(72), px(2), px(72), px(22)), [this] { delete_detail(); });
        int title_y = px(39);
        MoveWindow(title_edit_, px(1), title_y, area.right - px(2), px(28), FALSE);
        ShowWindow(title_edit_, SW_SHOW);
        int body_y = detail_ == Detail::note ? px(114) : px(81);
        note_body_y_ = body_y;
        const int compact = area.bottom < px(260) ? px(9) : 0;
        int body_bottom = detail_ == Detail::task ? area.bottom - px(130) + compact : area.bottom - px(39);
        MoveWindow(body_edit_, px(1), body_y, area.right - px(10), std::max(px(18), body_bottom - body_y), FALSE);
        ShowWindow(body_edit_, tag_picker_open_ ? SW_HIDE : SW_SHOW);
        if (detail_ == Detail::note) {
            button("note-tags", content_, L"TAGS", rect(0, px(80), px(64), px(23)),
                [this] { open_tag_picker(); }, true, false, tag_picker_open_);
            if (tag_picker_open_) layout_tag_picker(area);
        }
        if (detail_ == Detail::task) {
            int half = (area.right - px(12)) / 2;
            MoveWindow(operator_edit_, 0, area.bottom - px(99) + compact, half, px(23), FALSE);
            MoveWindow(project_edit_, half + px(12), area.bottom - px(99) + compact, half, px(23), FALSE);
            ShowWindow(operator_edit_, SW_SHOW);
            ShowWindow(project_edit_, SW_SHOW);
            const char* values[] = {"todo", "in_progress", "done"};
            const wchar_t* labels[] = {L"TO DO", L"IN PROGRESS", L"DONE"};
            int button_width = (area.right - px(12)) / 3;
            for (int i = 0; i < 3; ++i) {
                std::string value = values[i];
                button("status" + std::to_string(i), content_, labels[i],
                    rect(i * (button_width + px(6)), area.bottom - px(64), button_width, px(22)),
                    [this, value] { detail_status_ = value; detail_dirty_ = true; save_detail(); layout(); },
                    true, false, detail_status_ == value);
            }
        }
        button("save", content_, L"SAVE", rect(area.right - px(72), area.bottom - px(27), px(72), px(23)),
            [this] { if (save_detail()) feedback(L"Saved."); });
    }
    void layout_note(RECT area) {
        button("back", content_, L"< BACK", rect(0, px(2), px(64), px(22)), [this] { leave_detail(); });
        button("note-tags", content_, L"TAGS", rect(area.right - px(112), px(2), px(51), px(22)),
               [this] { open_tag_picker(); }, true, false, tag_picker_open_);
        button("note-edit", content_, L"EDIT", rect(area.right - px(53), px(2), px(53), px(22)),
               [this] { open_note_metadata(); }, !tag_picker_open_, false, note_metadata_open_);
        ShowWindow(composer_edit_, !tag_picker_open_ && !note_metadata_open_ ? SW_SHOW : SW_HIDE);
        if (title_edit_) ShowWindow(title_edit_, note_metadata_open_ && !tag_picker_open_ ? SW_SHOW : SW_HIDE);
        if (body_edit_) ShowWindow(body_edit_, note_metadata_open_ && !tag_picker_open_ ? SW_SHOW : SW_HIDE);
        if (tag_picker_open_) { layout_tag_picker(area); return; }
        if (note_metadata_open_) {
            MoveWindow(title_edit_, px(1), px(58), area.right - px(2), px(27), FALSE);
            MoveWindow(body_edit_, px(1), px(116), area.right - px(10), std::max(px(60), static_cast<int>(area.bottom) - px(161)), FALSE);
            button("delete", content_, L"DELETE NOTE", rect(0, area.bottom - px(27), px(103), px(23)), [this] { delete_detail(); });
            button("save", content_, L"SAVE", rect(area.right - px(72), area.bottom - px(27), px(72), px(23)),
                   [this] { if (save_detail()) { open_note_metadata(); feedback(L"Note details saved."); } });
            return;
        }
        const auto& composer_images = composer_draft().images;
        if (note_find_open_) {
            MoveWindow(note_find_edit_, px(5), px(41), area.right - px(147), px(22), FALSE);
            button("note-find-prev", content_, L"PREV", rect(area.right - px(135), px(40), px(42), px(24)),
                   [this] { move_note_find(-1); }, !note_find_results_.empty());
            button("note-find-next", content_, L"NEXT", rect(area.right - px(89), px(40), px(42), px(24)),
                   [this] { move_note_find(1); }, !note_find_results_.empty());
            button("note-find-close", content_, L"DONE", rect(area.right - px(43), px(40), px(43), px(24)),
                   [this] { close_note_find(); });
        }
        const int history_top = px(note_find_open_ ? 93 : 35);
        // Keep a usable composer when Find, attachments and the header strips
        // share the minimum-height popup. Full-size previews return as space allows.
        const int available = static_cast<int>(area.bottom) - history_top - px(37);
        const int attachments = composer_images.empty() ? 0 : std::clamp(available - px(59), px(27), px(55));
        const int composer_height = std::clamp(available - attachments - px(24), px(35), px(57));
        note_history_bounds_ = RECT{0, history_top, area.right,
            static_cast<int>(area.bottom) - px(37) - composer_height - attachments};
        note_composer_bounds_ = RECT{0, note_history_bounds_.bottom + px(8) + attachments, area.right, area.bottom - px(29)};
        note_attachment_bounds_ = RECT{0, note_history_bounds_.bottom + px(8), area.right, note_history_bounds_.bottom + attachments};
        MoveWindow(composer_edit_, px(5), note_composer_bounds_.top + px(5), area.right - px(16),
                   std::max(px(25), static_cast<int>(note_composer_bounds_.bottom - note_composer_bounds_.top) - px(10)), FALSE);
        const int send_width = note_edit_ ? px(88) : px(62);
        button("note-send", content_, note_edit_ ? L"SAVE EDIT" : L"SEND",
               rect(area.right - send_width, area.bottom - px(23), send_width, px(22)),
               [this] { send_note_message(); });
        if (note_edit_)
            button("note-edit-cancel", content_, L"CANCEL", rect(area.right - send_width - px(74), area.bottom - px(23), px(66), px(22)),
                   [this] { cancel_note_message_edit(); });
        const int attachment_height = std::max(px(14), static_cast<int>(note_attachment_bounds_.bottom - note_attachment_bounds_.top) - px(5));
        int attachment_width = std::min({px(45), static_cast<int>(area.right) / 8, attachment_height + px(3)});
        for (std::size_t i = 0; i < composer_images.size(); ++i) {
            const auto name = composer_images[i];
            int left = static_cast<int>(i) * (attachment_width + px(3));
            button("attachment-remove:" + name, content_, L"x", rect(left + attachment_width - px(13), note_attachment_bounds_.top, px(13), px(14)),
                   [this, name] { remove_pending_image(name); });
        }
        if (note_layout_dirty_ || note_layout_width_ != area.right || note_layout_dpi_ != dpi_) {
            note_rows_.clear();
            int y = note_offset_ ? px(36) : px(8);
            for (std::size_t i = 0; i < note_messages_.size(); ++i) {
                const auto& message = note_messages_[i];
                auto value = widen(get_string(message, "text"));
                int measured = value.empty() ? 0 : text_height(value, area.right - px(32), body_font_) + px(9);
                int height = px(23) + measured + static_cast<int>(message.at("images").size()) * px(160) + px(18);
                note_rows_.push_back({i, y, height, measured});
                y += height;
            }
            note_history_height_ = y;
            note_layout_width_ = area.right;
            note_layout_dpi_ = dpi_;
            note_layout_dirty_ = false;
        }
        note_scroll_ = std::clamp(note_scroll_, 0, note_scroll_limit());
        if (note_offset_ && note_scroll_ < px(24)) {
            button("note-earlier", content_, L"LOAD EARLIER", rect(px(8), note_history_bounds_.top + px(3), px(121), px(22)),
                   [this] { load_earlier_messages(); });
        }
        cache_note_images();
    }

    void build_cards() {
        std::map<std::string, Card> by_chat;
        std::map<std::string, std::int64_t> other_activity;
        auto card_for = [&](const Json& item) -> Card& {
            auto& card = by_chat[get_string(item, "chat")];
            card.chat = get_string(item, "chat");
            return card;
        };
        for (const auto& request : snapshot_["requests"]) {
            if (request.value("archived", false)) continue;
            auto& card = card_for(request);
            const auto created = request.value("created_at", std::int64_t(0));
            const auto resolved = request.value("resolved_at", created);
            auto& latest = other_activity[card.chat];
            latest = std::max({latest, created, resolved});
            if (get_string(request, "status") == "pending") card.pending.push_back(request);
            else if (card.answered.is_null() || resolved >= card.answered.value("resolved_at", std::int64_t(0)))
                card.answered = request;
        }
        for (const auto& job : snapshot_["jobs"]) {
            if (job.value("archived", false)) continue;
            auto& card = card_for(job);
            const auto created = job.value("created_at", std::int64_t(0));
            auto& latest = other_activity[card.chat];
            latest = std::max({latest, created, job.value("finished_at", created)});
            if (get_string(job, "status") == "running") card.running = true;
            if (card.job.is_null() || created >= card.job.value("created_at", std::int64_t(0))) card.job = job;
        }
        for (const auto& done : snapshot_.value("inbox", Json::array())) {
            if (done.value("archived", false)) continue;
            card_for(done).done = done;
        }
        cards_.clear();
        for (auto& [chat, card] : by_chat) {
            (void)chat;
            const auto others = other_activity[card.chat];
            const auto finished = card.done.is_null() ? std::int64_t(0) : card.done.value("finished_at", std::int64_t(0));
            card.activity = std::max(others, finished);
            std::sort(card.pending.begin(), card.pending.end(), [](const Json& a, const Json& b) {
                return a.value("created_at", std::int64_t(0)) < b.value("created_at", std::int64_t(0));
            });
            // Finishing wins over older activity; anything newer means the job
            // carried on after reporting, so the card goes back to work.
            card.state = !card.pending.empty() ? "waiting" : !card.done.is_null() && finished >= others ? "done" : "working";
            cards_.push_back(std::move(card));
        }
        auto rank = [](const Card& card) { return card.state == "waiting" ? 0 : card.state == "working" ? 1 : 2; };
        std::stable_sort(cards_.begin(), cards_.end(), [&](const Card& a, const Card& b) {
            if (rank(a) != rank(b)) return rank(a) < rank(b);
            return a.activity > b.activity;
        });
    }
    int card_index(const std::string& id) const {
        for (std::size_t i = 0; i < cards_.size(); ++i) {
            const auto& card = cards_[i];
            if (card.chat == id || get_string(card.job, "id") == id || get_string(card.done, "id") == id ||
                get_string(card.answered, "id") == id) return static_cast<int>(i);
            for (const auto& request : card.pending) if (get_string(request, "id") == id) return static_cast<int>(i);
        }
        return -1;
    }
    void scroll_to_card(const std::string& id) {
        build_cards();
        const int index = card_index(id);
        if (index >= 0 && card_width_) {
            RECT area{};
            GetClientRect(content_, &area);
            const int left = index * (card_width_ + card_gap_);
            if (left < cards_scroll_) cards_scroll_ = left;
            else if (left + card_width_ > cards_scroll_ + area.right) cards_scroll_ = left + card_width_ - area.right;
        }
        layout();
    }
    CardGeometry card_geometry(int x, int height) const {
        CardGeometry g{};
        const int w = card_width_;
        g.tag = RECT{x, 0, x + w, px(20)};
        g.close = RECT{x + w - px(22), 0, x + w, px(22)};
        const int name_h = pixel_glyph_height * pixel_scale(2.0) + px(12);
        g.name = RECT{x, px(26), x + w, px(26) + name_h};
        g.actions = RECT{x, height - px(26), x + w, height};
        const int box_h = std::clamp(height * 30 / 100, px(70), px(130));
        g.box = RECT{x, g.actions.top - px(10) - box_h, x + w, g.actions.top - px(10)};
        g.art = RECT{x + px(6), g.name.bottom + px(6), x + w - px(6), g.box.top - px(6)};
        return g;
    }
    static std::wstring request_outcome(const Json& request) {
        const auto status = get_string(request, "status");
        const bool question = get_string(request, "kind") == "question";
        if (status == "approved") return question ? L"ANSWERED YES" : L"YES - INPUT GRANTED";
        if (status == "denied") return L"ANSWERED NO";
        if (status == "custom") return L"REPLIED";
        if (status == "expired") return L"EXPIRED WITHOUT AN ANSWER";
        if (status == "released") return L"INPUT RELEASED";
        return L"CANCELLED";
    }
    // A working card shows its newest event: a command result or an answer.
    static bool card_shows_finished_job(const Card& card) {
        if (card.job.is_null() || card.running || get_string(card.job, "status") == "running") return false;
        const auto job_time = std::max(card.job.value("created_at", std::int64_t(0)), card.job.value("finished_at", std::int64_t(0)));
        return card.answered.is_null() || job_time >= card.answered.value("resolved_at", std::int64_t(0));
    }
    // The text shown in a card's box, and the small line beneath it.
    std::pair<std::wstring, std::wstring> card_text(const Card& card) const {
        if (card.state == "waiting") {
            const auto& request = card.pending.front();
            std::wstring meta = get_string(request, "kind") == "question" ? L"QUESTION" :
                L"INPUT LEASE " + std::to_wstring(std::max<std::int64_t>(1, request.value("lease_seconds", 300) / 60)) + L" MIN";
            if (card.pending.size() > 1) meta += L"  +" + std::to_wstring(card.pending.size() - 1) + L" MORE";
            return {widen(get_string(request, "message")), meta};
        }
        if (card.state == "done") {
            const auto report = get_string(card.done, "report");
            return {widen(get_string(card.done, "summary")), report.empty() ? L"NO REPORT" : L"REPORT " + widen(report)};
        }
        const auto job_time = card.job.is_null() ? std::int64_t(0) :
            std::max(card.job.value("created_at", std::int64_t(0)), card.job.value("finished_at", std::int64_t(0)));
        const auto answer_time = card.answered.is_null() ? std::int64_t(0) : card.answered.value("resolved_at", std::int64_t(0));
        if (!card.job.is_null() && (card.running || job_time >= answer_time)) {
            const auto status = get_string(card.job, "status");
            if (status == "running")
                return {widen(get_string(card.job, "title")), L"RUNNING SINCE " + display_time(card.job.value("created_at", std::int64_t(0)))};
            if (status == "notice") return {widen(get_string(card.job, "title")), L"NOTICE"};
            std::wstring body = widen(get_string(card.job, "title")) + (status == "completed" ? L" finished." : L" failed.");
            const auto tail = get_string(card.job, "output_tail");
            if (!tail.empty()) body += L"\n" + shortened(tail, 300, true);
            return {body, L"EXIT " + std::to_wstring(card.job.value("exit_code", 0))};
        }
        if (!card.answered.is_null()) {
            std::wstring body = widen(get_string(card.answered, "message"));
            if (get_string(card.answered, "status") == "custom")
                body += L"\n\n> " + widen(get_string(card.answered, "response"));
            return {body, request_outcome(card.answered)};
        }
        return {L"", L""};
    }
    void dismiss_card(const Card& card) {
        // Hide everything finished for this chat; pending and running items stay.
        for (const auto& request : snapshot_["requests"])
            if (get_string(request, "chat") == card.chat && get_string(request, "status") != "pending" && !request.value("archived", false))
                mutate({{"op", "item.dismiss"}, {"kind", "request"}, {"id", request["id"]}});
        for (const auto& job : snapshot_["jobs"])
            if (get_string(job, "chat") == card.chat && get_string(job, "status") != "running" && !job.value("archived", false))
                mutate({{"op", "item.dismiss"}, {"kind", "job"}, {"id", job["id"]}});
        if (!card.done.is_null()) mutate({{"op", "item.dismiss"}, {"kind", "inbox"}, {"id", card.done["id"]}});
    }
    void layout_dashboard(RECT area) {
        build_cards();
        content_height_ = area.bottom;
        card_gap_ = px(16);
        const int count = std::max(1, (static_cast<int>(area.right) + card_gap_) / (px(230) + card_gap_));
        card_width_ = std::min(px(290), (static_cast<int>(area.right) - (count - 1) * card_gap_) / count);
        const int n = static_cast<int>(cards_.size());
        cards_extent_ = n ? n * card_width_ + (n - 1) * card_gap_ : 0;
        cards_scroll_ = std::clamp(cards_scroll_, 0, std::max(0, cards_extent_ - static_cast<int>(area.right)));
        const int height = area.bottom - px(12);
        bool reply_found = false;
        for (int i = 0; i < n; ++i) {
            const auto& card = cards_[static_cast<std::size_t>(i)];
            const int x = i * (card_width_ + card_gap_) - cards_scroll_;
            const auto g = card_geometry(x, height);
            const int w = card_width_;
            const auto key = card.chat;
            auto third = [&](int slot) {
                const int each = (w - 2 * px(8)) / 3;
                return rect(x + slot * (each + px(8)), g.actions.top, slot == 2 ? w - 2 * (each + px(8)) : each, px(24));
            };
            if (card.state == "waiting") {
                const auto request = card.pending.front();
                const auto id = get_string(request, "id");
                if (id == reply_id_ && reply_edit_) {
                    reply_found = true;
                    MoveWindow(reply_edit_, g.box.left + px(6), g.box.top + px(6), w - px(12),
                               g.box.bottom - g.box.top - px(12), FALSE);
                    ShowWindow(reply_edit_, SW_SHOW);
                    button(id + "send", content_, L"SEND", third(0), [this] { send_reply(); });
                    button(id + "saved-replies", content_, L"SAVED", third(1), [this] { use_saved_reply(); });
                    button(id + "cancel", content_, L"CANCEL", third(2), [this] { cancel_reply(); });
                } else {
                    const bool available = get_string(request, "kind") != "input" || snapshot_["lease"].is_null();
                    button(id + "yes", content_, L"YES", third(0), [this, id] { resolve(id, "approved"); }, available);
                    button(id + "custom", content_, L"CUSTOM", third(1), [this, id] { begin_reply(id); });
                    button(id + "no", content_, L"NO", third(2), [this, id] { resolve(id, "denied"); });
                }
                continue;
            }
            if (!card.running) button("card-dismiss:" + key, content_, L"DISMISS", g.close, [this, card] { dismiss_card(card); });
            if (card.state == "done") {
                const auto text = report_clipboard_text(get_string(card.done, "summary"), get_string(card.done, "report"));
                button("card-copy-report:" + key, content_, L"COPY REPORT", rect(x, g.actions.top, w, px(24)),
                       [this, text] { copy(text, L"Summary and report path copied."); });
                continue;
            }
            const auto& lease = snapshot_["lease"];
            if (!card.answered.is_null() && !lease.is_null() && lease.value("id", "") == get_string(card.answered, "id")) {
                const auto id = get_string(card.answered, "id");
                button("card-release:" + key, content_, L"RELEASE INPUT", rect(x, g.actions.top, w, px(24)),
                       [this, id] { mutate({{"op", "request.release"}, {"id", id}}); });
                continue;
            }
            if (card_shows_finished_job(card)) {
                auto resume = get_string(card.job, "resume_text");
                if (resume.empty()) resume = "@" + card.chat + ": " + get_string(card.job, "title") + ". Continue from this result.";
                const auto path = get_string(card.job, "log_path");
                const int half = (w - px(8)) / 2;
                button("card-copy:" + key, content_, L"COPY", path.empty() ? rect(x, g.actions.top, w, px(24)) :
                       rect(x, g.actions.top, half, px(24)), [this, resume] { copy(resume); });
                if (!path.empty()) button("card-log:" + key, content_, L"VIEW LOG",
                       rect(x + half + px(8), g.actions.top, w - half - px(8), px(24)), [this, path] { view_log(path); });
            }
        }
        if (reply_edit_ && !reply_found) ShowWindow(reply_edit_, SW_HIDE);
    }

    void draw_root(HDC dc) {
        RECT area{};
        GetClientRect(window_, &area);
        fill(dc, area, frame);
        // The working area (cards, status and notes) sits on a lighter panel.
        fill(dc, RECT{middle_rect_.left - px(10), 1, arrow_rect_.left, area.bottom - 1}, background);
        fill(dc, RECT{middle_rect_.left - px(10), px(10), middle_rect_.left - px(10) + 1, area.bottom - px(10)}, divider);
        fill(dc, RECT{arrow_rect_.left, 1, arrow_rect_.left + 1, area.bottom - 1}, divider);
        outline(dc, area, RGB(56, 56, 56));

        const int heading = pixel_scale(2.0);
        const RECT tasks_heading{tasks_rect_.left, tasks_rect_.top, tasks_rect_.left + pixel_width(L"Task List", heading) + 2 * px(8),
                                 tasks_rect_.top + px(28)};
        fill(dc, tasks_heading, well);
        pixel_text(dc, L"Task List", RECT{tasks_heading.left + px(8), tasks_heading.top, tasks_heading.right, tasks_heading.bottom},
                   heading, RGB(200, 200, 194), false);
        fill(dc, task_entry_bounds_, well);

        text(dc, L"MESSAGE (OPTIONAL)", RECT{presence_rect_.left, presence_box_.top - px(18), presence_rect_.right, presence_box_.top - px(2)},
             small_font_, subdued, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        fill(dc, presence_box_, well);
        if (get_string(presence(), "state", "unknown") == "unknown")
            text(dc, L"Pick a status so agents know whether you are at the PC.",
                 RECT{presence_rect_.left, presence_box_.bottom + px(8), presence_rect_.right, presence_box_.bottom + px(44)},
                 small_font_, muted, DT_WORDBREAK | DT_NOPREFIX);
        if (lease_line_y_) {
            text(dc, L"INPUT IN USE", RECT{presence_rect_.left, lease_line_y_, presence_rect_.right - px(82), lease_line_y_ + px(22)},
                 small_font_, orange, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            text(dc, L"@" + widen(get_string(snapshot_["lease"], "chat")),
                 RECT{presence_rect_.left, lease_line_y_ + px(22), presence_rect_.right, lease_line_y_ + px(40)},
                 small_font_, muted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if (quiet_line_y_) text(dc, busy_caption(), RECT{presence_rect_.left, quiet_line_y_, presence_rect_.right - px(58), quiet_line_y_ + px(22)},
                                small_font_, muted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (!window_settings_.notes_collapsed()) {
            pixel_text(dc, L"NOTES", RECT{notes_rect_.left, notes_rect_.top, notes_rect_.right, notes_rect_.top + px(30)},
                       heading, RGB(200, 200, 194), false);
            rounded(dc, search_bounds_, search_fill, search_bounds_.bottom - search_bounds_.top);
            // A small magnifier drawn with lines.
            const int cx = search_bounds_.left + px(11), cy = search_bounds_.top + px(11);
            HPEN pen = CreatePen(PS_SOLID, std::max(1, px(1)), RGB(170, 170, 166));
            auto old_pen = SelectObject(dc, pen);
            auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, cx - px(5), cy - px(5), cx + px(4), cy + px(4));
            MoveToEx(dc, cx + px(3), cy + px(3), nullptr);
            LineTo(dc, cx + px(7), cy + px(7));
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
            if (filter_line_y_) {
                auto label = untagged_filter_ ? L"Showing untagged notes" : L"Showing #" + widen(note_filter_);
                text(dc, label, RECT{notes_rect_.left, filter_line_y_, notes_rect_.right - px(64), filter_line_y_ + px(20)},
                     small_font_, muted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
        if (note_command_bounds_.bottom > note_command_bounds_.top) {
            fill(dc, note_command_bounds_, well);
            outline(dc, note_command_bounds_, border);
            if (note_command_choices_.empty()) {
                auto bounds = note_command_bounds_;
                InflateRect(&bounds, -px(8), -px(6));
                text(dc, L"No matching command.\nType / to see the available commands.", bounds, small_font_, muted);
            } else if (note_command_visible_ < note_command_choices_.size()) {
                const int height = note_command_bounds_.bottom - note_command_bounds_.top - px(8);
                const int thumb = std::max(px(8), height * static_cast<int>(note_command_visible_) /
                    static_cast<int>(note_command_choices_.size()));
                const int top = note_command_bounds_.top + px(4) + (height - thumb) * static_cast<int>(note_command_first_) /
                    static_cast<int>(note_command_choices_.size() - note_command_visible_);
                fill(dc, RECT{note_command_bounds_.right - px(4), top,
                    note_command_bounds_.right - px(4) + std::max(1, px(1)), top + thumb}, muted);
            }
        }
        if (note_undo_bounds_.bottom > note_undo_bounds_.top) {
            auto bounds = note_undo_bounds_;
            bounds.right -= px(63);
            text(dc, L"Deleted \"" + widen(note_undo_title_) + L"\"", bounds, small_font_, muted,
                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    void draw_content(HDC dc) {
        RECT area{};
        GetClientRect(content_, &area);
        fill(dc, area, background);
        if (settings_open_) draw_settings(dc, area);
        else if (detail_ != Detail::none) draw_detail(dc, area);
        else if (note_page_visible()) draw_note_page(dc, area);
        else if (tag_picker_open_) draw_tag_picker(dc);
        else draw_dashboard(dc, area);
        if (!settings_open_ && !feedback_.empty() && GetTickCount64() < feedback_until_) {
            RECT box{px(3), area.bottom - px(46), area.right - px(3), area.bottom - px(3)};
            if (detail_ == Detail::note && !note_metadata_open_ && !tag_picker_open_)
                box = RECT{px(3), std::max(note_history_bounds_.top, note_history_bounds_.bottom - px(46)),
                           area.right - px(3), note_history_bounds_.bottom - px(3)};
            else if (note_page_visible())
                box = RECT{px(3), area.bottom - px(65), area.right - px(3), area.bottom - px(30)};
            fill(dc, box, top_background);
            outline(dc, box, border);
            InflateRect(&box, -px(6), -px(5));
            text(dc, feedback_, box, small_font_, foreground, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    void draw_note_page(HDC dc, RECT area) const {
        text(dc, widen(note_command_name(note_page_)), RECT{px(77), px(2), area.right, px(24)},
            body_font_, foreground, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        outline(dc, RECT{0, px(35), area.right, px(65)}, border);
        text(dc, note_page_ == NotePage::remove ? L"Click a note to move it to Trash." :
            note_page_ == NotePage::trash ? L"Restore, or delete forever." :
            note_page_ == NotePage::search ? L"Search entries. Enter jumps to a match." : L"Search names. Enter opens a note.",
            RECT{0, px(72), area.right - px(94), px(89)}, small_font_, note_page_ == NotePage::remove ? orange : muted,
            DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        const auto total_results = note_page_ == NotePage::search ? note_search_total_ : note_results_.size();
        text(dc, note_search_pending_ && note_page_ == NotePage::search ? L"Searching..." :
            std::to_wstring(total_results) + (total_results == 1 ? L" match" : L" matches"),
            RECT{area.right - px(88), px(72), area.right, px(89)}, small_font_, muted,
            DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        if (note_results_.empty()) {
            const bool empty = snapshot_.value(note_page_ == NotePage::trash ? "trash" : "notes", Json::array()).empty();
            const bool searching = note_page_ == NotePage::search;
            const bool blank = searching && edit_text(note_query_edit_).empty();
            const auto heading = searching && note_search_pending_ ? L"Searching your entries..." :
                blank ? L"Find something you wrote." :
                empty ? (note_page_ == NotePage::trash ? L"Trash is empty." : L"No notes yet.") :
                searching ? L"No matching entries." : L"No matching notes.";
            text(dc, heading,
                RECT{px(8), px(115), area.right - px(8), px(142)}, title_font_);
            text(dc, searching ? L"Type a word, phrase, or part of a link." :
                empty ? (note_page_ == NotePage::trash ? L"Deleted notes stay here until their retention period ends." :
                L"Go back to create a note.") : L"Try another part of the name.",
                RECT{px(8), px(147), area.right - px(8), px(188)}, body_font_, muted);
        }
        if (static_cast<int>(note_results_.size()) > note_result_rows_) {
            const int height = note_results_bounds_.bottom - note_results_bounds_.top;
            const int total = static_cast<int>(note_results_.size());
            const int thumb = std::max(px(20), height * note_result_rows_ / total);
            const int top = note_results_bounds_.top + (height - thumb) * note_result_offset_ / (total - note_result_rows_);
            fill(dc, RECT{area.right - std::max(1, px(1)), top, area.right, top + thumb}, border);
        }
        if (total_results > static_cast<std::size_t>(note_result_rows_)) {
            const auto start = note_page_ == NotePage::search ? note_search_page_offset_ : 0;
            const auto first = std::to_wstring(start + note_result_offset_ + 1);
            const auto last = std::to_wstring(start + std::min(static_cast<int>(note_results_.size()), note_result_offset_ + note_result_rows_));
            text(dc, first + L" - " + last + L" of " + std::to_wstring(total_results),
                RECT{0, area.bottom - px(24), area.right - px(123), area.bottom - px(2)}, small_font_, subdued,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    struct EditorScroll { RECT bounds{}; int first = 0; int total = 0; int visible = 0; };
    EditorScroll editor_scroll(HWND editor) const {
        EditorScroll info;
        GetWindowRect(editor, &info.bounds);
        MapWindowPoints(nullptr, content_, reinterpret_cast<POINT*>(&info.bounds), 2);
        HDC dc = GetDC(editor);
        auto old = SelectObject(dc, body_font_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, old);
        ReleaseDC(editor, dc);
        info.visible = std::max(1, static_cast<int>((info.bounds.bottom - info.bounds.top) / std::max(1L, metrics.tmHeight)));
        info.total = static_cast<int>(SendMessageW(editor, EM_GETLINECOUNT, 0, 0));
        info.first = static_cast<int>(SendMessageW(editor, EM_GETFIRSTVISIBLELINE, 0, 0));
        return info;
    }
    void draw_editor_scroll(HDC dc, HWND editor) const {
        if (!editor || !IsWindowVisible(editor)) return;
        auto info = editor_scroll(editor);
        if (info.total <= info.visible) return;
        int height = info.bounds.bottom - info.bounds.top;
        int thumb = std::max(px(20), height * info.visible / info.total);
        int top = info.bounds.top + (height - thumb) * std::min(info.first, info.total - info.visible) / std::max(1, info.total - info.visible);
        fill(dc, RECT{info.bounds.right + px(5), top, info.bounds.right + px(5) + std::max(1, px(1)), top + thumb}, muted);
    }
    bool editor_gutter(HWND editor, int x, int y) const {
        if (!editor || !IsWindowVisible(editor)) return false;
        auto info = editor_scroll(editor);
        return info.total > info.visible && x >= info.bounds.right && x <= info.bounds.right + px(9)
            && y >= info.bounds.top && y <= info.bounds.bottom;
    }
    void scroll_editor(HWND editor, int y) {
        auto info = editor_scroll(editor);
        int range = std::max(0, info.total - info.visible);
        int target = std::clamp((y - static_cast<int>(info.bounds.top)) * range /
            std::max(1, static_cast<int>(info.bounds.bottom - info.bounds.top)), 0, range);
        SendMessageW(editor, EM_LINESCROLL, 0, target - info.first);
        InvalidateRect(content_, nullptr, FALSE);
    }
    void draw_detail(HDC dc, RECT area) {
        if (detail_ == Detail::note) { draw_note(dc, area); return; }
        text(dc, detail_ == Detail::note ? L"NOTE" : L"TASK", RECT{px(82), px(3), area.right - px(82), px(23)},
             small_font_, muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        outline(dc, RECT{0, px(38), area.right, px(68)}, border);
        const int compact = area.bottom < px(260) ? px(9) : 0;
        int body_bottom = detail_ == Detail::task ? area.bottom - px(130) + compact : area.bottom - px(39);
        if (detail_ == Detail::note) {
            auto tags = tag_caption(detail_tags_);
            text(dc, tags.empty() ? L"Add tags" : tags,
                 RECT{px(74), px(80), area.right - px(5), px(103)}, body_font_, tags.empty() ? subdued : muted,
                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if (tag_picker_open_) draw_tag_picker(dc);
        else {
            outline(dc, RECT{0, note_body_y_ - 1, area.right, body_bottom + 1}, border);
            draw_editor_scroll(dc, body_edit_);
        }
        if (detail_ == Detail::task) {
            int half = (area.right - px(12)) / 2;
            text(dc, L"OPERATOR", RECT{0, area.bottom - px(117) + compact, half, area.bottom - px(100) + compact}, small_font_, muted);
            text(dc, L"PROJECT", RECT{half + px(12), area.bottom - px(117) + compact, area.right, area.bottom - px(100) + compact}, small_font_, muted);
            fill(dc, RECT{0, area.bottom - px(75) + compact, half, area.bottom - px(75) + compact + 1}, border);
            fill(dc, RECT{half + px(12), area.bottom - px(75) + compact, area.right, area.bottom - px(75) + compact + 1}, border);
        }
        text(dc, detail_dirty_ ? L"Saving..." : L"Saved locally. Ctrl+Enter to save.",
            RECT{0, area.bottom - px(26), area.right - px(81), area.bottom - px(5)}, small_font_, subdued,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    void draw_note(HDC dc, RECT area) {
        text(dc, widen(detail_original_title_), RECT{px(74), px(2), area.right - px(120), px(24)},
             body_font_, foreground, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        fill(dc, RECT{0, px(31), area.right, px(31) + 1}, border);
        if (tag_picker_open_) { draw_tag_picker(dc); return; }
        if (note_metadata_open_) {
            text(dc, L"TITLE", rect(0, px(40), area.right, px(15)), small_font_, muted);
            outline(dc, RECT{0, px(57), area.right, px(86)}, border);
            text(dc, L"DESCRIPTION", rect(0, px(97), area.right, px(15)), small_font_, muted);
            outline(dc, RECT{0, px(115), area.right, area.bottom - px(44)}, border);
            draw_editor_scroll(dc, body_edit_);
            return;
        }
        if (note_find_open_) {
            outline(dc, RECT{0, px(37), area.right - px(142), px(66)}, border);
            const auto status = note_find_pending_ ? L"Searching..." : edit_text(note_find_edit_).empty() ? L"Find text in this note" :
                note_find_results_.empty() ? L"No matching entries" :
                std::to_wstring(note_find_selection_ + 1) + L" / " + std::to_wstring(note_find_results_.size()) + L" matching entries";
            text(dc, status, RECT{px(3), px(71), area.right - px(3), px(88)}, small_font_, muted,
                 DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, note_history_bounds_.left, note_history_bounds_.top, note_history_bounds_.right, note_history_bounds_.bottom);
        for (const auto& row : note_rows_) {
            int y = note_history_bounds_.top + row.top - note_scroll_;
            if (y + row.height < note_history_bounds_.top || y > note_history_bounds_.bottom) continue;
            const auto& entry = note_messages_[row.index];
            if (get_string(entry, "id") == note_highlight_id_ && GetTickCount64() < note_highlight_until_) {
                fill(dc, RECT{px(3), y - px(3), area.right - px(5), y + row.height - px(5)}, top_background);
                fill(dc, RECT{px(3), y - px(3), px(5), y + row.height - px(5)}, muted);
            }
            text(dc, L">", rect(px(8), y, px(12), px(16)), body_font_, orange, DT_SINGLELINE);
            text(dc, entry.contains("edited_at") ? L"me · edited" : L"me",
                 rect(px(25), y, px(105), px(16)), small_font_, muted, DT_SINGLELINE | DT_VCENTER);
            text(dc, display_time(entry.value("created_at", std::int64_t(0))),
                 RECT{area.right - px(110), y, area.right - px(12), y + px(16)}, small_font_, subdued,
                 DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            text(dc, widen(get_string(entry, "text")), rect(px(16), y + px(23), area.right - px(32), row.text_height), body_font_);
            int image_y = y + px(23) + row.text_height;
            for (const auto& name : entry.at("images")) {
                draw_note_image(dc, name.get<std::string>(), RECT{px(16), image_y, area.right - px(16), image_y + px(150)});
                image_y += px(160);
            }
        }
        const int height = note_history_bounds_.bottom - note_history_bounds_.top;
        if (note_history_height_ > height) {
            int thumb = std::max(px(18), height * height / note_history_height_);
            int top = note_history_bounds_.top + (height - thumb) * note_scroll_ / std::max(1, note_scroll_limit());
            fill(dc, RECT{area.right - std::max(1, px(1)), top, area.right, top + thumb}, border);
        }
        RestoreDC(dc, saved);
        outline(dc, note_composer_bounds_, border);
        draw_editor_scroll(dc, composer_edit_);
        const auto& images = composer_draft().images;
        if (!images.empty()) {
            const int attachment_height = std::max(px(14), static_cast<int>(note_attachment_bounds_.bottom - note_attachment_bounds_.top) - px(5));
            int attachment_width = std::min({px(45), static_cast<int>(area.right) / 8, attachment_height + px(3)});
            for (std::size_t i = 0; i < images.size(); ++i) {
                int left = static_cast<int>(i) * (attachment_width + px(3));
                draw_note_image(dc, images[i], RECT{left, note_attachment_bounds_.top,
                    left + attachment_width, note_attachment_bounds_.top + attachment_height});
            }
        }
        text(dc, note_edit_ ? L"Editing entry" : L"Enter to add  /  Shift+Enter for a new line",
             RECT{0, area.bottom - px(22), area.right - px(note_edit_ ? 174 : 73), area.bottom - px(1)}, small_font_, subdued,
             DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    void draw_dashboard(HDC dc, RECT area) {
        if (cards_.empty()) {
            const auto& character = character_for("KNOCK");
            draw_character(dc, character, RECT{area.right / 2 - px(80), px(20), area.right / 2 + px(80), px(150)});
            text(dc, L"No jobs yet.", RECT{0, px(165), area.right, px(190)}, title_font_, foreground,
                 DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
            text(dc, L"Agents appear here when they ask, run a command or finish.\nGive them knock info to get started.",
                 RECT{px(20), px(195), area.right - px(20), px(240)}, body_font_, muted, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
            return;
        }
        const int height = area.bottom - px(12);
        const int tag_scale = pixel_scale(1.2);
        const int base_name_scale = pixel_scale(2.0);
        for (std::size_t i = 0; i < cards_.size(); ++i) {
            const auto& card = cards_[i];
            const int x = static_cast<int>(i) * (card_width_ + card_gap_) - cards_scroll_;
            if (x + card_width_ < 0 || x > area.right) continue;
            const auto g = card_geometry(x, height);
            const bool waiting = card.state == "waiting", done = card.state == "done";
            const std::wstring tag = waiting ? L"WAITING" : done ? L"DONE" : L"WORKING";
            const int tag_w = pixel_width(tag, tag_scale) + 2 * px(7);
            const RECT tag_box{x + (card_width_ - tag_w) / 2, g.tag.top + px(2), x + (card_width_ + tag_w) / 2, g.tag.bottom};
            fill(dc, tag_box, waiting ? orange_fill : done ? green_fill : blue_fill);
            pixel_text(dc, tag, tag_box, tag_scale, waiting ? orange : done ? green : blue, true);
            const auto name = widen(card.chat);
            int name_scale = base_name_scale;
            while (name_scale > 1 && pixel_width(name, name_scale) > card_width_ - px(24)) --name_scale;
            const int name_w = std::min(card_width_, (pixel_printable(name) ? pixel_width(name, name_scale) : card_width_) + 2 * px(10));
            const RECT name_box{x + (card_width_ - name_w) / 2, g.name.top, x + (card_width_ + name_w) / 2, g.name.bottom};
            fill(dc, name_box, raised);
            pixel_text(dc, name, RECT{name_box.left + px(10), name_box.top, name_box.right - px(10), name_box.bottom},
                       name_scale, RGB(200, 200, 194), true);
            draw_character(dc, character_for(card.chat), g.art);
            rounded(dc, g.box, well, px(10));
            const bool replying = waiting && get_string(card.pending.front(), "id") == reply_id_ && reply_edit_;
            if (!replying) {
                const auto [body, meta] = card_text(card);
                RECT inner = g.box;
                InflateRect(&inner, -px(9), -px(7));
                const int meta_h = meta.empty() ? 0 : line_height(small_font_) + px(3);
                text(dc, body, RECT{inner.left, inner.top, inner.right, inner.bottom - meta_h}, small_font_, RGB(200, 200, 194),
                     DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
                if (!meta.empty())
                    text(dc, meta, RECT{inner.left, inner.bottom - meta_h + px(3), inner.right, inner.bottom}, small_font_,
                         done ? green : muted, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_PATH_ELLIPSIS);
            } else {
                draw_editor_scroll(dc, reply_edit_);
            }
        }
        if (cards_extent_ > area.right) {
            const int track = area.bottom - px(3);
            fill(dc, RECT{0, track, area.right, track + 1}, divider);
            const int thumb = std::max(px(30), static_cast<int>(area.right) * static_cast<int>(area.right) / cards_extent_);
            const int left = (area.right - thumb) * cards_scroll_ / std::max(1, cards_extent_ - static_cast<int>(area.right));
            fill(dc, RECT{left, track - 1, left + thumb, track + 2}, RGB(90, 90, 90));
        }
    }
    void paint(HWND target, bool content) {
        PAINTSTRUCT state{};
        HDC dc = BeginPaint(target, &state);
        RECT area{};
        GetClientRect(target, &area);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, area.right), std::max(1L, area.bottom));
        auto old = SelectObject(buffer, bitmap);
        std::exception_ptr error;
        try { if (content) draw_content(buffer); else draw_root(buffer); }
        catch (...) { error = std::current_exception(); }
        BitBlt(dc, 0, 0, area.right, area.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(target, &state);
        if (error) std::rethrow_exception(error);
    }
    void scroll_by(int amount) {
        if (settings_open_ || tag_picker_open_) return;
        if (note_page_visible()) {
            scroll_note_results(amount > 0 ? note_result_rows_ : -note_result_rows_);
            return;
        }
        if (detail_ == Detail::note && !note_metadata_open_) {
            const int next = std::clamp(note_scroll_ + amount, 0, note_scroll_limit());
            if (next != note_scroll_) { note_scroll_ = next; layout(); }
            return;
        }
        if (detail_ != Detail::none) return;
        RECT area{};
        GetClientRect(content_, &area);
        const int next = std::clamp(cards_scroll_ + amount, 0, std::max(0, cards_extent_ - static_cast<int>(area.right)));
        if (next == cards_scroll_) return;
        cards_scroll_ = next;
        layout();
    }
    void escape() {
        if (settings_open_) settings_back();
        else if (tag_picker_open_) close_tag_picker();
        else if (detail_ == Detail::note && note_metadata_open_) open_note_metadata();
        else if (detail_ == Detail::note && note_find_open_) close_note_find();
        else if (detail_ == Detail::note && note_edit_) cancel_note_message_edit();
        else if (reply_edit_ && dashboard()) cancel_reply();
        else if (detail_ != Detail::none) leave_detail();
        else if (note_page_visible()) close_note_page();
        else hide();
    }
    void changed(const Json& event) {
        snapshot_ = store_.snapshot();
        update_busy_mode();
        const auto operation = get_string(event, "operation");
        if (operation.starts_with("note.")) note_results_dirty_ = true;
        if (!note_undo_id_.empty()) {
            const auto trash = snapshot_.value("trash", Json::array());
            if (std::none_of(trash.begin(), trash.end(), [this](const auto& note) {
                    return get_string(note, "id") == note_undo_id_;
                })) note_undo_id_.clear();
        }
        bool follow_note = false;
        if (detail_ != Detail::none) {
            const auto& records = snapshot_[detail_ == Detail::task ? "tasks" : "notes"];
            auto current = std::find_if(records.begin(), records.end(), [this](const Json& item) {
                return get_string(item, "id") == detail_id_;
            });
            if (current != records.end() && detail_ == Detail::note) {
                detail_tags_ = tags_of(*current);
                if (get_string(event, "id") == detail_id_ && get_string(event, "operation").starts_with("note.")) {
                    follow_note = get_string(event, "operation") == "note.append" && !note_find_open_ &&
                        note_scroll_limit() - note_scroll_ <= px(24);
                    const auto count = current->value("message_count", std::size_t(0));
                    if (note_offset_ >= count) note_offset_ = count > 100 ? count - 100 : 0;
                    load_note_history();
                    if (note_find_open_ && !note_find_pending_) refresh_note_find(false);
                    if (note_metadata_open_ && !detail_dirty_) {
                        populating_ = true;
                        auto title = widen(get_string(note_record_, "title"));
                        auto body = widen(get_string(note_record_, "body"));
                        if (edit_text(title_edit_) != title) SetWindowTextW(title_edit_, title.c_str());
                        if (edit_text(body_edit_) != body) SetWindowTextW(body_edit_, body.c_str());
                        populating_ = false;
                    }
                }
            }
            if (current == records.end()) {
                if (detail_ == Detail::note && note_draft_dirty_ && !save_note_draft()) return;
                if (detail_dirty_) {
                    auto folder = data_directory() / L"drafts";
                    std::filesystem::create_directories(folder);
                    Json draft{{"title", narrow(edit_text(title_edit_))}, {"body", narrow(edit_text(body_edit_))}};
                    if (detail_ == Detail::note) draft["tags"] = detail_tags_;
                    std::ofstream file(folder / (detail_id_ + ".json"), std::ios::binary);
                    file << draft.dump(2);
                    file.flush();
                    if (!file) throw std::runtime_error("Could not preserve the draft of a removed item.");
                    feedback(L"Item removed by an agent. Your draft is in the data folder.");
                } else feedback(L"This item was removed by an agent.");
                detail_ = Detail::none;
                destroy_detail_edits();
            }
        }
        if (get_string(event, "type") == "request" || get_string(event, "type") == "job" ||
            get_string(event, "type") == "done") enqueue_banner(event);
        // Another change never replaces a status message being typed.
        if (presence_edit_ && !presence_dirty_ && GetFocus() != presence_edit_) {
            const auto message = widen(get_string(presence(), "message"));
            if (edit_text(presence_edit_) != message) {
                populating_ = true;
                SetWindowTextW(presence_edit_, message.c_str());
                populating_ = false;
            }
        }
        update_tray();
        layout();
        if (follow_note) scroll_note_to_end();
    }
    LRESULT command(WPARAM wparam) {
        int id = LOWORD(wparam);
        int event = HIWORD(wparam);
        if (event == BN_CLICKED) {
            auto found = buttons_.find(id);
            if (found != buttons_.end()) {
                // Copy the callback: it may rebuild and remove its own button.
                auto action = found->second.action;
                if (action) action();
            }
        }
        if ((event == EN_UPDATE || event == EN_VSCROLL) && (id == 32 || id == 35 || id == 37 || id == 44))
            InvalidateRect(content_, nullptr, FALSE);
        if (event == EN_CHANGE && !populating_ && id >= 31 && id <= 34) {
            detail_dirty_ = true;
            SetTimer(window_, 3, 600, nullptr);
        }
        if (event == EN_CHANGE && id == 36 && tag_picker_open_) {
            tag_offset_ = 0;
            layout();
        }
        if (event == EN_CHANGE && !populating_ && id == 37 && detail_ == Detail::note) {
            composer_draft().client_id.clear();
            note_draft_dirty_ = true;
            SetTimer(window_, 4, 600, nullptr);
        }
        if (event == EN_CHANGE && !populating_ && id == 30) {
            note_command_selection_ = 0;
            notes_scroll_ = 0;
            layout();
        }
        if (event == EN_CHANGE && !populating_ && id == 45) {
            presence_dirty_ = true;
            SetTimer(window_, 7, 1500, nullptr);
        }
        if (event == EN_KILLFOCUS && id == 45) save_presence_message(false);
        if ((event == EN_SETFOCUS || event == EN_KILLFOCUS) && id == 30) layout();
        if (event == EN_CHANGE && !populating_ && id == 51) {
            note_selected_id_.clear();
            note_result_offset_ = 0;
            note_search_page_offset_ = 0;
            note_results_dirty_ = true;
            if (note_page_ == NotePage::search) {
                note_results_.clear();
                note_search_total_ = 0;
                note_search_next_.reset();
                note_search_pending_ = true;
                SetTimer(window_, 6, 150, nullptr);
            }
            layout();
        }
        if (event == EN_CHANGE && !populating_ && id == 52) {
            note_find_pending_ = true;
            note_find_results_.clear();
            note_find_selection_ = 0;
            note_highlight_id_.clear();
            SetTimer(window_, 6, 150, nullptr);
            layout();
        }
        return 0;
    }
    LRESULT message(HWND target, UINT message, WPARAM wparam, LPARAM lparam, bool content) {
        if (message == WM_PAINT) { paint(target, content); return 0; }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_DRAWITEM) {
            const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (draw.CtlType == ODT_MENU) draw_menu(draw); else draw_button(draw);
            return TRUE;
        }
        if (message == WM_MEASUREITEM && reinterpret_cast<MEASUREITEMSTRUCT*>(lparam)->CtlType == ODT_MENU) {
            measure_menu(*reinterpret_cast<MEASUREITEMSTRUCT*>(lparam)); return TRUE;
        }
        if (message == WM_CONTEXTMENU) {
            app_context_menu(wparam ? reinterpret_cast<HWND>(wparam) : target, lparam); return 0;
        }
        if (message == WM_COMMAND) return command(wparam);
        if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
            HDC dc = reinterpret_cast<HDC>(wparam);
            const bool search = reinterpret_cast<HWND>(lparam) == footer_edit_;
            SetTextColor(dc, search ? RGB(225, 225, 220) : foreground);
            SetBkColor(dc, search ? search_fill : well);
            return reinterpret_cast<LRESULT>(search ? search_brush_ : edit_brush_);
        }
        if (message == WM_MOUSEWHEEL) {
            if (settings_open_) return 0;
            wheel_remainder_ += GET_WHEEL_DELTA_WPARAM(wparam);
            int steps = wheel_remainder_ / WHEEL_DELTA;
            wheel_remainder_ %= WHEEL_DELTA;
            if (content && dashboard()) { scroll_by(-steps * px(80)); return 0; }
            if (GetFocus() == footer_edit_ && !note_command_choices_.empty() && steps) {
                const int count = static_cast<int>(note_command_choices_.size());
                note_command_selection_ = static_cast<std::size_t>(std::clamp(static_cast<int>(note_command_selection_) - steps, 0, count - 1));
                layout();
            }
            else if (tag_picker_open_) scroll_tags(-steps * 2);
            else if (note_page_visible()) scroll_note_results(-steps * 3);
            else scroll_by(-steps * px(48));
            return 0;
        }
        if (message == WM_KEYDOWN) {
            if (wparam == VK_ESCAPE) { escape(); return 0; }
            if (note_keys(target, wparam)) return 0;
            if (note_page_visible() && GetKeyState(VK_CONTROL) < 0 && (wparam == VK_HOME || wparam == VK_END)) {
                scroll_note_results(wparam == VK_HOME ? -static_cast<int>(note_results_.size()) : static_cast<int>(note_results_.size()));
                return 0;
            }
            const bool conversation = detail_ == Detail::note && !note_metadata_open_;
            const int page = conversation ? note_history_bounds_.bottom - note_history_bounds_.top : viewport_height_;
            const int extent = conversation ? note_history_height_ : content_height_;
            if (wparam == VK_NEXT) { scroll_by(page - px(30)); return 0; }
            if (wparam == VK_PRIOR) { scroll_by(-page + px(30)); return 0; }
            if (wparam == VK_HOME && GetKeyState(VK_CONTROL) < 0) { scroll_by(-extent); return 0; }
            if (wparam == VK_END && GetKeyState(VK_CONTROL) < 0) { scroll_by(extent); return 0; }
        }
        if (content) {
            if (message == WM_LBUTTONDOWN) {
                if (note_page_visible() && static_cast<int>(note_results_.size()) > note_result_rows_ &&
                    GET_X_LPARAM(lparam) >= note_results_bounds_.right - px(10) &&
                    GET_Y_LPARAM(lparam) >= note_results_bounds_.top && GET_Y_LPARAM(lparam) < note_results_bounds_.bottom) {
                    dragging_note_results_ = true;
                    drag_start_y_ = GET_Y_LPARAM(lparam);
                    drag_start_scroll_ = note_result_offset_;
                    SetCapture(content_);
                    return 0;
                }
                for (HWND editor : {body_edit_, reply_edit_, composer_edit_, settings_reply_edit_}) {
                    if (editor_gutter(editor, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                        dragging_editor_ = editor;
                        SetCapture(content_);
                        scroll_editor(editor, GET_Y_LPARAM(lparam));
                        return 0;
                    }
                }
                if (!settings_open_ && detail_ == Detail::note && !tag_picker_open_ && !note_metadata_open_) {
                    const int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
                    if (x >= note_history_bounds_.right - px(10) && y >= note_history_bounds_.top &&
                        y < note_history_bounds_.bottom && note_scroll_limit()) {
                        dragging_note_scroll_ = true;
                        drag_start_y_ = y;
                        drag_start_scroll_ = note_scroll_;
                        SetCapture(content_);
                        return 0;
                    }
                    auto filename = note_image_at(x, y);
                    if (!filename.empty()) { open_note_image(filename); return 0; }
                }
            }
            if (message == WM_MOUSEMOVE && dragging_note_results_) {
                const int height = note_results_bounds_.bottom - note_results_bounds_.top;
                const int target_scroll = drag_start_scroll_ + MulDiv(GET_Y_LPARAM(lparam) - drag_start_y_,
                    static_cast<int>(note_results_.size()), std::max(1, height));
                scroll_note_results(target_scroll - note_result_offset_);
                return 0;
            }
            if (message == WM_LBUTTONUP && dragging_note_results_) { dragging_note_results_ = false; ReleaseCapture(); return 0; }
            if (message == WM_MOUSEMOVE && dragging_editor_) { scroll_editor(dragging_editor_, GET_Y_LPARAM(lparam)); return 0; }
            if (message == WM_LBUTTONUP && dragging_editor_) { dragging_editor_ = nullptr; ReleaseCapture(); return 0; }
            if (message == WM_MOUSEMOVE && dragging_note_scroll_) {
                const int height = note_history_bounds_.bottom - note_history_bounds_.top;
                const int movement = GET_Y_LPARAM(lparam) - drag_start_y_;
                const int target_scroll = drag_start_scroll_ + MulDiv(movement, note_history_height_, std::max(1, height));
                scroll_by(target_scroll - note_scroll_);
                return 0;
            }
            if (message == WM_LBUTTONUP && dragging_note_scroll_) { dragging_note_scroll_ = false; ReleaseCapture(); return 0; }
            if (message == WM_CAPTURECHANGED) {
                dragging_editor_ = nullptr;
                dragging_scroll_ = false;
                dragging_note_scroll_ = false;
                dragging_note_results_ = false;
            }
            return DefWindowProcW(target, message, wparam, lparam);
        }
        if (taskbar_created_ && message == taskbar_created_) { add_tray(); update_tray(); return 0; }
        if (message == show_message) { show(); return 0; }
        if (message == change_message) {
            std::unique_ptr<Json> event(reinterpret_cast<Json*>(lparam));
            changed(*event);
            return 0;
        }
        switch (message) {
        case WM_HOTKEY:
            if (!context_menu_ && hotkey_.matches(wparam)) {
                if (IsWindowVisible(window_)) hide(); else show({}, true);
            }
            return 0;
        case tray_message: {
            UINT event = LOWORD(lparam);
            if (event == NIN_SELECT || event == NIN_KEYSELECT) {
                if (IsWindowVisible(window_)) hide(); else show();
            } else if (event == WM_CONTEXTMENU) context_menu();
            else if (event == NIN_BALLOONUSERCLICK) {
                auto id = active_banner_ ? active_banner_->id : std::string();
                const bool inbox = active_banner_ && active_banner_->inbox;
                show(id, false, inbox);
                next_banner();
            } else if (event == NIN_BALLOONTIMEOUT || event == NIN_BALLOONHIDE) next_banner();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE && IsWindowVisible(window_) &&
                !keep_open_ && !context_menu_ && !quitting_) hide();
            return 0;
        case WM_SIZE: layout(); return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            create_fonts();
            auto* bounds = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOACTIVATE | SWP_NOZORDER);
            layout();
            return 0;
        }
        case WM_TIMER:
            if (wparam == 1) {
                store_.tick();
                update_busy_mode();
                if (busy() && IsWindowVisible(window_)) {
                    InvalidateRect(window_, nullptr, FALSE);
                    if (settings_open_) InvalidateRect(content_, nullptr, FALSE);
                }
                if (!note_undo_id_.empty() && GetTickCount64() >= note_undo_until_) {
                    note_undo_id_.clear();
                    layout();
                }
                if (!note_highlight_id_.empty() && GetTickCount64() >= note_highlight_until_) {
                    note_highlight_id_.clear();
                    InvalidateRect(content_, nullptr, FALSE);
                }
                if (active_banner_ && GetTickCount64() - banner_started_ > 60000) next_banner();
                if (!feedback_.empty() && GetTickCount64() >= feedback_until_) {
                    feedback_.clear(); InvalidateRect(content_, nullptr, FALSE);
                }
            } else if (wparam == 7) save_presence_message(false);
            else if (wparam == 3) { KillTimer(window_, 3); save_detail(SaveIntent::background); }
            else if (wparam == 4) { KillTimer(window_, 4); save_note_draft(); }
            else if (wparam == 6) {
                KillTimer(window_, 6);
                if (note_find_pending_ && note_find_open_)
                    refresh_note_find(!settings_open_ && !note_metadata_open_ && !tag_picker_open_);
                if (note_search_pending_) {
                    note_search_pending_ = false;
                    note_results_dirty_ = true;
                    layout();
                }
            }
            return 0;
        case WM_CLOSE: hide(); return 0;
        case WM_QUERYENDSESSION: return save_detail() ? TRUE : FALSE;
        case WM_ENDSESSION: if (wparam) DestroyWindow(window_); return 0;
        case WM_DESTROY: {
            hotkey_.stop();
            store_.set_changed({});
            server_.stop();
            MSG queued{};
            while (PeekMessageW(&queued, window_, change_message, change_message, PM_REMOVE))
                delete reinterpret_cast<Json*>(queued.lParam);
            NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
            data.hWnd = window_;
            data.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &data);
            KillTimer(window_, 1); KillTimer(window_, 3); KillTimer(window_, 4); KillTimer(window_, 6); KillTimer(window_, 7);
            PostQuitMessage(0);
            return 0;
        }
        }
        return DefWindowProcW(target, message, wparam, lparam);
    }
    static LRESULT CALLBACK root_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        try { return app->message(window, message, wparam, lparam, false); }
        catch (const std::exception& error) { app->feedback(widen(error.what())); return 0; }
    }
    static LRESULT CALLBACK panel_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        try { return app->panel_message(window, message, wparam, lparam); }
        catch (const std::exception& error) { app->feedback(widen(error.what())); return 0; }
    }
    static LRESULT CALLBACK content_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        try { return app->message(window, message, wparam, lparam, true); }
        catch (const std::exception& error) { app->feedback(widen(error.what())); return 0; }
    }
    static LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
        auto* app = reinterpret_cast<App*>(data);
        if (message == WM_CONTEXTMENU) { app->app_context_menu(window, lparam); return 0; }
        if (message == WM_MOUSEWHEEL && (window == app->note_query_edit_ || window == app->note_find_edit_ || window == app->footer_edit_))
            return SendMessageW(app->window_, message, wparam, lparam);
        if (message == WM_PASTE && window == app->composer_edit_ && app->paste_note_attachment()) return 0;
        if (message == WM_CHAR && wparam == 0x16 && window == app->composer_edit_) {
            SendMessageW(window, WM_PASTE, 0, 0); return 0;
        }
        if (message == WM_MOUSEWHEEL && (window == app->body_edit_ || window == app->reply_edit_ || window == app->composer_edit_ || window == app->settings_reply_edit_)) {
            SendMessageW(window, EM_LINESCROLL, 0, -GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 3);
            InvalidateRect(app->content_, nullptr, FALSE);
            return 0;
        }
        if (message == WM_KEYDOWN) {
            if (wparam == VK_ESCAPE) { app->escape(); return 0; }
            if (app->note_keys(window, wparam)) return 0;
            if (app->settings_keys(window, wparam)) return 0;
            if (wparam == VK_INSERT && GetKeyState(VK_SHIFT) < 0 && window == app->composer_edit_) {
                SendMessageW(window, WM_PASTE, 0, 0); return 0;
            }
            if (wparam == VK_RETURN && window == app->footer_edit_) { app->notes_enter(); return 0; }
            if (wparam == VK_RETURN && window == app->task_entry_edit_) { app->add_task(); return 0; }
            if (wparam == VK_RETURN && window == app->presence_edit_ && GetKeyState(VK_SHIFT) >= 0) {
                app->save_presence_message(true); return 0;
            }
            if (wparam == VK_RETURN && window == app->tag_query_edit_) { app->activate_tag_query(); return 0; }
            if (wparam == VK_RETURN && window == app->composer_edit_ && GetKeyState(VK_SHIFT) >= 0) {
                app->send_note_message(); return 0;
            }
            if (wparam == VK_RETURN && GetKeyState(VK_CONTROL) < 0) {
                if (window == app->reply_edit_) app->send_reply(); else app->save_detail();
                return 0;
            }
        }
        if (message == WM_CHAR && (wparam == VK_ESCAPE || (wparam == VK_RETURN &&
            (window == app->footer_edit_ || window == app->tag_query_edit_ || window == app->note_query_edit_ ||
             window == app->note_find_edit_ ||
             window == app->settings_width_edit_ || window == app->settings_height_edit_ || GetKeyState(VK_CONTROL) < 0 ||
             window == app->settings_retention_edit_ ||
             window == app->task_entry_edit_ ||
             (window == app->presence_edit_ && GetKeyState(VK_SHIFT) >= 0) ||
             (window == app->composer_edit_ && GetKeyState(VK_SHIFT) >= 0))))) return 0;
        if (message == WM_CHAR && wparam == 0x06 && app->detail_ == Detail::note) return 0;
        return DefSubclassProc(window, message, wparam, lparam);
    }
    static LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
        auto* app = reinterpret_cast<App*>(data);
        if (message == WM_CONTEXTMENU) { app->app_context_menu(window, lparam); return 0; }
        if (message == WM_ERASEBKGND) {
            RECT bounds{};
            GetClientRect(window, &bounds);
            fill(reinterpret_cast<HDC>(wparam), bounds, background);
            return 1;
        }
        int id = GetDlgCtrlID(window);
        auto found = app->buttons_.find(id);
        if (found != app->buttons_.end()) {
            if (message == WM_MOUSEMOVE && !found->second.hover) {
                found->second.hover = true;
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
                TrackMouseEvent(&track);
                InvalidateRect(window, nullptr, FALSE);
            }
            if (message == WM_MOUSELEAVE) { found->second.hover = false; InvalidateRect(window, nullptr, FALSE); }
        }
        if (message == WM_SETFOCUS && GetParent(window) == app->content_ && app->dashboard()) {
            // Keyboard focus on a card's button brings that card into view.
            RECT bounds{}, area{};
            GetWindowRect(window, &bounds);
            MapWindowPoints(nullptr, app->content_, reinterpret_cast<POINT*>(&bounds), 2);
            GetClientRect(app->content_, &area);
            if (bounds.left < 0) app->scroll_by(bounds.left - app->px(4));
            else if (bounds.right > area.right) app->scroll_by(bounds.right - area.right + app->px(4));
        }
        if (message == WM_KEYDOWN && wparam == VK_ESCAPE) { app->escape(); return 0; }
        if (message == WM_MOUSEWHEEL) return SendMessageW(app->window_, message, wparam, lparam);
        return DefSubclassProc(window, message, wparam, lparam);
    }
};
}

int run_tray(HINSTANCE instance, bool show_on_start) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 1;
    HANDLE mutex = nullptr;
    int result = 1;
    try {
        auto name = L"Local\\Knock.Tray.v1." + user_identity();
        mutex = CreateMutexW(nullptr, TRUE, name.c_str());
        if (!mutex) throw std::runtime_error("Could not create the app instance lock.");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (show_on_start) {
                HWND existing = FindWindowW(window_class, nullptr);
                if (existing) PostMessageW(existing, show_message, 0, 0);
            }
            CloseHandle(mutex);
            Gdiplus::GdiplusShutdown(token);
            return 0;
        }
        { App app(instance); result = app.run(show_on_start); }
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, widen(error.what()).c_str(), L"Knock could not start", MB_OK | MB_ICONERROR);
    }
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    Gdiplus::GdiplusShutdown(token);
    return result;
}
}
