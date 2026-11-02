#pragma once

#include "platform.hpp"
#include <commctrl.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace tinycast {

struct Options {
    bool background{};
    bool smoke{};
    std::wstring capture;
    std::wstring diagnostics;
    std::wstring theme;
};

class App {
public:
    explicit App(Options options);
    ~App();
    bool create(HINSTANCE instance, const std::wstring& className);
    int run();
    static constexpr UINT ShowMessage = WM_APP + 1;

private:
    enum class Mode { Home, Clipboard, Files, Snippets, Quicklinks, History, Settings, Confirm };
    struct FileJob { std::wstring query; std::vector<std::wstring> roots; std::uint64_t generation{}; };
    struct FileResult { std::vector<Entry> entries; std::uint64_t generation{}; bool failed{}; };
    struct Theme {
        COLORREF background{}, text{}, muted{}, faint{}, selection{}, hover{}, accent{}, control{};
    };

    Options options_;
    Settings settings_;
    std::wstring configWarning_, status_;
    HINSTANCE instance_{};
    HWND window_{}, search_{}, list_{}, previousWindow_{};
    HBRUSH background_{};
    HFONT searchFont_{}, titleFont_{}, detailFont_{}, symbolFont_{};
    HICON appIcon_{};
    Theme theme_;
    unsigned dpi_{96};
    UINT taskbarCreated_{};
    bool dark_{}, chinese_{}, composing_{}, hotkeyRegistered_{}, trayAdded_{}, clipboardListening_{};
    bool modal_{}, shuttingDown_{}, indexing_{}, filesBusy_{};
    ULONGLONG imeCommitUntil_{};
    DWORD ignoredClipboardSequence_{};
    Mode mode_{Mode::Home}, confirmReturn_{Mode::Home};
    Entry pending_;
    std::wstring confirmQuery_;
    std::vector<Entry> applications_, visible_, fileResults_;
    std::vector<std::wstring> clips_, calculations_;
    std::map<std::wstring, HICON> iconCache_;
    struct RestoreState { DWORD pid{}; WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)}; };
    std::map<HWND, RestoreState> restore_;
    std::jthread appWorker_, fileWorker_;
    std::mutex fileMutex_;
    std::condition_variable_any fileCondition_;
    std::optional<FileJob> fileJob_;
    std::stop_source activeFileStop_;
    std::uint64_t fileGeneration_{};

    const wchar_t* tr(const wchar_t* en, const wchar_t* zh) const;
    int px(int value) const;
    std::wstring query() const;
    std::wstring modeName() const;
    std::wstring emptyMessage() const;
    std::wstring hotkeyLabel() const;
    void applyTheme();
    void makeFonts();
    void layout();
    void position();
    void show(bool capturePrevious = true);
    void hide(bool restoreFocus = true);
    void shutdown();
    void setMode(Mode mode);
    void refresh();
    void setResults(std::vector<Entry> results);
    void activate(bool copyOnly = false);
    void execute(Entry entry, bool copyOnly = false, bool confirmed = false);
    void command(const Entry& entry, bool confirmed);
    void requestConfirmation(Entry entry);
    void persist();
    void updateStatus(std::wstring text);
    void toggleFavorite();
    void tray(bool add);
    void trayMenu();
    void registerShortcut();
    void clipboardSubscription();
    void captureClipboard();
    void refreshApplications();
    void startFileWorker();
    void scheduleFiles();
    void launchFileJob();
    void paint();
    void drawRow(const DRAWITEMSTRUCT& item);
    void drawText(HDC dc, std::wstring_view text, RECT rect, HFONT font, COLORREF color,
                  UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) const;
    void rounded(HDC dc, RECT rect, int radius, COLORREF fill) const;
    HICON entryIcon(const Entry& entry);
    bool key(WPARAM key);
    void smokeTest();
    bool captureBitmap(const std::wstring& path);
    void diagnostics(int failures);
    LRESULT message(UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK listProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR id, DWORD_PTR data);
};

}
