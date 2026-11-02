#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tinycast {

enum class EntryKind { Application, Command, Quicklink, Snippet, File, Clipboard, Calculator };

struct Entry {
    std::wstring id;
    std::wstring title;
    std::wstring subtitle;
    std::wstring target;
    std::wstring arguments;
    EntryKind kind{EntryKind::Application};
    bool pinned{false};
    std::uint32_t useCount{0};
};

std::wstring normalize(std::wstring_view value);
std::optional<int> fuzzyScore(std::wstring_view query, std::wstring_view candidate);
std::vector<std::size_t> rankEntries(const std::vector<Entry>& entries, std::wstring_view query,
                                     std::size_t limit = 100);

struct Calculation {
    double value{};
    std::wstring formatted;
    std::wstring unit;
};

std::optional<Calculation> calculate(std::wstring_view expression);

struct Rect {
    double x{}, y{}, width{}, height{};
};

enum class WindowAction {
    LeftHalf, RightHalf, TopHalf, BottomHalf,
    TopLeft, TopRight, BottomLeft, BottomRight,
    FirstThird, CenterThird, LastThird, FirstTwoThirds, LastTwoThirds,
    Maximize, Center, AlmostMaximize, MakeLarger, MakeSmaller
};

Rect placeWindow(WindowAction action, Rect current, Rect workArea, double gap = 8);
std::wstring expandTemplate(std::wstring_view pattern, std::wstring_view input,
                            std::wstring_view clipboard, std::wstring_view date,
                            std::wstring_view time);
std::wstring urlEncode(std::wstring_view value);

}
