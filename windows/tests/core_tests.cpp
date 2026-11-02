#include "core.hpp"
#include <cmath>
#include <iostream>
#include <limits>

using namespace tinycast;
namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void number(std::wstring_view expression, double expected, const char* message) {
    const auto result = calculate(expression);
    check(result && std::abs(result->value - expected) <= std::max(1.0, std::abs(expected)) * 1e-10, message);
}
void invalid(std::wstring_view expression) { check(!calculate(expression), "invalid calculation rejected"); }
}

int main() {
    check(normalize(L"VS Code") == L"vs code", "ASCII normalization");
    check(normalize(L"\uff21\uff22\uff23") == L"abc", "fullwidth normalization");
    check(normalize(L"\u5fae\u4fe1") == L"\u5fae\u4fe1", "CJK preserved");
    check(normalize(L"caf\u00e9") == normalize(L"cafe\u0301"), "Latin combining marks");
    check(normalize(L"co\u200bde") == L"code", "format character ignored");
    check(fuzzyScore(L"code", L"Code").value_or(0) > fuzzyScore(L"code", L"Code Editor").value_or(0), "exact before prefix");
    check(fuzzyScore(L"code", L"Code Editor").value_or(0) > fuzzyScore(L"code", L"Visual Code").value_or(0), "prefix before word");
    check(fuzzyScore(L"code", L"Visual Code").value_or(0) > fuzzyScore(L"code", L"Decode").value_or(0), "word before substring");
    check(fuzzyScore(L"vsc", L"Visual Studio Code").has_value(), "subsequence match");
    check(!fuzzyScore(L"xyz", L"Code"), "unmatched omitted");
    check(fuzzyScore(L"\u5fae", L"\u5fae\u4fe1").has_value(), "CJK prefix");
    std::vector<Entry> entries{{L"1", L"Code Editor", L"", L"", L"", EntryKind::Application, true, 10000},
                               {L"2", L"Code", L"", L"", L"", EntryKind::Application, false, 0}};
    check(rankEntries(entries, L"code").front() == 1, "pins and usage cannot defeat exact title");
    check(rankEntries(entries, L"").front() == 0, "empty query pins first");
    check(rankEntries(entries, L"", 1).size() == 1, "result bound");
    check(rankEntries(entries, L"", 0).empty(), "zero result bound");
    number(L"2+3*4", 14, "operator precedence");
    number(L"(2+3)*4", 20, "parentheses");
    number(L"-2^2", -4, "unary exponent precedence");
    number(L"2^-2", .25, "negative exponent");
    number(L"2^3^2", 512, "right associative exponent");
    number(L"50% * 200", 100, "percentage");
    number(L"1e3 + .5", 1000.5, "scientific notation");
    number(L"sqrt(81)", 9, "sqrt");
    number(L"max(2, 5, 3)", 5, "max function");
    number(L"min(2,-5,3)", -5, "min function");
    number(L"sin(pi/2)", 1, "trigonometry");
    number(L"log(1000)", 3, "base10 log");
    number(L"pow(3,4)", 81, "power function");
    number(L"= 8 / 4", 2, "calculator prefix");
    number(L"2 \u00d7 3", 6, "multiplication glyph");
    number(L"10kg to lb", 22.0462262184878, "mass conversion");
    number(L"32 f to c", 0, "temperature conversion");
    number(L"100 c to f", 212, "temperature reverse");
    number(L"1m in cm", 100, "length conversion");
    number(L"1 gib to mib", 1024, "binary data conversion");
    number(L"2 h to min", 120, "time conversion");
    for (const auto* text : {L"", L"hello", L"1/0", L"sqrt(-1)", L"1e9999", L"1+", L"2abc", L"max()",
                             L"pow(2)", L"2**3", L"(1+2", L"1kg to cm", L"1 usd to eur", L"-1 k to c"}) invalid(text);
    invalid(std::wstring(600, L'1'));
    invalid(std::wstring(50, L'(') + L"1" + std::wstring(50, L')'));
    const Rect area{-1920, -200, 1920, 1080}, current{-1800, 0, 800, 600};
    const auto left = placeWindow(WindowAction::LeftHalf, current, area, 0);
    const auto right = placeWindow(WindowAction::RightHalf, current, area, 0);
    check(left.x == -1920 && left.width == 960, "left on negative-coordinate display");
    check(left.x + left.width == right.x && right.x + right.width == 0, "half edges meet");
    const auto quarter = placeWindow(WindowAction::BottomRight, current, area, 0);
    check(quarter.x == -960 && quarter.y == 340 && quarter.height == 540, "quarter placement");
    const auto gapLeft = placeWindow(WindowAction::LeftHalf, current, area, 8);
    const auto gapRight = placeWindow(WindowAction::RightHalf, current, area, 8);
    check(gapRight.x - gapLeft.x - gapLeft.width == 8, "inter-window gap");
    const auto third = placeWindow(WindowAction::FirstThird, current, {0, 0, 1001, 701}, 0);
    const auto third2 = placeWindow(WindowAction::CenterThird, current, {0, 0, 1001, 701}, 0);
    check(third.x + third.width == third2.x, "thirds share rounded edge");
    const auto centered = placeWindow(WindowAction::Center, current, area, 0);
    check(centered.x == -1360 && centered.y == 40, "center preserves size");
    const auto maximum = placeWindow(WindowAction::Maximize, current, area, 8);
    check(maximum.x == -1912 && maximum.width == 1904, "maximize work area inset");
    const auto tiny = placeWindow(WindowAction::MakeSmaller, {0,0,10,10}, {0,0,100,80}, 0);
    check(tiny.width <= 100 && tiny.height <= 80 && tiny.width > 0, "tiny work area clamp");
    const auto hugeGap = placeWindow(WindowAction::LeftHalf, current, area, 99999);
    check(hugeGap.width > 0, "gap sanitization");
    check(expandTemplate(L"{query}/{clipboard}/{date}/{time}", L"x", L"y", L"d", L"t") == L"x/y/d/t", "template substitutions");
    check(expandTemplate(L"{query}", L"{clipboard}", L"secret", L"", L"") == L"{clipboard}", "replacement is never re-expanded");
    check(expandTemplate(L"{unknown}", L"", L"", L"", L"") == L"{unknown}", "unknown placeholders preserved");
    check(urlEncode(L"a b&c") == L"a%20b%26c", "URL metacharacters");
    check(urlEncode(L"\u4e2d\u6587") == L"%E4%B8%AD%E6%96%87", "UTF8 encoding");
    check(urlEncode(L"\U0001f600") == L"%F0%9F%98%80", "supplementary Unicode encoding");
    check(urlEncode(L"~_.-") == L"~_.-", "unreserved characters");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
