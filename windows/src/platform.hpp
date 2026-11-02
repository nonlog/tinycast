#pragma once

#include "core.hpp"
#include <windows.h>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

namespace tinycast {

struct Quicklink {
    std::wstring name, keyword, url;
};
struct Snippet {
    std::wstring name, text;
};
struct CustomCommand {
    std::wstring name, executable, arguments;
};
struct Settings {
    std::wstring theme{L"system"};
    std::wstring language{L"system"};
    unsigned hotkeyModifiers{MOD_CONTROL | MOD_ALT};
    unsigned hotkeyKey{VK_SPACE};
    bool clipboardEnabled{false};
    unsigned clipboardLimit{100};
    unsigned windowGap{8};
    std::vector<std::wstring> fileRoots;
    std::vector<Quicklink> quicklinks;
    std::vector<Snippet> snippets;
    std::vector<CustomCommand> commands;
    std::vector<std::wstring> favorites;
    std::map<std::wstring, std::uint32_t> usage;
};

std::wstring dataDirectory();
std::wstring settingsPath();
Settings loadSettings(std::wstring& warning);
bool saveSettings(const Settings& settings, std::wstring& error);
std::vector<Entry> scanApplications(std::stop_token stop);
std::vector<Entry> searchFiles(const std::vector<std::wstring>& roots, std::wstring_view query,
                               std::stop_token stop, std::size_t limit = 100);
HICON loadEntryIcon(const Entry& entry, int pixels);
bool openTarget(HWND owner, std::wstring_view target, std::wstring_view arguments,
                std::wstring& error);
std::optional<std::wstring> readClipboardText(HWND owner);
bool copyText(HWND owner, std::wstring_view text, std::wstring& error);
bool pasteText(HWND owner, HWND destination, std::wstring_view text, std::wstring& error);
bool moveWindow(HWND target, WindowAction action, unsigned gap, std::wstring& error);
bool launchAtLoginEnabled();
bool setLaunchAtLogin(bool enabled, std::wstring& error);
bool systemUsesDarkTheme();
std::wstring chooseFolder(HWND owner);
std::wstring errorMessage(unsigned long code);

}
