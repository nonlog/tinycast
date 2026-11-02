#include "platform.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace tinycast;
namespace fs = std::filesystem;
namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
std::string bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

int main() {
    wchar_t temporary[32768]{};
    GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    const auto root = fs::path(temporary) / (L"tinycast-platform-test-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"TINYCAST_DATA_DIR", root.c_str());
    std::wstring error;
    try {
        auto settings = loadSettings(error);
        check(error.empty(), "missing settings use defaults without error");
        check(!settings.clipboardEnabled, "clipboard is opt-in");
        check(!fs::exists(root), "reading defaults creates no files");
        check(settings.quicklinks.size() == 2, "default quicklinks");
        check(dataDirectory() == root.wstring(), "isolated data path");
        settings.language = L"zh-CN"; settings.theme = L"dark";
        settings.snippets = {{L"\u4e2d\u6587", L"Unicode \u4e2d\u6587\n{clipboard}\n\"quoted\""}};
        settings.commands = {{L"Example", L"C:\\Program Files\\Example\\app.exe", L"--name \"two words\""}};
        settings.favorites = {L"app:test"}; settings.usage[L"app:test"] = 7;
        check(saveSettings(settings, error) && error.empty(), "atomic settings save");
        const auto loaded = loadSettings(error);
        check(error.empty(), "saved settings parse");
        check(loaded.language == L"zh-CN" && loaded.theme == L"dark", "appearance roundtrip");
        check(loaded.snippets.size() == 1 && loaded.snippets[0].text == settings.snippets[0].text, "Unicode and escapes roundtrip");
        check(loaded.commands.size() == 1 && loaded.commands[0].arguments == settings.commands[0].arguments, "command arguments not rewritten");
        check(loaded.favorites == settings.favorites && loaded.usage.at(L"app:test") == 7, "ranking state roundtrip");
        check(!fs::exists(settingsPath() + L".tmp." + std::to_wstring(GetCurrentProcessId())), "temporary save removed");
        for (const auto* malformed : {"{broken", "{\"schemaVersion\":2}", "{\"theme\":\"unknown\"}",
                                      "{\"clipboardLimit\":99999}", "{\"fileRoots\":[\"relative\"]}",
                                      "{\"quicklinks\":[{\"name\":\"x\",\"url\":\"javascript:alert(1)\"}]}"}) {
            { std::ofstream output(fs::path(settingsPath()), std::ios::binary); output << malformed; }
            const auto fallback = loadSettings(error);
            check(!error.empty() && !fallback.clipboardEnabled, "invalid settings fail closed");
            check(bytes(settingsPath()) == malformed, "invalid source file preserved");
        }
        const auto files = root / L"files";
        fs::create_directories(files / L"sub");
        { std::ofstream(files / L"report.txt") << "fixture"; std::ofstream(files / L"sub" / L"report-two.txt") << "fixture"; }
        const auto found = searchFiles({files.wstring()}, L"report", std::stop_token{}, 10);
        check(found.size() == 2, "configured-folder filename search");
        check(searchFiles({files.wstring()}, L"report", std::stop_token{}, 1).size() == 1, "file result limit");
        check(searchFiles({files.wstring()}, L"r", std::stop_token{}, 10).empty(), "short query rejected");
        std::stop_source stopped; stopped.request_stop();
        check(searchFiles({files.wstring()}, L"report", stopped.get_token(), 10).empty(), "cancelled search returns promptly");
        check(!moveWindow(nullptr, WindowAction::LeftHalf, 8, error), "invalid target window rejected");
        check(!pasteText(nullptr, nullptr, L"never copied", error), "invalid paste destination rejected before touching clipboard");
    } catch (...) { ++failures; std::cerr << "Unexpected platform-test exception\n"; }
    std::error_code cleanup;
    fs::remove_all(root, cleanup);
    check(!cleanup, "isolated files cleaned");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
