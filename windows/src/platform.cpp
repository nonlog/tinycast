#include "platform.hpp"

#include <dwmapi.h>
#include <knownfolders.h>
#include <roapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>

namespace tinycast {
namespace {
namespace fs = std::filesystem;
using namespace winrt::Windows::Data::Json;

struct Apartment {
    HRESULT result{RoInitialize(RO_INIT_SINGLETHREADED)};
    ~Apartment() { if (SUCCEEDED(result)) RoUninitialize(); }
};

std::mutex& jsonMutex() {
    static std::mutex mutex;
    return mutex;
}

// Settings values never expose WinRT objects. Serialize their activation scope and
// release cached factories before its last COM reference can disappear. Otherwise
// a later JsonObject::Parse can call a factory freed by RoUninitialize.
// See microsoft/cppwinrt's clear_factory_cache and Old New Thing 2021-11-05.
struct JsonSession {
    std::unique_lock<std::mutex> lock{jsonMutex()};
    Apartment apartment;
    ~JsonSession() { winrt::clear_factory_cache(); }
};
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
};

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) return {};
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

std::wstring executablePath() {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size == path.size()) return {};
    path.resize(size);
    return path;
}

std::string utf8(std::wstring_view value) {
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size && !value.empty()) throw std::runtime_error("Invalid Unicode");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (size) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                 static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring fromUtf8(std::string_view value) {
    if (value.starts_with("\xef\xbb\xbf")) value.remove_prefix(3);
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (!size && !value.empty()) throw std::runtime_error("Invalid UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                 static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring jsonString(const JsonObject& object, std::wstring_view key, std::wstring_view fallback = L"",
                         std::size_t maximum = 32768) {
    const winrt::hstring hkey(key);
    if (!object.HasKey(hkey)) return std::wstring(fallback);
    const auto value = object.GetNamedString(hkey);
    if (value.size() > maximum || std::wstring_view(value).find(L'\0') != std::wstring_view::npos)
        throw std::runtime_error("Invalid settings string");
    return std::wstring(value);
}

unsigned jsonNumber(const JsonObject& object, std::wstring_view key, unsigned fallback,
                     unsigned minimum, unsigned maximum) {
    const winrt::hstring hkey(key);
    if (!object.HasKey(hkey)) return fallback;
    const double value = object.GetNamedNumber(hkey);
    if (!std::isfinite(value) || value < minimum || value > maximum || std::floor(value) != value)
        throw std::runtime_error("Invalid settings number");
    return static_cast<unsigned>(value);
}

void putString(JsonObject& object, std::wstring_view key, std::wstring_view value) {
    object.Insert(winrt::hstring(key), JsonValue::CreateStringValue(winrt::hstring(value)));
}

JsonArray readArray(const JsonObject& root, std::wstring_view name, unsigned maximum = 512) {
    const winrt::hstring key(name);
    if (!root.HasKey(key)) return JsonArray{};
    const auto array = root.GetNamedArray(key);
    if (array.Size() > maximum) throw std::runtime_error("Settings list is too large");
    return array;
}

Settings defaults() {
    Settings settings;
    for (const auto* id : {&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads}) {
        auto path = knownFolder(*id);
        if (!path.empty()) settings.fileRoots.push_back(std::move(path));
    }
    settings.quicklinks = {{L"Google", L"g", L"https://www.google.com/search?q={query}"},
                           {L"GitHub", L"gh", L"https://github.com/search?q={query}"}};
    return settings;
}

bool localDirectory(const fs::path& path) {
    const auto spelling = path.wstring();
    if (spelling.starts_with(L"\\\\")) return false;
    const auto attributes = GetFileAttributesW(spelling.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE))) return false;
    return GetDriveTypeW(path.root_path().c_str()) != DRIVE_REMOTE;
}

bool excludedClipboardOwner() {
    DWORD pid{};
    GetWindowThreadProcessId(GetClipboardOwner(), &pid);
    if (!pid) return false;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process.value) return false;
    wchar_t path[32768]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    if (!QueryFullProcessImageNameW(process.value, 0, path, &size)) return false;
    const auto name = normalize(fs::path(path).filename().wstring());
    return name == L"keepass.exe" || name == L"keepassxc.exe" ||
           name == L"1password.exe" || name == L"bitwarden.exe";
}

bool excludedClipboardFormat() {
    for (const auto* name : {L"ExcludeClipboardContentFromMonitorProcessing", L"Clipboard Viewer Ignore"})
        if (IsClipboardFormatAvailable(RegisterClipboardFormatW(name))) return true;
    const auto format = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (IsClipboardFormatAvailable(format)) {
        const auto handle = GetClipboardData(format);
        if (handle && GlobalSize(handle) >= sizeof(DWORD)) {
            const auto* value = static_cast<const DWORD*>(GlobalLock(handle));
            if (value) {
                const bool excluded = *value == 0;
                GlobalUnlock(handle);
                if (excluded) return true;
            }
        }
    }
    return false;
}

bool isShellSurface(HWND window) {
    if (window == GetDesktopWindow() || window == GetShellWindow()) return true;
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0 ||
           wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0;
}

}

std::wstring errorMessage(unsigned long code) {
    wchar_t* buffer{};
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                       reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length ? std::wstring(buffer, length) : L"Windows error " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!message.empty() && std::iswspace(message.back())) message.pop_back();
    return message;
}

std::wstring dataDirectory() {
    const auto length = GetEnvironmentVariableW(L"TINYCAST_DATA_DIR", nullptr, 0);
    if (length > 0 && length <= 32768) {
        std::wstring value(length, L'\0');
        const auto copied = GetEnvironmentVariableW(L"TINYCAST_DATA_DIR", value.data(), length);
        value.resize(copied);
        if (!value.empty() && fs::path(value).is_absolute()) return value;
    }
    const auto base = knownFolder(FOLDERID_LocalAppData);
    if (base.empty()) throw std::runtime_error("Local AppData is unavailable");
    return (fs::path(base) / L"TinycastWindows").wstring();
}

std::wstring settingsPath() { return (fs::path(dataDirectory()) / L"settings.json").wstring(); }

Settings loadSettings(std::wstring& warning) {
    warning.clear();
    Settings settings = defaults();
    try {
        JsonSession session;
        const auto path = settingsPath();
        Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return settings;
            throw std::runtime_error("Cannot read settings");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size) || size.QuadPart > 2 * 1024 * 1024 || size.QuadPart <= 0)
            throw std::runtime_error("Invalid settings size");
        std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read{};
        if (!ReadFile(file.value, content.data(), static_cast<DWORD>(content.size()), &read, nullptr) || read != content.size())
            throw std::runtime_error("Cannot read settings");
        const auto root = JsonObject::Parse(winrt::hstring(fromUtf8(content)));
        jsonNumber(root, L"schemaVersion", 1, 1, 1);
        settings.theme = jsonString(root, L"theme", L"system", 16);
        if (settings.theme != L"system" && settings.theme != L"dark" && settings.theme != L"light")
            throw std::runtime_error("Unknown theme");
        settings.language = jsonString(root, L"language", L"system", 16);
        if (settings.language != L"system" && settings.language != L"en" && settings.language != L"zh-CN")
            throw std::runtime_error("Unknown language");
        settings.hotkeyModifiers = jsonNumber(root, L"hotkeyModifiers", MOD_CONTROL | MOD_ALT, 1, 15);
        settings.hotkeyKey = jsonNumber(root, L"hotkeyKey", VK_SPACE, 8, 254);
        settings.clipboardEnabled = root.GetNamedBoolean(L"clipboardEnabled", false);
        settings.clipboardLimit = jsonNumber(root, L"clipboardLimit", 100, 10, 500);
        settings.windowGap = jsonNumber(root, L"windowGap", 8, 0, 64);
        if (root.HasKey(L"fileRoots")) {
            settings.fileRoots.clear();
            for (const auto& value : readArray(root, L"fileRoots", 32)) {
                const auto folder = value.GetString();
                if (folder.size() > 32767 || !fs::path(std::wstring(folder)).is_absolute())
                    throw std::runtime_error("File roots must be absolute paths");
                settings.fileRoots.emplace_back(folder);
            }
        }
        if (root.HasKey(L"quicklinks")) {
            settings.quicklinks.clear();
            for (const auto& value : readArray(root, L"quicklinks")) {
                const auto item = value.GetObject();
                auto name = jsonString(item, L"name", L"", 256);
                auto keyword = jsonString(item, L"keyword", L"", 32);
                auto url = jsonString(item, L"url", L"", 8192);
                if (name.empty() || url.empty() || keyword.find_first_of(L" \t\r\n") != std::wstring::npos)
                    throw std::runtime_error("Invalid quicklink");
                const auto folded = normalize(url);
                if (!folded.starts_with(L"https://") && !folded.starts_with(L"http://"))
                    throw std::runtime_error("Quicklinks require an HTTP(S) URL");
                settings.quicklinks.push_back({std::move(name), std::move(keyword), std::move(url)});
            }
        }
        for (const auto& value : readArray(root, L"snippets")) {
            const auto item = value.GetObject();
            auto name = jsonString(item, L"name", L"", 256);
            auto text = jsonString(item, L"text", L"", 65536);
            if (name.empty()) throw std::runtime_error("A snippet needs a name");
            settings.snippets.push_back({std::move(name), std::move(text)});
        }
        for (const auto& value : readArray(root, L"commands")) {
            const auto item = value.GetObject();
            auto name = jsonString(item, L"name", L"", 256);
            auto executable = jsonString(item, L"executable");
            auto arguments = jsonString(item, L"arguments");
            if (name.empty() || executable.empty()) throw std::runtime_error("Invalid custom command");
            settings.commands.push_back({std::move(name), std::move(executable), std::move(arguments)});
        }
        for (const auto& value : readArray(root, L"favorites", 4096)) {
            const auto text = value.GetString();
            if (text.size() > 32768) throw std::runtime_error("Invalid favorite");
            settings.favorites.emplace_back(text);
        }
        if (root.HasKey(L"usage")) {
            const auto usage = root.GetNamedObject(L"usage");
            if (usage.Size() > 4096) throw std::runtime_error("Usage list too large");
            for (const auto& pair : usage) {
                const double value = pair.Value().GetNumber();
                if (!std::isfinite(value) || value < 0 || value > 1000000)
                    throw std::runtime_error("Invalid usage count");
                settings.usage[std::wstring(pair.Key())] = static_cast<std::uint32_t>(value);
            }
        }
        return settings;
    } catch (...) {
        warning = L"Settings could not be read. The original file was preserved; defaults are active. Open settings.json to repair it, then reload.";
        return defaults();
    }
}

bool saveSettings(const Settings& settings, std::wstring& error) {
    error.clear();
    std::wstring temporary;
    try {
        JsonSession session;
        JsonObject root;
        root.Insert(L"schemaVersion", JsonValue::CreateNumberValue(1));
        putString(root, L"theme", settings.theme);
        putString(root, L"language", settings.language);
        root.Insert(L"hotkeyModifiers", JsonValue::CreateNumberValue(settings.hotkeyModifiers));
        root.Insert(L"hotkeyKey", JsonValue::CreateNumberValue(settings.hotkeyKey));
        root.Insert(L"clipboardEnabled", JsonValue::CreateBooleanValue(settings.clipboardEnabled));
        root.Insert(L"clipboardLimit", JsonValue::CreateNumberValue(settings.clipboardLimit));
        root.Insert(L"windowGap", JsonValue::CreateNumberValue(settings.windowGap));
        JsonArray roots, links, snippets, commands, favorites;
        for (const auto& path : settings.fileRoots) roots.Append(JsonValue::CreateStringValue(winrt::hstring(path)));
        for (const auto& link : settings.quicklinks) {
            JsonObject item;
            putString(item, L"name", link.name); putString(item, L"keyword", link.keyword); putString(item, L"url", link.url);
            links.Append(item);
        }
        for (const auto& snippet : settings.snippets) {
            JsonObject item; putString(item, L"name", snippet.name); putString(item, L"text", snippet.text);
            snippets.Append(item);
        }
        for (const auto& command : settings.commands) {
            JsonObject item; putString(item, L"name", command.name); putString(item, L"executable", command.executable);
            putString(item, L"arguments", command.arguments); commands.Append(item);
        }
        for (const auto& id : settings.favorites) favorites.Append(JsonValue::CreateStringValue(winrt::hstring(id)));
        root.Insert(L"fileRoots", roots); root.Insert(L"quicklinks", links); root.Insert(L"snippets", snippets);
        root.Insert(L"commands", commands); root.Insert(L"favorites", favorites);
        JsonObject usage;
        for (const auto& [id, count] : settings.usage) usage.Insert(winrt::hstring(id), JsonValue::CreateNumberValue(count));
        root.Insert(L"usage", usage);
        const auto content = utf8(std::wstring_view(root.Stringify()));
        if (content.size() > 2 * 1024 * 1024) throw std::runtime_error("Settings too large");
        fs::create_directories(dataDirectory());
        const auto destination = settingsPath();
        temporary = destination + L".tmp." + std::to_wstring(GetCurrentProcessId());
        {
            Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create settings file");
            DWORD written{};
            if (!WriteFile(file.value, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) ||
                written != content.size() || !FlushFileBuffers(file.value))
                throw std::runtime_error("Cannot write settings file");
        }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace settings file");
        return true;
    } catch (...) {
        error = L"Could not save settings. Check the data folder's permissions and available disk space.";
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        return false;
    }
}

std::vector<Entry> scanApplications(std::stop_token stop) {
    Apartment apartment;
    std::vector<Entry> entries;
    std::set<std::wstring> seen;
    for (const auto* id : {&FOLDERID_Programs, &FOLDERID_CommonPrograms}) {
        const auto root = knownFolder(*id);
        if (root.empty()) continue;
        std::error_code error;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
        for (; it != end && !stop.stop_requested() && entries.size() < 4096; it.increment(error)) {
            if (error) { error.clear(); continue; }
            const auto path = it->path();
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) continue;
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (it.depth() >= 8 || attributes & FILE_ATTRIBUTE_REPARSE_POINT) it.disable_recursion_pending();
                continue;
            }
            const auto extension = normalize(path.extension().wstring());
            if (extension != L".lnk" && extension != L".appref-ms" && extension != L".url" && extension != L".exe") continue;
            auto name = path.stem().wstring();
            const auto folded = normalize(name);
            if (folded.find(L"uninstall") != std::wstring::npos || folded.find(L"\u5378\u8f7d") != std::wstring::npos) continue;
            if (!seen.insert(folded).second) continue;
            const auto full = path.wstring();
            entries.push_back({L"app:" + normalize(full), std::move(name), L"Application", full, L"", EntryKind::Application});
        }
    }
    winrt::com_ptr<IShellItem> folder;
    if (!stop.stop_requested() && SUCCEEDED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(folder.put())))) {
        winrt::com_ptr<IEnumShellItems> items;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(items.put())))) {
            while (!stop.stop_requested() && entries.size() < 4096) {
                winrt::com_ptr<IShellItem> item;
                if (items->Next(1, item.put(), nullptr) != S_OK) break;
                PWSTR name{}, identity{};
                winrt::com_ptr<IShellItem2> properties;
                if (FAILED(item->QueryInterface(IID_PPV_ARGS(properties.put())))) continue;
                if (FAILED(properties->GetString(PKEY_AppUserModel_ID, &identity))) continue;
                const std::wstring appID(identity);
                CoTaskMemFree(identity);
                if (appID.find(L'!') == std::wstring::npos) continue;
                if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) continue;
                std::wstring title(name);
                CoTaskMemFree(name);
                if (!seen.insert(normalize(title)).second) continue;
                entries.push_back({L"app:" + appID, std::move(title), L"Application", L"shell:AppsFolder\\" + appID,
                                   L"", EntryKind::Application});
            }
        }
    }
    return entries;
}

std::vector<Entry> searchFiles(const std::vector<std::wstring>& roots, std::wstring_view query,
                               std::stop_token stop, std::size_t limit) {
    std::vector<Entry> entries;
    if (query.size() < 2 || query.size() > 256 || limit == 0) return entries;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    std::size_t examined = 0;
    std::set<std::wstring> seen;
    for (const auto& root : roots) {
        if (stop.stop_requested() || !localDirectory(fs::path(root))) continue;
        std::error_code error;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
        for (; it != end; it.increment(error)) {
            if (stop.stop_requested() || ++examined > 20000 || std::chrono::steady_clock::now() > deadline || entries.size() >= limit)
                return entries;
            if (error) { error.clear(); continue; }
            const auto path = it->path();
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) continue;
            if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) {
                if (attributes & FILE_ATTRIBUTE_DIRECTORY) it.disable_recursion_pending();
                continue;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && it.depth() >= 8) it.disable_recursion_pending();
            const auto name = path.filename().wstring();
            if (!fuzzyScore(query, name)) continue;
            const auto full = path.wstring();
            if (!seen.insert(normalize(full)).second) continue;
            entries.push_back({L"file:" + normalize(full), name, path.parent_path().wstring(), full, L"", EntryKind::File});
        }
    }
    return entries;
}

HICON loadEntryIcon(const Entry& entry, int pixels) {
    if (entry.kind != EntryKind::Application && entry.kind != EntryKind::File) return nullptr;
    winrt::com_ptr<IShellItemImageFactory> factory;
    if (SUCCEEDED(SHCreateItemFromParsingName(entry.target.c_str(), nullptr, IID_PPV_ARGS(factory.put())))) {
        HBITMAP bitmap{};
        if (SUCCEEDED(factory->GetImage({pixels, pixels}, SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT, &bitmap))) {
            ICONINFO info{};
            info.fIcon = TRUE; info.hbmColor = bitmap;
            info.hbmMask = CreateBitmap(pixels, pixels, 1, 1, nullptr);
            const auto icon = CreateIconIndirect(&info);
            DeleteObject(bitmap);
            if (info.hbmMask) DeleteObject(info.hbmMask);
            if (icon) return icon;
        }
    }
    SHFILEINFOW info{};
    if (SHGetFileInfoW(entry.target.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON)) return info.hIcon;
    SHSTOCKICONINFO stock{sizeof(stock)};
    if (SUCCEEDED(SHGetStockIconInfo(SIID_APPLICATION, SHGSI_ICON | SHGSI_SMALLICON, &stock))) return stock.hIcon;
    return nullptr;
}

bool openTarget(HWND owner, std::wstring_view target, std::wstring_view arguments, std::wstring& error) {
    error.clear();
    if (target.empty() || target.size() > 32767 || arguments.size() > 32767 ||
        target.find(L'\0') != std::wstring_view::npos || arguments.find(L'\0') != std::wstring_view::npos) {
        error = L"Invalid launch target."; return false;
    }
    const std::wstring file(target), args(arguments);
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.hwnd = owner; info.lpVerb = L"open"; info.lpFile = file.c_str();
    info.lpParameters = args.empty() ? nullptr : args.c_str(); info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) { error = errorMessage(GetLastError()); return false; }
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

std::optional<std::wstring> readClipboardText(HWND owner) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || excludedClipboardOwner() || !OpenClipboard(owner)) return std::nullopt;
    struct Close { ~Close() { CloseClipboard(); } } close;
    if (excludedClipboardFormat()) return std::nullopt;
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return std::nullopt;
    const auto bytes = GlobalSize(handle);
    if (bytes < sizeof(wchar_t) || bytes > 131074) return std::nullopt;
    const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!text) return std::nullopt;
    const auto available = bytes / sizeof(wchar_t);
    const auto length = wcsnlen_s(text, available);
    std::optional<std::wstring> value;
    if (length > 0 && length < available && length <= 65536) value = std::wstring(text, length);
    GlobalUnlock(handle);
    return value;
}

bool copyText(HWND owner, std::wstring_view text, std::wstring& error) {
    error.clear();
    if (text.size() > 65536) { error = L"Text exceeds the 64K clipboard limit."; return false; }
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { error = L"Could not allocate clipboard memory."; return false; }
    auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
    if (!destination) { GlobalFree(memory); error = L"Could not access clipboard memory."; return false; }
    std::copy(text.begin(), text.end(), destination); destination[text.size()] = L'\0'; GlobalUnlock(memory);
    if (!OpenClipboard(owner)) { GlobalFree(memory); error = L"Clipboard is busy. Try again."; return false; }
    const bool success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!success) { GlobalFree(memory); error = L"Windows could not write to the clipboard."; }
    return success;
}

bool pasteText(HWND owner, HWND destination, std::wstring_view text, std::wstring& error) {
    error.clear();
    DWORD pid{};
    if (!IsWindow(destination) || isShellSurface(destination) || !GetWindowThreadProcessId(destination, &pid) || pid == GetCurrentProcessId()) {
        error = L"The previous application is no longer available. Use Ctrl+Enter to copy instead."; return false;
    }
    SetForegroundWindow(destination);
    if (GetForegroundWindow() != destination) {
        error = L"Could not focus the previous application. Use Ctrl+Enter to copy instead."; return false;
    }
    const auto held = [] {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
               (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000) ||
               (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    };
    for (int attempt = 0; attempt < 6 && held(); ++attempt) Sleep(20);
    if (held() || GetForegroundWindow() != destination) {
        error = L"Focus changed or modifier keys are held. Release the keys and try again."; return false;
    }
    if (!copyText(owner, text, error)) return false;
    if (GetForegroundWindow() != destination) { error = L"Focus changed. The text was copied but not pasted."; return false; }
    INPUT events[4]{};
    for (auto& event : events) event.type = INPUT_KEYBOARD;
    events[0].ki.wVk = VK_CONTROL; events[1].ki.wVk = L'V';
    events[2].ki.wVk = L'V'; events[2].ki.dwFlags = KEYEVENTF_KEYUP;
    events[3].ki.wVk = VK_CONTROL; events[3].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(4, events, sizeof(INPUT)) != 4) {
        INPUT release[2]{};
        for (auto& event : release) { event.type = INPUT_KEYBOARD; event.ki.dwFlags = KEYEVENTF_KEYUP; }
        release[0].ki.wVk = L'V'; release[1].ki.wVk = VK_CONTROL;
        SendInput(2, release, sizeof(INPUT));
        error = L"Windows blocked simulated input (possibly an elevated app). The text is copied; paste it manually."; return false;
    }
    return true;
}

bool moveWindow(HWND target, WindowAction action, unsigned gap, std::wstring& error) {
    DWORD pid{};
    if (!IsWindow(target) || isShellSurface(target) || !IsWindowVisible(target) ||
        !GetWindowThreadProcessId(target, &pid) || pid == GetCurrentProcessId()) {
        error = L"No movable application window was selected."; return false;
    }
    if (IsIconic(target) || IsZoomed(target)) ShowWindow(target, SW_RESTORE);
    RECT outer{}, visible{};
    if (!GetWindowRect(target, &outer)) { error = errorMessage(GetLastError()); return false; }
    visible = outer;
    DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible));
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &info)) {
        error = errorMessage(GetLastError()); return false;
    }
    const auto result = placeWindow(action,
        {static_cast<double>(visible.left), static_cast<double>(visible.top), static_cast<double>(visible.right - visible.left), static_cast<double>(visible.bottom - visible.top)},
        {static_cast<double>(info.rcWork.left), static_cast<double>(info.rcWork.top), static_cast<double>(info.rcWork.right - info.rcWork.left), static_cast<double>(info.rcWork.bottom - info.rcWork.top)}, gap);
    const int x = static_cast<int>(result.x) - (visible.left - outer.left);
    const int y = static_cast<int>(result.y) - (visible.top - outer.top);
    const int width = static_cast<int>(result.width) + (visible.left - outer.left) + (outer.right - visible.right);
    const int height = static_cast<int>(result.height) + (visible.top - outer.top) + (outer.bottom - visible.bottom);
    if (!SetWindowPos(target, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE)) {
        error = errorMessage(GetLastError()); return false;
    }
    return true;
}

bool launchAtLoginEnabled() {
    DWORD size{};
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                         L"TinycastWindows", RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS && size > sizeof(wchar_t);
}

bool setLaunchAtLogin(bool enabled, std::wstring& error) {
    HKEY key{};
    auto status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                  0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) { error = errorMessage(status); return false; }
    if (enabled) {
        auto path = fs::path(executablePath());
        const auto current = path.parent_path().parent_path() / L"current" / path.filename();
        std::error_code ec;
        if (normalize(path.parent_path().parent_path().filename().wstring()) == L"tinycast" && fs::exists(current, ec)) path = current;
        const auto command = L"\"" + path.wstring() + L"\" --background";
        status = RegSetValueExW(key, L"TinycastWindows", 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"TinycastWindows");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) { error = errorMessage(status); return false; }
    return true;
}

bool systemUsesDarkTheme() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

std::wstring chooseFolder(HWND owner) {
    Apartment apartment;
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) return {};
    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (FAILED(dialog->Show(owner))) return {};
    winrt::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return {};
    PWSTR path{};
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::wstring result(path); CoTaskMemFree(path); return result;
}

}
