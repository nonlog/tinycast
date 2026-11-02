#include "palette.hpp"

#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tinycast {
namespace {
constexpr UINT AppsReady = WM_APP + 2, FilesReady = WM_APP + 3, TrayMessage = WM_APP + 4;
constexpr UINT RefreshMessage = WM_APP + 5;
constexpr UINT_PTR FileTimer = 1, SmokeTimer = 2;
constexpr int SearchID = 101, ListID = 102;
constexpr unsigned HotkeyID = 1;
constexpr int PanelWidth = 750, PanelHeight = 520;

const wchar_t* actionNames[] = {
    L"Left half", L"Right half", L"Top half", L"Bottom half",
    L"Top-left quarter", L"Top-right quarter", L"Bottom-left quarter", L"Bottom-right quarter",
    L"First third", L"Center third", L"Last third", L"First two thirds", L"Last two thirds",
    L"Maximize", L"Center window", L"Almost maximize", L"Make larger", L"Make smaller"
};
const wchar_t* actionNamesZh[] = {
    L"窗口移到左半屏", L"窗口移到右半屏", L"窗口移到上半屏", L"窗口移到下半屏",
    L"窗口移到左上角", L"窗口移到右上角", L"窗口移到左下角", L"窗口移到右下角",
    L"窗口移到左侧三分之一", L"窗口移到中间三分之一", L"窗口移到右侧三分之一",
    L"窗口移到左侧三分之二", L"窗口移到右侧三分之二", L"最大化窗口", L"窗口居中",
    L"窗口放大到九成", L"放大窗口", L"缩小窗口"
};

std::wstring oneLine(std::wstring text, std::size_t limit = 100) {
    for (auto& c : text) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    if (text.size() > limit) { text.resize(limit); text += L"…"; }
    return text;
}

std::pair<std::wstring, std::wstring> dateAndTime() {
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t date[32]{}, time[32]{};
    swprintf_s(date, L"%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    swprintf_s(time, L"%02u:%02u", now.wHour, now.wMinute);
    return {date, time};
}

std::wstring trimText(std::wstring text) {
    const auto start = text.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return {};
    return text.substr(start, text.find_last_not_of(L" \t\r\n") - start + 1);
}

HICON makeIcon() {
    constexpr int size = 32;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header); header.bV5Width = size; header.bV5Height = -size;
    header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000; header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff; header.bV5AlphaMask = 0xff000000;
    void* pixels{};
    const auto screen = GetDC(nullptr);
    const auto bitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto memory = CreateCompatibleDC(screen); ReleaseDC(nullptr, screen);
    if (!bitmap || !memory || !pixels) { if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); return nullptr; }
    const auto old = SelectObject(memory, bitmap);
    auto* data = static_cast<DWORD*>(pixels);
    std::fill(data, data + size * size, 0xff252129);
    POINT bolt[]{{18,4},{8,18},{15,18},{13,28},{24,13},{17,13}};
    const auto pen = CreatePen(PS_SOLID, 1, RGB(238,225,255));
    const auto brush = CreateSolidBrush(RGB(238,225,255));
    const auto oldPen = SelectObject(memory, pen), oldBrush = SelectObject(memory, brush);
    Polygon(memory, bolt, static_cast<int>(std::size(bolt)));
    SelectObject(memory, oldPen); SelectObject(memory, oldBrush);
    DeleteObject(pen); DeleteObject(brush);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const int dx = x < 7 ? 7 - x : x > 24 ? x - 24 : 0;
        const int dy = y < 7 ? 7 - y : y > 24 ? y - 24 : 0;
        data[y * size + x] = dx * dx + dy * dy > 49 ? 0 : data[y * size + x] | 0xff000000;
    }
    SelectObject(memory, old); DeleteDC(memory);
    const auto mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO icon{}; icon.fIcon = TRUE; icon.hbmColor = bitmap; icon.hbmMask = mask;
    const auto result = CreateIconIndirect(&icon);
    DeleteObject(bitmap); DeleteObject(mask);
    return result;
}
}

App::App(Options options) : options_(std::move(options)) {
    settings_ = loadSettings(configWarning_);
    if (!options_.theme.empty()) settings_.theme = options_.theme;
    if (options_.smoke) settings_.clipboardEnabled = false;
}

App::~App() {
    shutdown();
    for (auto font : {searchFont_, titleFont_, detailFont_, symbolFont_}) if (font) DeleteObject(font);
    if (background_) DeleteObject(background_);
    for (const auto& [key, icon] : iconCache_) if (icon) DestroyIcon(icon);
    if (appIcon_) DestroyIcon(appIcon_);
}

const wchar_t* App::tr(const wchar_t* en, const wchar_t* zh) const { return chinese_ ? zh : en; }
int App::px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

std::wstring App::query() const {
    const int length = GetWindowTextLengthW(search_);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    value.resize(GetWindowTextW(search_, value.data(), length + 1));
    return value;
}

std::wstring App::modeName() const {
    switch (mode_) {
    case Mode::Clipboard: return tr(L"CLIPBOARD", L"剪贴板历史");
    case Mode::Files: return tr(L"FILES · BOUNDED FOLDER SEARCH", L"文件 · 限额文件夹搜索");
    case Mode::Snippets: return tr(L"SNIPPETS", L"文本片段");
    case Mode::Quicklinks: return tr(L"QUICKLINKS", L"快捷链接");
    case Mode::History: return tr(L"CALCULATOR HISTORY · THIS SESSION", L"计算历史 · 当前会话");
    case Mode::Settings: return tr(L"SETTINGS", L"设置");
    case Mode::Confirm: return tr(L"CONFIRM ACTION", L"确认操作");
    default: return query().empty() ? tr(L"APPLICATIONS & COMMANDS", L"应用与命令") : tr(L"RESULTS", L"搜索结果");
    }
}

std::wstring App::hotkeyLabel() const {
    std::wstring result;
    if (settings_.hotkeyModifiers & MOD_CONTROL) result += L"Ctrl+";
    if (settings_.hotkeyModifiers & MOD_ALT) result += L"Alt+";
    if (settings_.hotkeyModifiers & MOD_SHIFT) result += L"Shift+";
    if (settings_.hotkeyModifiers & MOD_WIN) result += L"Win+";
    if (settings_.hotkeyKey == VK_SPACE) return result + L"Space";
    wchar_t name[64]{};
    GetKeyNameTextW(static_cast<LONG>(MapVirtualKeyW(settings_.hotkeyKey, MAPVK_VK_TO_VSC) << 16), name, 64);
    return result + name;
}

std::wstring App::emptyMessage() const {
    if (mode_ == Mode::Files && query().size() < 2)
        return tr(L"Type at least two characters to search file names", L"输入至少两个字符，搜索文件名");
    if (mode_ == Mode::Files && filesBusy_)
        return tr(L"Searching configured local folders…", L"正在搜索配置的本地文件夹…");
    if (mode_ == Mode::Clipboard)
        return tr(L"Text you copy after enabling history appears here", L"开启历史后复制的文本将显示在这里");
    if (mode_ == Mode::Snippets)
        return tr(L"Add snippets in Settings → Edit settings.json", L"在「设置 → 编辑 settings.json」中添加文本片段");
    if (mode_ == Mode::History)
        return tr(L"Calculate in the main search field, then press Enter", L"在主搜索框输入算式，再按 Enter 复制结果");
    return tr(L"No matching results", L"没有匹配的结果");
}

bool App::create(HINSTANCE instance, const std::wstring& className) {
    instance_ = instance;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    appIcon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, 0));
    if (!appIcon_) appIcon_ = makeIcon();
    WNDCLASSEXW cls{sizeof(cls)};
    cls.style = CS_DBLCLKS | CS_DROPSHADOW; cls.lpfnWndProc = windowProc; cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = appIcon_; cls.hIconSm = appIcon_;
    cls.lpszClassName = className.c_str();
    if (!RegisterClassExW(&cls)) return false;
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, className.c_str(), L"Tinycast Windows Preview",
                              WS_POPUP | WS_THICKFRAME, CW_USEDEFAULT, CW_USEDEFAULT, PanelWidth, PanelHeight,
                              nullptr, nullptr, instance, this);
    if (!window_) return false;
    dpi_ = GetDpiForWindow(window_);
    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              0, 0, 10, 10, window_, reinterpret_cast<HMENU>(SearchID), instance, nullptr);
    list_ = CreateWindowExW(0, L"LISTBOX", L"Search results", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                            LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                            0, 0, 10, 10, window_, reinterpret_cast<HMENU>(ListID), instance, nullptr);
    if (!search_ || !list_) return false;
    SendMessageW(search_, EM_SETLIMITTEXT, 512, 0);
    SetWindowSubclass(search_, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(list_, listProc, 1, reinterpret_cast<DWORD_PTR>(this));
    applyTheme(); layout();
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (options_.smoke) {
        applications_ = {{L"sample:notes", L"记事本", L"Application", L"", L"", EntryKind::Application},
                         {L"sample:terminal", L"Windows Terminal", L"Application", L"", L"", EntryKind::Application},
                         {L"sample:code", L"Visual Studio Code", L"Application", L"", L"", EntryKind::Application}};
    } else {
        tray(true); registerShortcut(); clipboardSubscription();
        startFileWorker(); refreshApplications();
    }
    refresh();
    if (!configWarning_.empty()) updateStatus(configWarning_);
    if (!options_.background || options_.smoke) show();
    if (options_.smoke) SetTimer(window_, SmokeTimer, 700, nullptr);
    return true;
}

int App::run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void App::makeFonts() {
    for (auto font : {searchFont_, titleFont_, detailFont_, symbolFont_}) if (font) DeleteObject(font);
    const auto make = [&](int size, int weight, const wchar_t* family) {
        return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, family);
    };
    searchFont_ = make(20, FW_NORMAL, L"Segoe UI");
    titleFont_ = make(15, FW_MEDIUM, L"Segoe UI");
    detailFont_ = make(12, FW_NORMAL, L"Segoe UI");
    symbolFont_ = make(20, FW_NORMAL, L"Segoe MDL2 Assets");
    SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(searchFont_), TRUE);
    SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
    SendMessageW(list_, LB_SETITEMHEIGHT, 0, px(54));
}

void App::applyTheme() {
    dark_ = settings_.theme == L"dark" || (settings_.theme == L"system" && systemUsesDarkTheme());
    chinese_ = settings_.language == L"zh-CN" ||
               (settings_.language == L"system" && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE);
    theme_ = dark_ ? Theme{RGB(28,28,32), RGB(245,245,248), RGB(162,162,174), RGB(112,112,127),
                           RGB(51,49,60), RGB(40,39,46), RGB(196,170,250), RGB(45,44,52)} :
                     Theme{RGB(249,249,252), RGB(32,31,39), RGB(100,99,113), RGB(134,133,146),
                           RGB(231,226,242), RGB(240,237,246), RGB(105,67,166), RGB(235,233,241)};
    if (background_) DeleteObject(background_);
    background_ = CreateSolidBrush(theme_.background);
    BOOL dark = dark_; const int corner = 2;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const COLORREF border = dark_ ? RGB(65,63,74) : RGB(216,211,227);
    DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    SetWindowTheme(list_, dark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(L"Search apps, commands, or calculate…", L"搜索应用、命令，或输入算式…")));
    makeFonts();
    InvalidateRect(window_, nullptr, TRUE); InvalidateRect(search_, nullptr, TRUE); InvalidateRect(list_, nullptr, TRUE);
}

void App::layout() {
    RECT area{}; GetClientRect(window_, &area);
    MoveWindow(search_, px(62), px(25), area.right - px(94), px(35), TRUE);
    MoveWindow(list_, px(16), px(98), area.right - px(32), area.bottom - px(184), TRUE);
}

void App::position() {
    POINT point{}; GetCursorPos(&point);
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info);
    const int width = std::min(px(PanelWidth), info.rcWork.right - info.rcWork.left - px(24));
    const int height = std::min(px(PanelHeight), info.rcWork.bottom - info.rcWork.top - px(24));
    const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
    const int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) * 2 / 5;
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    layout();
}

void App::show(bool capturePrevious) {
    if (capturePrevious) {
        const auto foreground = GetForegroundWindow();
        DWORD pid{}; GetWindowThreadProcessId(foreground, &pid);
        if (foreground && pid != GetCurrentProcessId()) previousWindow_ = foreground;
    }
    position(); ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); SetFocus(search_);
    SendMessageW(search_, EM_SETSEL, 0, -1);
    InvalidateRect(window_, nullptr, TRUE);
}

void App::hide(bool restoreFocus) {
    KillTimer(window_, FileTimer);
    { std::lock_guard lock(fileMutex_); activeFileStop_.request_stop(); fileJob_.reset(); }
    ++fileGeneration_; filesBusy_ = false;
    ShowWindow(window_, SW_HIDE);
    if (restoreFocus && IsWindow(previousWindow_)) SetForegroundWindow(previousWindow_);
}

void App::shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    appWorker_.request_stop(); fileWorker_.request_stop();
    { std::lock_guard lock(fileMutex_); activeFileStop_.request_stop(); fileJob_.reset(); }
    fileCondition_.notify_all();
    if (appWorker_.joinable()) appWorker_.join();
    if (fileWorker_.joinable()) fileWorker_.join();
    if (clipboardListening_) RemoveClipboardFormatListener(window_);
    if (hotkeyRegistered_) UnregisterHotKey(window_, HotkeyID);
    if (trayAdded_) tray(false);
    MSG message{};
    while (PeekMessageW(&message, window_, AppsReady, AppsReady, PM_REMOVE)) delete reinterpret_cast<std::vector<Entry>*>(message.lParam);
    while (PeekMessageW(&message, window_, FilesReady, FilesReady, PM_REMOVE)) delete reinterpret_cast<FileResult*>(message.lParam);
    clips_.clear();
}

void App::setMode(Mode mode) {
    { std::lock_guard lock(fileMutex_); activeFileStop_.request_stop(); fileJob_.reset(); }
    ++fileGeneration_; filesBusy_ = false;
    mode_ = mode; status_.clear(); fileResults_.clear();
    SetWindowTextW(search_, L"");
    EnableWindow(search_, mode != Mode::Confirm);
    refresh(); SetFocus(mode == Mode::Confirm ? list_ : search_);
}

void App::refresh() {
    if (!search_ || composing_ || shuttingDown_) return;
    const auto text = trimText(query());
    std::vector<Entry> entries;
    const auto add = [&](const wchar_t* id, const wchar_t* en, const wchar_t* zh,
                         const wchar_t* detailEn, const wchar_t* detailZh) {
        entries.push_back({id, tr(en, zh), tr(detailEn, detailZh), L"", L"", EntryKind::Command});
    };
    if (mode_ == Mode::Confirm) {
        add(L"confirm:cancel", L"Cancel", L"取消", L"Keep everything unchanged", L"不执行任何更改");
        entries.push_back({L"confirm:run", tr(L"Confirm: ", L"确认：") + pending_.title, pending_.subtitle,
                           L"", L"", EntryKind::Command});
        setResults(std::move(entries)); return;
    }
    if (mode_ == Mode::Home) {
        entries = applications_;
        for (auto& entry : entries) entry.subtitle = tr(L"Application", L"应用程序");
        if (settings_.clipboardEnabled) add(L"cmd:clipboard", L"Clipboard history", L"剪贴板历史", L"Recent text · Tab", L"最近复制的文本 · Tab");
        else add(L"cmd:clipboard", L"Enable clipboard history", L"启用剪贴板历史", L"Opt-in · text stays in memory only", L"需主动开启 · 文本仅保留在内存中");
        add(L"cmd:files", L"Search files", L"搜索文件", L"Search names in configured folders", L"搜索配置文件夹内的文件名 · files");
        add(L"cmd:snippets", L"Text snippets", L"文本片段", L"Reusable text · no keyboard monitoring", L"常用文本 · 不监听键盘输入 · snippets");
        add(L"cmd:quicklinks", L"Quicklinks", L"快捷链接", L"Try g search terms or gh repository", L"输入 g 搜索词，或 gh 仓库名");
        add(L"cmd:history", L"Calculator history", L"计算历史", L"Results from this session", L"当前会话中已复制的计算结果");
        add(L"cmd:settings", L"Tinycast settings", L"Tinycast 设置", L"Appearance, shortcut, clipboard · Ctrl+,", L"外观、快捷键、剪贴板 · Ctrl+, · settings");
        add(L"cmd:refresh", L"Refresh applications", L"刷新应用列表", L"Rescan Start Menu and packaged apps", L"重新扫描开始菜单与商店应用 · refresh");
        for (std::size_t i = 0; i < std::size(actionNames); ++i)
            entries.push_back({L"window:" + std::to_wstring(i), chinese_ ? actionNamesZh[i] : actionNames[i],
                               tr(L"Window management · previous app", L"窗口管理 · 操作之前使用的应用"), L"", L"", EntryKind::Command});
        add(L"cmd:restore", L"Restore window", L"还原窗口", L"Restore the window's original position", L"还原窗口原来的位置与大小");
        for (const auto& item : settings_.commands)
            entries.push_back({L"custom:" + item.name, item.name, tr(L"Custom command · confirmation required", L"自定义命令 · 执行前需要确认"),
                               item.executable, item.arguments, EntryKind::Command});
    }
    if (mode_ == Mode::Home || mode_ == Mode::Quicklinks) {
        for (const auto& item : settings_.quicklinks)
            entries.push_back({L"link:" + item.name, item.name, tr(L"Quicklink · ", L"快捷链接 · ") + item.keyword,
                               item.url, L"", EntryKind::Quicklink});
    }
    if (mode_ == Mode::Home || mode_ == Mode::Snippets) {
        for (const auto& item : settings_.snippets)
            entries.push_back({L"snippet:" + item.name, item.name, tr(L"Text snippet · Enter to paste", L"文本片段 · Enter 粘贴"), item.text, L"", EntryKind::Snippet});
    }
    if (mode_ == Mode::Clipboard) {
        for (std::size_t i = 0; i < clips_.size(); ++i)
            entries.push_back({L"clip:" + std::to_wstring(i), oneLine(clips_[i]),
                               tr(L"Enter to paste · Ctrl+Enter to copy", L"Enter 粘贴 · Ctrl+Enter 复制"), clips_[i], L"", EntryKind::Clipboard});
    }
    if (mode_ == Mode::History) {
        for (std::size_t i = 0; i < calculations_.size(); ++i)
            entries.push_back({L"calc:" + std::to_wstring(i), calculations_[i], tr(L"Copy result", L"复制结果"), calculations_[i], L"", EntryKind::Calculator});
    }
    if (mode_ == Mode::Files) entries = fileResults_;
    if (mode_ == Mode::Settings) {
        entries.push_back({L"setting:theme", tr(L"Appearance", L"外观"), settings_.theme + tr(L" · cycle System / Dark / Light", L" · 切换跟随系统 / 深色 / 浅色"), L"", L"", EntryKind::Command});
        entries.push_back({L"setting:language", tr(L"Interface language", L"界面语言"), settings_.language + L" · System / English / 简体中文", L"", L"", EntryKind::Command});
        entries.push_back({L"setting:hotkey", tr(L"Global shortcut", L"全局快捷键"), hotkeyLabel() + tr(L" · Enter cycles presets", L" · Enter 切换预设"), L"", L"", EntryKind::Command});
        entries.push_back({L"setting:clipboard", tr(L"Clipboard history", L"剪贴板历史"), settings_.clipboardEnabled ? tr(L"On · turn off recording", L"已开启 · 停止记录") : tr(L"Off · enable in-memory text history", L"已关闭 · 开启仅驻留内存的文本历史"), L"", L"", EntryKind::Command});
        add(L"setting:clear", L"Clear clipboard history", L"清空剪贴板历史", L"Remove all recorded text from this session", L"清除本次会话记录的全部文本");
        entries.push_back({L"setting:startup", tr(L"Launch at sign-in", L"登录时启动"), launchAtLoginEnabled() ? tr(L"On · disable", L"已开启 · 关闭") : tr(L"Off · enable", L"已关闭 · 开启"), L"", L"", EntryKind::Command});
        add(L"setting:folder", L"Add file search folder", L"添加文件搜索文件夹", L"Local folders only · no continuous crawl", L"只搜索本地文件夹 · 不进行持续扫描");
        add(L"setting:json", L"Edit settings.json", L"编辑 settings.json", L"Quicklinks, snippets, commands, folders and shortcuts", L"配置快捷链接、文本片段、命令、文件夹及快捷键");
        add(L"setting:reload", L"Reload settings", L"重新加载设置", L"Validate your edits before applying them", L"验证文件内容后再应用修改");
        add(L"setting:about", L"About this Windows preview", L"关于 Windows 预览版", L"Native C++ · AGPL-3.0 · not full macOS feature parity", L"原生 C++ · AGPL-3.0 · 尚未覆盖 macOS 全部功能");
        add(L"setting:source", L"View source code", L"查看源代码", L"nonlog/tinycast · derived from abue-ammar/tinycast", L"nonlog/tinycast · 基于 abue-ammar/tinycast");
        add(L"setting:quit", L"Quit Tinycast", L"退出 Tinycast", L"Stop the tray app and clear in-memory history", L"退出托盘程序，并清空内存历史");
    }
    for (auto& entry : entries) {
        entry.pinned = std::find(settings_.favorites.begin(), settings_.favorites.end(), entry.id) != settings_.favorites.end();
        if (const auto it = settings_.usage.find(entry.id); it != settings_.usage.end()) entry.useCount = it->second;
    }
    std::vector<Entry> ranked;
    if ((mode_ == Mode::Settings || mode_ == Mode::Clipboard || mode_ == Mode::History) && text.empty()) ranked = std::move(entries);
    else for (const auto index : rankEntries(entries, text, 100)) ranked.push_back(std::move(entries[index]));
    if (mode_ == Mode::Home) {
        if (const auto calculation = calculate(text); calculation && !text.empty())
            ranked.insert(ranked.begin(), {L"calc:live", calculation->formatted, tr(L"Calculator · Enter to copy", L"计算器 · Enter 复制结果"), calculation->formatted, text, EntryKind::Calculator});
        for (const auto& link : settings_.quicklinks) {
            if (link.keyword.empty()) continue;
            const auto prefix = normalize(link.keyword) + L" ";
            if (normalize(text).starts_with(prefix)) {
                const auto input = text.substr(prefix.size());
                ranked.insert(ranked.begin(), {L"link:" + link.name, link.name + L": " + input,
                              tr(L"Search in your default browser", L"在默认浏览器中搜索"), link.url, input, EntryKind::Quicklink});
                break;
            }
        }
        if (!text.empty()) ranked.push_back({L"link:web", tr(L"Search the web: ", L"搜索网页：") + text,
                                            tr(L"Default browser · Google", L"默认浏览器 · Google"), L"https://www.google.com/search?q={query}", text, EntryKind::Quicklink});
    }
    setResults(std::move(ranked));
}

void App::setResults(std::vector<Entry> results) {
    visible_ = std::move(results);
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    for (const auto& entry : visible_) SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.title.c_str()));
    if (!visible_.empty()) SendMessageW(list_, LB_SETCURSEL, 0, 0);
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE); InvalidateRect(window_, nullptr, TRUE);
}

void App::activate(bool copyOnly) {
    if (composing_ || GetTickCount64() < imeCommitUntil_) return;
    const auto selected = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR || static_cast<std::size_t>(selected) >= visible_.size()) return;
    execute(visible_[static_cast<std::size_t>(selected)], copyOnly);
}

void App::execute(Entry entry, bool copyOnly, bool confirmed) {
    std::wstring error;
    if (entry.id == L"confirm:cancel") {
        setMode(confirmReturn_); SetWindowTextW(search_, confirmQuery_.c_str()); return;
    }
    if (entry.id == L"confirm:run") {
        const auto pending = pending_;
        setMode(confirmReturn_); execute(pending, false, true); return;
    }
    if (entry.kind == EntryKind::Command) { command(entry, confirmed); return; }
    if (entry.kind == EntryKind::Clipboard || entry.kind == EntryKind::Snippet || entry.kind == EntryKind::Calculator) {
        std::wstring text = entry.target;
        if (entry.kind == EntryKind::Snippet) {
            std::wstring clipboard;
            if (text.find(L"{clipboard}") != std::wstring::npos) clipboard = readClipboardText(window_).value_or(L"");
            const auto [date, time] = dateAndTime();
            text = expandTemplate(text, query(), clipboard, date, time);
        }
        if (copyOnly || entry.kind == EntryKind::Calculator) {
            if (!copyText(window_, text, error)) { updateStatus(error); return; }
            ignoredClipboardSequence_ = GetClipboardSequenceNumber();
            if (entry.kind == EntryKind::Calculator) {
                std::erase(calculations_, text); calculations_.insert(calculations_.begin(), text);
                if (calculations_.size() > 100) calculations_.pop_back();
            }
            updateStatus(tr(L"Copied. Ctrl+V pastes in your application.", L"已复制，可在目标应用中按 Ctrl+V 粘贴。"));
        } else {
            hide(false);
            if (!pasteText(window_, previousWindow_, text, error)) { show(false); updateStatus(error); }
            ignoredClipboardSequence_ = GetClipboardSequenceNumber();
        }
    } else {
        std::wstring target = entry.target;
        if (entry.kind == EntryKind::Quicklink) {
            if (target.find(L"{query}") != std::wstring::npos && entry.arguments.empty()) {
                const auto it = std::find_if(settings_.quicklinks.begin(), settings_.quicklinks.end(), [&](const Quicklink& link) { return L"link:" + link.name == entry.id; });
                if (it != settings_.quicklinks.end() && !it->keyword.empty()) {
                    setMode(Mode::Home); SetWindowTextW(search_, (it->keyword + L" ").c_str());
                    SendMessageW(search_, EM_SETSEL, -1, -1); return;
                }
            }
            const auto [date, time] = dateAndTime();
            target = expandTemplate(target, urlEncode(entry.arguments), L"", date, time);
        }
        hide(false);
        if (!openTarget(window_, target, entry.kind == EntryKind::Quicklink ? L"" : entry.arguments, error)) {
            show(false); updateStatus(error); return;
        }
    }
    if (entry.kind != EntryKind::Clipboard && entry.kind != EntryKind::Calculator && entry.kind != EntryKind::File && settings_.usage.size() < 4096) {
        auto& count = settings_.usage[entry.id]; count = std::min<std::uint32_t>(count + 1, 1000000); persist();
    }
}

void App::requestConfirmation(Entry entry) {
    confirmReturn_ = mode_; confirmQuery_ = query(); pending_ = std::move(entry); setMode(Mode::Confirm);
}

void App::command(const Entry& entry, bool confirmed) {
    const auto& id = entry.id;
    std::wstring error;
    if (id.starts_with(L"window:")) {
        const auto index = static_cast<std::size_t>(std::stoul(id.substr(7)));
        if (index >= std::size(actionNames)) return;
        if (IsWindow(previousWindow_) && !restore_.contains(previousWindow_)) {
            RestoreState state; GetWindowThreadProcessId(previousWindow_, &state.pid);
            if (GetWindowPlacement(previousWindow_, &state.placement)) {
                if (restore_.size() >= 128) restore_.clear();
                restore_[previousWindow_] = state;
            }
        }
        hide();
        if (!moveWindow(previousWindow_, static_cast<WindowAction>(index), settings_.windowGap, error)) { show(false); updateStatus(error); }
    } else if (id == L"cmd:restore") {
        const auto found = restore_.find(previousWindow_);
        DWORD pid{}; GetWindowThreadProcessId(previousWindow_, &pid);
        if (found == restore_.end() || found->second.pid != pid) { updateStatus(tr(L"No saved position for this window.", L"此窗口没有可还原的位置。")); return; }
        hide();
        if (!SetWindowPlacement(previousWindow_, &found->second.placement)) { show(false); updateStatus(errorMessage(GetLastError())); }
        else restore_.erase(found);
    } else if (id == L"cmd:clipboard") {
        if (settings_.clipboardEnabled) setMode(Mode::Clipboard);
        else command({L"setting:clipboard", tr(L"Enable text clipboard history", L"开启文本剪贴板历史"),
                      tr(L"Records future copies in memory. Sensitive text can be captured; exclude formats are respected.", L"将记录之后复制的文本，仅保留在内存中。可能含敏感内容，遵守应用的排除标记。"),
                      L"", L"", EntryKind::Command}, confirmed);
    } else if (id == L"cmd:files") setMode(Mode::Files);
    else if (id == L"cmd:snippets") setMode(Mode::Snippets);
    else if (id == L"cmd:quicklinks") setMode(Mode::Quicklinks);
    else if (id == L"cmd:history") setMode(Mode::History);
    else if (id == L"cmd:settings") setMode(Mode::Settings);
    else if (id == L"cmd:refresh") refreshApplications();
    else if (id.starts_with(L"custom:")) {
        if (!confirmed) {
            Entry pending = entry; pending.subtitle = entry.target + L" " + entry.arguments;
            requestConfirmation(std::move(pending)); return;
        }
        hide(false);
        if (!openTarget(window_, entry.target, entry.arguments, error)) { show(false); updateStatus(error); }
    } else if (id == L"setting:theme") {
        settings_.theme = settings_.theme == L"system" ? L"dark" : settings_.theme == L"dark" ? L"light" : L"system";
        applyTheme(); persist(); refresh();
    } else if (id == L"setting:language") {
        settings_.language = settings_.language == L"system" ? L"zh-CN" : settings_.language == L"zh-CN" ? L"en" : L"system";
        applyTheme(); persist(); refresh();
    } else if (id == L"setting:hotkey") {
        const auto oldModifiers = settings_.hotkeyModifiers, oldKey = settings_.hotkeyKey;
        settings_.hotkeyModifiers = oldModifiers == (MOD_CONTROL | MOD_ALT) ? MOD_ALT : oldModifiers == MOD_ALT ? MOD_WIN | MOD_SHIFT : MOD_CONTROL | MOD_ALT;
        settings_.hotkeyKey = VK_SPACE;
        if (hotkeyRegistered_) UnregisterHotKey(window_, HotkeyID);
        hotkeyRegistered_ = false; registerShortcut();
        if (!hotkeyRegistered_ && !options_.smoke) {
            settings_.hotkeyModifiers = oldModifiers; settings_.hotkeyKey = oldKey; registerShortcut();
            updateStatus(tr(L"That shortcut is unavailable; the previous shortcut was kept.", L"该快捷键不可用，已保留原快捷键。"));
        } else persist();
        refresh();
    } else if (id == L"setting:clipboard") {
        if (!settings_.clipboardEnabled && !confirmed) {
            auto pending = entry;
            pending.title = tr(L"Enable clipboard history", L"开启剪贴板历史");
            pending.subtitle = tr(L"Future text copies are recorded in memory only. Sensitive text may be included.", L"之后复制的文本会被记录，仅驻留内存；其中可能包含敏感内容。");
            requestConfirmation(std::move(pending)); return;
        }
        settings_.clipboardEnabled = !settings_.clipboardEnabled;
        clipboardSubscription(); persist(); refresh();
    } else if (id == L"setting:clear") {
        if (!confirmed) { requestConfirmation(entry); return; }
        clips_.clear(); refresh(); updateStatus(tr(L"Recorded history cleared. The system clipboard was not changed.", L"已清空历史，系统剪贴板内容保持不变。"));
    } else if (id == L"setting:startup") {
        if (options_.smoke) return;
        if (!setLaunchAtLogin(!launchAtLoginEnabled(), error)) updateStatus(error);
        refresh();
    } else if (id == L"setting:folder") {
        modal_ = true; const auto path = chooseFolder(window_); modal_ = false;
        if (!path.empty() && settings_.fileRoots.size() < 32 &&
            std::find(settings_.fileRoots.begin(), settings_.fileRoots.end(), path) == settings_.fileRoots.end()) {
            settings_.fileRoots.push_back(path); persist(); updateStatus(tr(L"Search folder added.", L"已添加搜索文件夹。"));
        }
        SetFocus(search_);
    } else if (id == L"setting:json") {
        std::error_code ec;
        if (!std::filesystem::exists(settingsPath(), ec)) persist();
        hide(false);
        if (!openTarget(window_, L"notepad.exe", L"\"" + settingsPath() + L"\"", error)) { show(false); updateStatus(error); }
    } else if (id == L"setting:reload") {
        std::wstring warning;
        auto candidate = loadSettings(warning);
        if (!warning.empty()) { configWarning_ = warning; updateStatus(warning); return; }
        const auto oldModifiers = settings_.hotkeyModifiers, oldKey = settings_.hotkeyKey;
        settings_ = std::move(candidate); configWarning_.clear();
        if (hotkeyRegistered_) UnregisterHotKey(window_, HotkeyID);
        hotkeyRegistered_ = false; registerShortcut();
        if (!hotkeyRegistered_ && !options_.smoke) {
            settings_.hotkeyModifiers = oldModifiers; settings_.hotkeyKey = oldKey; registerShortcut();
            updateStatus(tr(L"Settings loaded; unavailable shortcut was not applied.", L"设置已加载，但冲突的快捷键未应用。"));
        }
        clipboardSubscription(); applyTheme(); refresh();
    } else if (id == L"setting:about") {
        updateStatus(tr(L"Windows preview 0.1.0 · Native C++/Win32 · Raycast extensions, AI/MCP and image history are not yet ported.",
                        L"Windows 预览版 0.1.0 · 原生 C++/Win32 · 尚未移植 Raycast 扩展、AI/MCP 与图片历史。"));
    } else if (id == L"setting:source") {
        hide(false); openTarget(window_, L"https://github.com/nonlog/tinycast/tree/main/windows", L"", error);
    } else if (id == L"setting:quit") DestroyWindow(window_);
}

void App::persist() {
    if (options_.smoke) return;
    if (!configWarning_.empty()) { updateStatus(configWarning_); return; }
    std::wstring error;
    if (!saveSettings(settings_, error)) updateStatus(error);
}

void App::updateStatus(std::wstring text) {
    status_ = std::move(text);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::toggleFavorite() {
    const auto selected = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR || static_cast<std::size_t>(selected) >= visible_.size()) return;
    const auto& entry = visible_[static_cast<std::size_t>(selected)];
    if (entry.kind == EntryKind::Clipboard || entry.kind == EntryKind::Calculator || mode_ == Mode::Confirm) return;
    if (auto it = std::find(settings_.favorites.begin(), settings_.favorites.end(), entry.id); it != settings_.favorites.end()) settings_.favorites.erase(it);
    else if (settings_.favorites.size() < 4096) settings_.favorites.push_back(entry.id);
    persist(); refresh();
}

void App::tray(bool add) {
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = window_; icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    icon.hIcon = appIcon_; icon.uCallbackMessage = TrayMessage;
    wcscpy_s(icon.szTip, L"Tinycast Windows Preview");
    if (add) {
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
        icon.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &icon);
    } else { Shell_NotifyIconW(NIM_DELETE, &icon); trayAdded_ = false; }
}

void App::trayMenu() {
    const auto menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, tr(L"Open Tinycast", L"打开 Tinycast"));
    AppendMenuW(menu, MF_STRING, 2, tr(L"Settings", L"设置"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, tr(L"Quit", L"退出"));
    POINT point{}; GetCursorPos(&point); SetForegroundWindow(window_);
    modal_ = true;
    const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
    modal_ = false; DestroyMenu(menu);
    if (choice == 1) { setMode(Mode::Home); show(); }
    else if (choice == 2) { setMode(Mode::Settings); show(); }
    else if (choice == 3) DestroyWindow(window_);
    PostMessageW(window_, WM_NULL, 0, 0);
}

void App::registerShortcut() {
    if (options_.smoke) return;
    hotkeyRegistered_ = RegisterHotKey(window_, HotkeyID, settings_.hotkeyModifiers | MOD_NOREPEAT, settings_.hotkeyKey) != FALSE;
    if (!hotkeyRegistered_) updateStatus(tr(L"Shortcut unavailable. Open Settings from the tray and choose another shortcut.", L"快捷键不可用，请从托盘打开设置并选择其他快捷键。"));
}

void App::clipboardSubscription() {
    if (options_.smoke) return;
    if (clipboardListening_ && !settings_.clipboardEnabled) {
        RemoveClipboardFormatListener(window_); clipboardListening_ = false;
    } else if (!clipboardListening_ && settings_.clipboardEnabled) {
        ignoredClipboardSequence_ = GetClipboardSequenceNumber();
        clipboardListening_ = AddClipboardFormatListener(window_) != FALSE;
        if (!clipboardListening_) updateStatus(tr(L"Windows could not enable clipboard notifications.", L"Windows 无法启用剪贴板通知。"));
    }
}

void App::captureClipboard() {
    if (!settings_.clipboardEnabled || options_.smoke || GetClipboardSequenceNumber() == ignoredClipboardSequence_) return;
    const auto value = readClipboardText(window_);
    if (!value) return;
    std::erase(clips_, *value); clips_.insert(clips_.begin(), *value);
    std::size_t total = 0, keep = 0;
    for (const auto& clip : clips_) {
        total += clip.size() * sizeof(wchar_t);
        if (total > 8 * 1024 * 1024 || keep >= settings_.clipboardLimit) break;
        ++keep;
    }
    clips_.resize(keep);
    if (mode_ == Mode::Clipboard) refresh();
}

void App::refreshApplications() {
    if (indexing_ || options_.smoke) return;
    if (appWorker_.joinable()) appWorker_.join();
    indexing_ = true; updateStatus(tr(L"Refreshing application index…", L"正在刷新应用索引…"));
    const auto window = window_;
    appWorker_ = std::jthread([window](std::stop_token stop) {
        auto entries = std::make_unique<std::vector<Entry>>();
        bool failed = false;
        try { *entries = scanApplications(stop); } catch (...) { failed = true; }
        if (!stop.stop_requested() && PostMessageW(window, AppsReady, failed, reinterpret_cast<LPARAM>(entries.get()))) entries.release();
    });
}

void App::startFileWorker() {
    fileWorker_ = std::jthread([this, window = window_](std::stop_token stop) {
        while (!stop.stop_requested()) {
            FileJob job;
            std::stop_token jobStop;
            {
                std::unique_lock lock(fileMutex_);
                if (!fileCondition_.wait(lock, stop, [&] { return fileJob_.has_value(); })) break;
                job = std::move(*fileJob_); fileJob_.reset();
                activeFileStop_ = std::stop_source{}; jobStop = activeFileStop_.get_token();
            }
            auto result = std::make_unique<FileResult>(); result->generation = job.generation;
            try { result->entries = searchFiles(job.roots, job.query, jobStop); } catch (...) { result->failed = true; }
            if (!stop.stop_requested() && !jobStop.stop_requested() && PostMessageW(window, FilesReady, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        }
    });
}

void App::scheduleFiles() {
    ++fileGeneration_;
    { std::lock_guard lock(fileMutex_); activeFileStop_.request_stop(); fileJob_.reset(); }
    fileResults_.clear(); filesBusy_ = false;
    KillTimer(window_, FileTimer);
    if (mode_ == Mode::Files && trimText(query()).size() >= 2 && !options_.smoke) {
        filesBusy_ = true; SetTimer(window_, FileTimer, 140, nullptr);
    }
}

void App::launchFileJob() {
    KillTimer(window_, FileTimer);
    if (mode_ != Mode::Files || !IsWindowVisible(window_)) return;
    { std::lock_guard lock(fileMutex_); fileJob_ = FileJob{trimText(query()), settings_.fileRoots, fileGeneration_}; }
    fileCondition_.notify_all();
}

void App::drawText(HDC dc, std::wstring_view text, RECT rect, HFONT font, COLORREF color, UINT format) const {
    const auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX);
    SelectObject(dc, previous);
}

void App::rounded(HDC dc, RECT rect, int radius, COLORREF fill) const {
    const auto brush = CreateSolidBrush(fill);
    const auto pen = CreatePen(PS_SOLID, 1, fill);
    const auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}

HICON App::entryIcon(const Entry& entry) {
    if (entry.kind != EntryKind::Application && entry.kind != EntryKind::File) return nullptr;
    if (options_.smoke) return nullptr;
    if (const auto found = iconCache_.find(entry.id); found != iconCache_.end()) return found->second;
    if (iconCache_.size() >= 128) {
        for (const auto& [key, icon] : iconCache_) if (icon) DestroyIcon(icon);
        iconCache_.clear();
    }
    auto icon = loadEntryIcon(entry, px(28)); iconCache_[entry.id] = icon; return icon;
}

void App::drawRow(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1) || item.itemID >= visible_.size()) return;
    const auto& entry = visible_[item.itemID];
    RECT rect = item.rcItem;
    FillRect(item.hDC, &rect, background_);
    rect.left += px(2); rect.right -= px(4); rect.top += px(2); rect.bottom -= px(2);
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    if (selected) rounded(item.hDC, rect, px(10), theme_.selection);
    const int iconX = rect.left + px(12), iconY = rect.top + px(11);
    if (const auto icon = entryIcon(entry)) DrawIconEx(item.hDC, iconX, iconY, icon, px(28), px(28), 0, nullptr, DI_NORMAL);
    else {
        RECT tile{iconX, iconY, iconX + px(28), iconY + px(28)};
        rounded(item.hDC, tile, px(6), theme_.control);
        const wchar_t* glyph = L"\uE8A5";
        if (entry.kind == EntryKind::Application) glyph = L"\uE71D";
        else if (entry.kind == EntryKind::File) glyph = L"\uE8B7";
        else if (entry.kind == EntryKind::Clipboard) glyph = L"\uE77F";
        else if (entry.kind == EntryKind::Calculator) glyph = L"\uE8EF";
        else if (entry.kind == EntryKind::Quicklink) glyph = L"\uE71B";
        else if (entry.kind == EntryKind::Snippet) glyph = L"\uE8A5";
        else if (entry.id.find(L"setting") != std::wstring::npos) glyph = L"\uE713";
        else if (entry.id.starts_with(L"window:")) glyph = L"\uE737";
        drawText(item.hDC, glyph, tile, symbolFont_, theme_.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RECT title{rect.left + px(54), rect.top + px(6), rect.right - px(48), rect.top + px(27)};
    drawText(item.hDC, entry.title, title, titleFont_, theme_.text);
    RECT detail{title.left, rect.top + px(28), rect.right - px(34), rect.bottom - px(5)};
    drawText(item.hDC, oneLine(entry.subtitle, 300), detail, detailFont_, theme_.muted);
    RECT trailing{rect.right - px(35), rect.top, rect.right - px(10), rect.bottom};
    if (entry.pinned) drawText(item.hDC, L"\uE718", trailing, symbolFont_, theme_.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    else if (selected) drawText(item.hDC, L"↵", trailing, titleFont_, theme_.faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void App::paint() {
    PAINTSTRUCT paint{}; const auto dc = BeginPaint(window_, &paint);
    RECT area{}; GetClientRect(window_, &area);
    const auto buffer = CreateCompatibleDC(dc);
    const auto bitmap = CreateCompatibleBitmap(dc, area.right, area.bottom);
    const auto old = SelectObject(buffer, bitmap); FillRect(buffer, &area, background_);
    RECT searchIcon{px(23), px(26), px(48), px(59)};
    drawText(buffer, mode_ == Mode::Home ? L"\uE721" : L"\uE72B", searchIcon, symbolFont_, theme_.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT heading{px(28), px(73), area.right - px(130), px(94)};
    drawText(buffer, modeName(), heading, detailFont_, theme_.muted);
    RECT count{area.right - px(122), px(73), area.right - px(28), px(94)};
    drawText(buffer, filesBusy_ || indexing_ ? tr(L"Searching…", L"搜索中…") : std::to_wstring(visible_.size()), count, detailFont_, theme_.faint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    const int footer = area.bottom - px(60);
    RECT gear{px(20), footer, px(54), footer + px(34)};
    rounded(buffer, gear, px(17), theme_.control);
    drawText(buffer, L"\uE713", gear, symbolFont_, theme_.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT brand{px(67), footer - px(1), area.right - px(185), footer + px(20)};
    drawText(buffer, L"Tinycast", brand, titleFont_, theme_.text);
    RECT hint{px(67), footer + px(20), area.right - px(185), footer + px(37)};
    drawText(buffer, tr(L"Windows preview · Ctrl+D favorite · Esc back", L"Windows 预览版 · Ctrl+D 收藏 · Esc 返回"), hint, detailFont_, theme_.faint);
    RECT action{area.right - px(162), footer, area.right - px(22), footer + px(35)};
    rounded(buffer, action, px(17), theme_.control);
    drawText(buffer, tr(L"Open / Run   ↵", L"打开 / 执行   ↵"), action, detailFont_, theme_.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (!status_.empty()) {
        RECT status{px(26), area.bottom - px(82), area.right - px(26), area.bottom - px(64)};
        drawText(buffer, status_, status, detailFont_, theme_.accent);
    }
    BitBlt(dc, 0, 0, area.right, area.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, old); DeleteObject(bitmap); DeleteDC(buffer); EndPaint(window_, &paint);
}

bool App::key(WPARAM key) {
    if (composing_ || GetTickCount64() < imeCommitUntil_ || key == VK_PROCESSKEY) return false;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (key == VK_DOWN || key == VK_UP) {
        if (visible_.empty()) return true;
        auto selected = SendMessageW(list_, LB_GETCURSEL, 0, 0);
        if (selected == LB_ERR) selected = 0;
        selected = std::clamp<LRESULT>(selected + (key == VK_DOWN ? 1 : -1), 0, static_cast<LRESULT>(visible_.size() - 1));
        SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(selected), 0);
        InvalidateRect(list_, nullptr, FALSE); return true;
    }
    if (key == VK_RETURN) { activate(control); return true; }
    if (key == VK_ESCAPE) {
        if (mode_ == Mode::Confirm) { setMode(confirmReturn_); SetWindowTextW(search_, confirmQuery_.c_str()); }
        else if (!query().empty()) SetWindowTextW(search_, L"");
        else if (mode_ != Mode::Home) setMode(Mode::Home);
        else hide();
        return true;
    }
    if (key == VK_TAB && settings_.clipboardEnabled) { setMode(mode_ == Mode::Clipboard ? Mode::Home : Mode::Clipboard); return true; }
    if (control && key == VK_OEM_COMMA) { setMode(Mode::Settings); return true; }
    if (control && key == L'D') { toggleFavorite(); return true; }
    if (control && key == L'A' && GetFocus() == search_) { SendMessageW(search_, EM_SETSEL, 0, -1); return true; }
    return false;
}

LRESULT CALLBACK App::editProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& app = *reinterpret_cast<App*>(data);
    if (message == WM_IME_STARTCOMPOSITION) app.composing_ = true;
    if (message == WM_IME_ENDCOMPOSITION) {
        app.composing_ = false; app.imeCommitUntil_ = GetTickCount64() + 120;
        PostMessageW(app.window_, RefreshMessage, 0, 0);
    }
    if (message == WM_KEYDOWN && app.key(wParam)) return 0;
    if (message == WM_CHAR && !app.composing_ && (wParam == L'\r' || wParam == L'\t' || wParam == VK_ESCAPE)) return 0;
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK App::listProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& app = *reinterpret_cast<App*>(data);
    if (message == WM_KEYDOWN && app.key(wParam)) return 0;
    if (message == WM_PAINT && app.visible_.empty()) {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint);
        RECT rect{}; GetClientRect(window, &rect); FillRect(dc, &rect, app.background_);
        rect.left += app.px(20); rect.right -= app.px(20);
        app.drawText(dc, app.emptyMessage(), rect, app.titleFont_, app.theme_.muted,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_ERASEBKGND) {
        RECT rect{}; GetClientRect(window, &rect); FillRect(reinterpret_cast<HDC>(wParam), &rect, app.background_); return 1;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK App::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->message(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::message(UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreated_ && message == taskbarCreated_ && !options_.smoke) { trayAdded_ = false; tray(true); return 0; }
    switch (message) {
    case WM_NCCALCSIZE: if (wParam) return 0; break;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: if (background_) paint(); else { PAINTSTRUCT p{}; BeginPaint(window_, &p); EndPaint(window_, &p); } return 0;
    case WM_SIZE: if (search_) layout(); return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam); makeFonts();
        for (const auto& [key, icon] : iconCache_) if (icon) DestroyIcon(icon);
        iconCache_.clear();
        const auto* rect = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE: if (search_) { applyTheme(); refresh(); } return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && !modal_ && !options_.smoke && !composing_) hide(false);
        return 0;
    case WM_CLOSE: hide(); return 0;
    case WM_DESTROY: shutdown(); PostQuitMessage(0); return 0;
    case WM_HOTKEY:
        if (IsWindowVisible(window_)) hide(); else { setMode(Mode::Home); show(); } return 0;
    case ShowMessage: setMode(Mode::Home); show(); return 0;
    case WM_CLIPBOARDUPDATE: captureClipboard(); return 0;
    case WM_KEYDOWN: if (key(wParam)) return 0; break;
    case WM_COMMAND:
        if (LOWORD(wParam) == SearchID && HIWORD(wParam) == EN_CHANGE && !composing_) {
            status_.clear(); if (mode_ == Mode::Files) scheduleFiles(); refresh();
        } else if (LOWORD(wParam) == ListID && HIWORD(wParam) == LBN_DBLCLK) activate();
        return 0;
    case RefreshMessage: if (mode_ == Mode::Files) scheduleFiles(); refresh(); return 0;
    case WM_DRAWITEM: if (wParam == ListID) { drawRow(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE; } break;
    case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = px(54); return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: SetTextColor(reinterpret_cast<HDC>(wParam), theme_.text); SetBkColor(reinterpret_cast<HDC>(wParam), theme_.background); return reinterpret_cast<LRESULT>(background_);
    case WM_LBUTTONUP: {
        RECT area{}; GetClientRect(window_, &area); const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (x < px(58) && y > area.bottom - px(65)) { setMode(Mode::Settings); return 0; }
        if (x > area.right - px(170) && y > area.bottom - px(65)) { activate(); return 0; }
        if (x < px(58) && y < px(65) && mode_ != Mode::Home) { setMode(Mode::Home); return 0; }
        SetFocus(search_); return 0;
    }
    case WM_TIMER:
        if (wParam == FileTimer) launchFileJob();
        else if (wParam == SmokeTimer) { KillTimer(window_, SmokeTimer); smokeTest(); }
        return 0;
    case AppsReady: {
        std::unique_ptr<std::vector<Entry>> entries(reinterpret_cast<std::vector<Entry>*>(lParam));
        indexing_ = false;
        if (!wParam) applications_ = std::move(*entries);
        refresh(); updateStatus(wParam ? tr(L"Application scan failed; the previous index was kept.", L"应用扫描失败，已保留原索引。") : tr(L"Application index ready.", L"应用索引已就绪。"));
        return 0;
    }
    case FilesReady: {
        std::unique_ptr<FileResult> result(reinterpret_cast<FileResult*>(lParam));
        if (mode_ == Mode::Files && result->generation == fileGeneration_) {
            fileResults_ = std::move(result->entries); filesBusy_ = false; refresh();
            updateStatus(result->failed ? tr(L"File search failed.", L"文件搜索失败。") : tr(L"Bounded results: up to 20,000 entries / 1.5 seconds. Not a complete disk index.", L"限额结果：最多扫描 2 万项 / 1.5 秒，并非完整磁盘索引。"));
        }
        return 0;
    }
    case TrayMessage:
        if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) trayMenu();
        else if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) { setMode(Mode::Home); show(); }
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

bool App::captureBitmap(const std::wstring& path) {
    RECT rect{}; GetClientRect(window_, &rect);
    const int width = rect.right, height = rect.bottom;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels{}; const auto dc = GetDC(window_), memory = CreateCompatibleDC(dc);
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !memory || !pixels) { if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); ReleaseDC(window_, dc); return false; }
    const auto old = SelectObject(memory, bitmap);
    SendMessageW(window_, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    BitBlt(memory, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    const auto bytes = static_cast<DWORD>(width * height * 4); file.bfSize = file.bfOffBits + bytes;
    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    output.write(static_cast<const char*>(pixels), bytes);
    const bool success = output.good();
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window_, dc);
    return success;
}

void App::diagnostics(int failures) {
    if (options_.diagnostics.empty()) return;
    PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    std::ofstream output(std::filesystem::path(options_.diagnostics), std::ios::binary);
    output << "{\n  \"smokeFailures\": " << failures << ",\n  \"workingSetBytes\": " << memory.WorkingSetSize
           << ",\n  \"privateBytes\": " << memory.PrivateUsage << ",\n  \"webViewRuntime\": false,\n  \"clipboardListening\": "
           << (clipboardListening_ ? "true" : "false") << ",\n  \"dpi\": " << dpi_ << "\n}\n";
}

void App::smokeTest() {
    int failures = 0;
    SetWindowTextW(search_, L"2+3*4");
    if (visible_.empty() || visible_[0].kind != EntryKind::Calculator || visible_[0].target != L"14") ++failures;
    SetWindowTextW(search_, L"记事本");
    if (query() != L"记事本" || visible_.empty() || visible_[0].title != L"记事本") ++failures;
    composing_ = true;
    if (key(VK_RETURN)) ++failures;
    composing_ = false;
    if (clipboardListening_ || hotkeyRegistered_) ++failures;
    SetWindowTextW(search_, L""); refresh();
    updateStatus(tr(L"Native Windows preview · No WebView · Clipboard history is off", L"原生 Windows 预览版 · 无 WebView · 剪贴板历史已关闭"));
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    if (!options_.capture.empty() && !captureBitmap(options_.capture)) ++failures;
    diagnostics(failures);
    DestroyWindow(window_); PostQuitMessage(failures == 0 ? 0 : 1);
}

}
