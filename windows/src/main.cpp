#include "palette.hpp"
#include <shellapi.h>
#include <filesystem>
#include <stdexcept>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int exitCode = 1;
    HANDLE mutex{};
    std::wstring smokeDirectory;
    bool smoke = false;
    try {
        tinycast::Options options;
        int count{}; auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) throw std::runtime_error("Cannot parse arguments");
        for (int i = 1; i < count; ++i) {
            const std::wstring arg(args[i]);
            if (arg == L"--background") options.background = true;
            else if (arg == L"--smoke-test") options.smoke = smoke = true;
            else if (arg == L"--capture" && i + 1 < count) options.capture = args[++i];
            else if (arg == L"--diagnostics" && i + 1 < count) options.diagnostics = args[++i];
            else if (arg == L"--theme" && i + 1 < count) options.theme = args[++i];
        }
        LocalFree(args);
        if (options.smoke) {
            wchar_t temp[32768]{};
            if (!GetTempPathW(static_cast<DWORD>(std::size(temp)), temp)) throw std::runtime_error("No temporary directory");
            smokeDirectory = (std::filesystem::path(temp) / (L"tinycast-smoke-" + std::to_wstring(GetCurrentProcessId()))).wstring();
            SetEnvironmentVariableW(L"TINYCAST_DATA_DIR", smokeDirectory.c_str());
        }
        const auto profile = tinycast::normalize(tinycast::dataDirectory());
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto ch : profile) { hash ^= static_cast<std::uint64_t>(ch); hash *= 1099511628211ull; }
        const auto className = L"TinycastWindows.Palette." + std::to_wstring(hash);
        const auto mutexName = L"Local\\TinycastWindows." + std::to_wstring(hash);
        mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (!mutex) throw std::runtime_error("Cannot create instance lock");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (const auto previous = FindWindowW(className.c_str(), nullptr)) {
                DWORD pid{}; GetWindowThreadProcessId(previous, &pid); AllowSetForegroundWindow(pid);
                PostMessageW(previous, tinycast::App::ShowMessage, 0, 0);
            }
            exitCode = 0;
        } else {
            tinycast::App app(std::move(options));
            if (!app.create(instance, className)) throw std::runtime_error("Cannot create the native window");
            exitCode = app.run();
        }
    } catch (...) {
        if (!smoke) MessageBoxW(nullptr, L"Tinycast could not start. Verify that this is a complete Windows build and that your profile folder is writable.",
                                L"Tinycast Windows", MB_ICONERROR | MB_OK);
        exitCode = 2;
    }
    if (mutex) CloseHandle(mutex);
    if (!smokeDirectory.empty()) { std::error_code error; std::filesystem::remove_all(smokeDirectory, error); }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return exitCode;
}
