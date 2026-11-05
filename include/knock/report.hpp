#pragma once

#include <string>

namespace knock {

// Text placed on the clipboard by COPY REPORT: the stored summary exactly,
// then the report path on its own line. Line breaks become CRLF for Windows
// editors and chat boxes; the summary was already stripped of terminal
// formatting when the agent ran knock done.
inline std::string report_clipboard_text(const std::string& summary, const std::string& report) {
    std::string result;
    auto append = [&](const std::string& text) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r') continue;
            if (text[i] == '\n') result += "\r\n";
            else result += text[i];
        }
    };
    append(summary);
    if (!report.empty()) {
        result += "\r\n\r\n";
        append(report);
    }
    return result;
}

}
