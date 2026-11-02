// Windows adaptation of Tinycast's SearchRelevance and WindowPlacementEngine; AGPL-3.0.
#include "core.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <locale>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace tinycast {
namespace {

std::wstring trim(std::wstring_view value) {
    while (!value.empty() && std::iswspace(value.front())) value.remove_prefix(1);
    while (!value.empty() && std::iswspace(value.back())) value.remove_suffix(1);
    return std::wstring(value);
}

bool wordCharacter(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c >= 0x80;
}

std::optional<int> foldedScore(std::wstring_view q, std::wstring_view c) {
    if (q.empty()) return 0;
    if (q == c) return 100000;
    const auto penalty = static_cast<int>(std::min<std::size_t>(c.size(), 2000));
    if (c.starts_with(q)) return 90000 - penalty;
    if (const auto at = c.find(q); at != std::wstring_view::npos)
        return (!wordCharacter(c[at - 1]) ? 80000 : 70000) - penalty;
    std::size_t qi = 0;
    int score = 0, run = 0;
    std::ptrdiff_t previous = -2;
    for (std::size_t ci = 0; ci < c.size() && qi < q.size(); ++ci) {
        if (c[ci] != q[qi]) continue;
        int bonus = 1;
        if (static_cast<std::ptrdiff_t>(ci) == previous + 1) bonus += ++run * 3;
        else run = 0;
        if (ci == 0) bonus += 12;
        else if (!wordCharacter(c[ci - 1])) bonus += 8;
        score += bonus;
        previous = static_cast<std::ptrdiff_t>(ci);
        ++qi;
    }
    if (qi != q.size()) return std::nullopt;
    return 10000 + std::min(score, 5000);
}

class ExpressionParser {
public:
    explicit ExpressionParser(std::wstring_view source) : source_(source) {}
    double evaluate() {
        const double value = sum();
        whitespace();
        if (position_ != source_.size()) throw std::invalid_argument("Trailing input");
        return finite(value);
    }
private:
    std::wstring_view source_;
    std::size_t position_{};
    unsigned depth_{};

    struct Depth {
        unsigned& value;
        explicit Depth(unsigned& depth) : value(depth) {
            if (++value > 32) throw std::invalid_argument("Expression too deep");
        }
        ~Depth() { --value; }
    };
    static double finite(double value) {
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite result");
        return value;
    }
    void whitespace() {
        while (position_ < source_.size() && std::iswspace(source_[position_])) ++position_;
    }
    bool take(wchar_t c) {
        whitespace();
        if (position_ < source_.size() && source_[position_] == c) { ++position_; return true; }
        return false;
    }
    double sum() {
        double value = product();
        for (;;) {
            if (take(L'+')) value = finite(value + product());
            else if (take(L'-')) value = finite(value - product());
            else return value;
        }
    }
    double product() {
        double value = unary();
        for (;;) {
            if (take(L'*')) value = finite(value * unary());
            else if (take(L'/')) {
                const double divisor = unary();
                if (divisor == 0) throw std::invalid_argument("Division by zero");
                value = finite(value / divisor);
            } else return value;
        }
    }
    double unary() {
        Depth guard(depth_);
        if (take(L'+')) return unary();
        if (take(L'-')) return finite(-unary());
        double value = primary();
        while (take(L'%')) value /= 100;
        if (take(L'^')) value = finite(std::pow(value, unary()));
        return value;
    }
    double primary() {
        whitespace();
        if (take(L'(')) {
            const double value = sum();
            if (!take(L')')) throw std::invalid_argument("Missing closing parenthesis");
            return value;
        }
        if (position_ == source_.size()) throw std::invalid_argument("Missing operand");
        const wchar_t c = source_[position_];
        if ((c >= L'0' && c <= L'9') || c == L'.') return number();
        const auto start = position_;
        while (position_ < source_.size() &&
               ((source_[position_] >= L'a' && source_[position_] <= L'z') ||
                (source_[position_] >= L'0' && source_[position_] <= L'9'))) ++position_;
        if (start == position_) throw std::invalid_argument("Invalid token");
        const auto name = source_.substr(start, position_ - start);
        if (name == L"pi") return std::numbers::pi;
        if (name == L"e") return std::numbers::e;
        if (!take(L'(')) throw std::invalid_argument("Unknown identifier");
        std::vector<double> args;
        do {
            if (args.size() >= 16) throw std::invalid_argument("Too many arguments");
            args.push_back(sum());
        } while (take(L','));
        if (!take(L')')) throw std::invalid_argument("Missing closing parenthesis");
        const double x = args.front();
        if (args.size() == 1) {
            if (name == L"sqrt") return finite(std::sqrt(x));
            if (name == L"abs") return std::abs(x);
            if (name == L"sin") return finite(std::sin(x));
            if (name == L"cos") return finite(std::cos(x));
            if (name == L"tan") return finite(std::tan(x));
            if (name == L"asin") return finite(std::asin(x));
            if (name == L"acos") return finite(std::acos(x));
            if (name == L"atan") return finite(std::atan(x));
            if (name == L"ln") return finite(std::log(x));
            if (name == L"log" || name == L"log10") return finite(std::log10(x));
            if (name == L"exp") return finite(std::exp(x));
            if (name == L"floor") return std::floor(x);
            if (name == L"ceil") return std::ceil(x);
            if (name == L"round") return std::round(x);
        }
        if (name == L"min") return *std::min_element(args.begin(), args.end());
        if (name == L"max") return *std::max_element(args.begin(), args.end());
        if (name == L"pow" && args.size() == 2) return finite(std::pow(x, args[1]));
        throw std::invalid_argument("Unknown function or argument count");
    }
    double number() {
        std::string ascii;
        const auto start = position_;
        while (position_ < source_.size() && source_[position_] <= 0x7f) {
            ascii.push_back(static_cast<char>(source_[position_]));
            ++position_;
        }
        double value{};
        const auto parsed = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr == ascii.data())
            throw std::invalid_argument("Invalid number");
        position_ = start + static_cast<std::size_t>(parsed.ptr - ascii.data());
        return finite(value);
    }
};

struct Unit { std::wstring_view name, family; double scale; double offset; };
constexpr Unit units[] = {
    {L"mm", L"length", .001, 0}, {L"cm", L"length", .01, 0},
    {L"m", L"length", 1, 0}, {L"km", L"length", 1000, 0},
    {L"in", L"length", .0254, 0}, {L"inch", L"length", .0254, 0},
    {L"ft", L"length", .3048, 0}, {L"feet", L"length", .3048, 0},
    {L"yd", L"length", .9144, 0}, {L"mi", L"length", 1609.344, 0},
    {L"mg", L"mass", .000001, 0}, {L"g", L"mass", .001, 0},
    {L"kg", L"mass", 1, 0}, {L"lb", L"mass", .45359237, 0},
    {L"lbs", L"mass", .45359237, 0}, {L"oz", L"mass", .028349523125, 0},
    {L"ms", L"time", .001, 0}, {L"s", L"time", 1, 0}, {L"sec", L"time", 1, 0},
    {L"min", L"time", 60, 0}, {L"h", L"time", 3600, 0}, {L"hr", L"time", 3600, 0},
    {L"day", L"time", 86400, 0}, {L"days", L"time", 86400, 0},
    {L"c", L"temperature", 1, 273.15}, {L"celsius", L"temperature", 1, 273.15},
    {L"f", L"temperature", 5.0 / 9, 273.15 - 32 * 5.0 / 9},
    {L"fahrenheit", L"temperature", 5.0 / 9, 273.15 - 32 * 5.0 / 9},
    {L"k", L"temperature", 1, 0}, {L"kelvin", L"temperature", 1, 0},
    {L"b", L"data", 1, 0}, {L"kb", L"data", 1000, 0}, {L"mb", L"data", 1000000, 0},
    {L"gb", L"data", 1000000000, 0}, {L"tb", L"data", 1000000000000.0, 0},
    {L"kib", L"data", 1024, 0}, {L"mib", L"data", 1048576, 0},
    {L"gib", L"data", 1073741824, 0}, {L"tib", L"data", 1099511627776.0, 0}
};

const Unit* unitNamed(std::wstring_view name) {
    if (name.starts_with(L"\u00b0")) name.remove_prefix(1);
    for (const auto& unit : units) if (unit.name == name) return &unit;
    return nullptr;
}

std::wstring formatNumber(double value) {
    if (value == 0) value = 0;
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12) << value;
    return out.str();
}

Rect rounded(Rect r) {
    const auto right = std::round(r.x + r.width), bottom = std::round(r.y + r.height);
    r.x = std::round(r.x); r.y = std::round(r.y);
    r.width = std::max(1.0, right - r.x); r.height = std::max(1.0, bottom - r.y);
    return r;
}

Rect clampRect(Rect r, Rect box) {
    r.x = std::clamp(r.x, box.x, std::max(box.x, box.x + box.width - r.width));
    r.y = std::clamp(r.y, box.y, std::max(box.y, box.y + box.height - r.height));
    return rounded(r);
}

}

std::wstring normalize(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    constexpr std::wstring_view accents = L"\u00e0\u00e1\u00e2\u00e3\u00e4\u00e5\u00e7\u00e8\u00e9\u00ea\u00eb\u00ec\u00ed\u00ee\u00ef\u00f1\u00f2\u00f3\u00f4\u00f5\u00f6\u00f9\u00fa\u00fb\u00fc\u00fd\u00ff";
    constexpr std::wstring_view ascii   = L"aaaaaaceeeeiiiinooooouuuuyy";
    for (wchar_t c : value) {
        if ((c >= 0x300 && c <= 0x36f) || (c >= 0x200b && c <= 0x200f) ||
            (c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x206f) ||
            c == 0xfeff || c == 0xad) continue;
        if (c >= 0xff01 && c <= 0xff5e) c = static_cast<wchar_t>(c - 0xfee0);
        if (c == 0x3000) c = L' ';
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
        if ((c >= 0xc0 && c <= 0xd6) || (c >= 0xd8 && c <= 0xde)) c += 32;
        if (const auto at = accents.find(c); at != std::wstring_view::npos) c = ascii[at];
        result.push_back(c);
    }
    return result;
}

std::optional<int> fuzzyScore(std::wstring_view query, std::wstring_view candidate) {
    return foldedScore(normalize(query), normalize(candidate));
}

std::vector<std::size_t> rankEntries(const std::vector<Entry>& entries, std::wstring_view query,
                                     std::size_t limit) {
    struct Hit { std::size_t index; int score; std::wstring name; };
    const auto q = normalize(trim(query));
    std::vector<Hit> hits;
    hits.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        auto title = normalize(entry.title);
        auto score = foldedScore(q, title);
        if (!q.empty()) {
            if (const auto secondary = foldedScore(q, normalize(entry.subtitle))) {
                const int value = *secondary - 15000;
                if (!score || value > *score) score = value;
            }
        }
        if (!score) continue;
        const int boost = (entry.pinned ? 2000 : 0) +
                          static_cast<int>(std::min<std::uint32_t>(entry.useCount, 200));
        hits.push_back({index, *score + boost, std::move(title)});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        return a.score != b.score ? a.score > b.score : a.name < b.name;
    });
    std::vector<std::size_t> result;
    result.reserve(std::min(limit, hits.size()));
    for (std::size_t i = 0; i < std::min(limit, hits.size()); ++i) result.push_back(hits[i].index);
    return result;
}

std::optional<Calculation> calculate(std::wstring_view expression) {
    if (expression.empty() || expression.size() > 512) return std::nullopt;
    auto text = trim(normalize(expression));
    if (text.starts_with(L"=")) text = trim(std::wstring_view(text).substr(1));
    std::replace(text.begin(), text.end(), L'\u00d7', L'*');
    std::replace(text.begin(), text.end(), L'\u00f7', L'/');
    std::replace(text.begin(), text.end(), L'\u2212', L'-');
    try {
        auto separator = text.find(L" to ");
        std::size_t separatorLength = 4;
        if (separator == std::wstring::npos) { separator = text.find(L" in "); separatorLength = 4; }
        if (separator != std::wstring::npos) {
            const auto destination = trim(std::wstring_view(text).substr(separator + separatorLength));
            auto source = trim(std::wstring_view(text).substr(0, separator));
            std::size_t split = source.size();
            while (split > 0 && ((source[split - 1] >= L'a' && source[split - 1] <= L'z') || source[split - 1] == 0xb0)) --split;
            const auto* from = unitNamed(std::wstring_view(source).substr(split));
            const auto* to = unitNamed(destination);
            if (!from || !to || from->family != to->family) return std::nullopt;
            const double input = ExpressionParser(std::wstring_view(source).substr(0, split)).evaluate();
            const double base = input * from->scale + from->offset;
            if (from->family == L"temperature" && base < -1e-10) return std::nullopt;
            double value = (base - to->offset) / to->scale;
            if (!std::isfinite(value)) return std::nullopt;
            if (from->family == L"temperature" && std::abs(value) < 1e-10) value = 0;
            return Calculation{value, formatNumber(value) + L" " + destination, destination};
        }
        const double value = ExpressionParser(text).evaluate();
        return Calculation{value, formatNumber(value), L""};
    } catch (const std::exception&) { return std::nullopt; }
}

Rect placeWindow(WindowAction action, Rect current, Rect workArea, double gap) {
    if (!std::isfinite(workArea.width) || !std::isfinite(workArea.height) ||
        workArea.width <= 0 || workArea.height <= 0) return current;
    gap = std::isfinite(gap) ? std::clamp(gap, 0.0, std::min(workArea.width, workArea.height) / 10) : 0;
    const auto tile = [&](double x0, double y0, double x1, double y1) {
        const double left = workArea.x + x0 * workArea.width + (x0 == 0 ? gap : gap / 2);
        const double top = workArea.y + y0 * workArea.height + (y0 == 0 ? gap : gap / 2);
        const double right = workArea.x + x1 * workArea.width - (x1 == 1 ? gap : gap / 2);
        const double bottom = workArea.y + y1 * workArea.height - (y1 == 1 ? gap : gap / 2);
        return rounded({left, top, right - left, bottom - top});
    };
    switch (action) {
    case WindowAction::LeftHalf: return tile(0, 0, .5, 1);
    case WindowAction::RightHalf: return tile(.5, 0, 1, 1);
    case WindowAction::TopHalf: return tile(0, 0, 1, .5);
    case WindowAction::BottomHalf: return tile(0, .5, 1, 1);
    case WindowAction::TopLeft: return tile(0, 0, .5, .5);
    case WindowAction::TopRight: return tile(.5, 0, 1, .5);
    case WindowAction::BottomLeft: return tile(0, .5, .5, 1);
    case WindowAction::BottomRight: return tile(.5, .5, 1, 1);
    case WindowAction::FirstThird: return tile(0, 0, 1.0 / 3, 1);
    case WindowAction::CenterThird: return tile(1.0 / 3, 0, 2.0 / 3, 1);
    case WindowAction::LastThird: return tile(2.0 / 3, 0, 1, 1);
    case WindowAction::FirstTwoThirds: return tile(0, 0, 2.0 / 3, 1);
    case WindowAction::LastTwoThirds: return tile(1.0 / 3, 0, 1, 1);
    default: break;
    }
    const Rect canvas{workArea.x + gap, workArea.y + gap, workArea.width - 2 * gap, workArea.height - 2 * gap};
    if (action == WindowAction::Maximize) return rounded(canvas);
    if (action == WindowAction::AlmostMaximize)
        return rounded({canvas.x + canvas.width * .05, canvas.y + canvas.height * .05, canvas.width * .9, canvas.height * .9});
    if (action == WindowAction::Center) {
        current.width = std::min(current.width, canvas.width);
        current.height = std::min(current.height, canvas.height);
        return rounded({canvas.x + (canvas.width - current.width) / 2, canvas.y + (canvas.height - current.height) / 2,
                        current.width, current.height});
    }
    const double direction = action == WindowAction::MakeLarger ? 1 : -1;
    const auto width = std::clamp(current.width + direction * std::max(2.0, std::round(canvas.width * .025) * 2),
                                std::min(canvas.width, std::max(200.0, canvas.width * .15)), canvas.width);
    const auto height = std::clamp(current.height + direction * std::max(2.0, std::round(canvas.height * .025) * 2),
                                 std::min(canvas.height, std::max(150.0, canvas.height * .15)), canvas.height);
    return clampRect({current.x - (width - current.width) / 2, current.y - (height - current.height) / 2, width, height}, canvas);
}

std::wstring expandTemplate(std::wstring_view pattern, std::wstring_view input,
                            std::wstring_view clipboard, std::wstring_view date,
                            std::wstring_view time) {
    std::wstring result;
    for (std::size_t at = 0; at < pattern.size();) {
        if (pattern[at] == L'{') {
            const auto end = pattern.find(L'}', at + 1);
            if (end != std::wstring_view::npos) {
                const auto key = pattern.substr(at + 1, end - at - 1);
                std::optional<std::wstring_view> replacement;
                if (key == L"query") replacement = input;
                else if (key == L"clipboard") replacement = clipboard;
                else if (key == L"date") replacement = date;
                else if (key == L"time") replacement = time;
                if (replacement) { result.append(*replacement); at = end + 1; continue; }
            }
        }
        result.push_back(pattern[at++]);
    }
    return result;
}

std::wstring urlEncode(std::wstring_view value) {
    std::wstring result;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    const auto byte = [&](unsigned c) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            result.push_back(static_cast<wchar_t>(c));
        else { result.push_back(L'%'); result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    };
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto cp = static_cast<std::uint32_t>(value[i]);
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 1 < value.size() && value[i + 1] >= 0xdc00 && value[i + 1] <= 0xdfff)
                cp = 0x10000 + ((cp - 0xd800) << 10) + (static_cast<std::uint32_t>(value[++i]) - 0xdc00);
            else cp = 0xfffd;
        } else if ((cp >= 0xdc00 && cp <= 0xdfff) || cp > 0x10ffff) cp = 0xfffd;
        if (cp < 0x80) byte(cp);
        else if (cp < 0x800) { byte(0xc0 | (cp >> 6)); byte(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { byte(0xe0 | (cp >> 12)); byte(0x80 | ((cp >> 6) & 63)); byte(0x80 | (cp & 63)); }
        else { byte(0xf0 | (cp >> 18)); byte(0x80 | ((cp >> 12) & 63)); byte(0x80 | ((cp >> 6) & 63)); byte(0x80 | (cp & 63)); }
    }
    return result;
}

}
