// Natural ("numeric-aware") string ordering for fleet names.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace natsort {

// Removes Paradox text markup so it does not affect ordering:
//   "\x11" + one byte      internal color code (as used by the outliner)
//   "§" + one byte         color start / "§!" color end (UTF-8 C2 A7)
//   "£name£"               inline icon (UTF-8 C2 A3 delimiters)
//   zero-width characters  U+200B..U+200D, U+2060, U+FEFF (e.g. the game's "​Grand Archive")
// Leading/trailing spaces are trimmed and runs of spaces collapse to one.
inline std::string StripMarkup(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto at = [&](size_t i) { return static_cast<unsigned char>(s[i]); };
    auto zeroWidth = [&](size_t i) {
        if (i + 2 >= s.size()) return false;
        if (at(i) == 0xE2 && at(i + 1) == 0x80 && at(i + 2) >= 0x8B && at(i + 2) <= 0x8D) return true;
        if (at(i) == 0xE2 && at(i + 1) == 0x81 && at(i + 2) == 0xA0) return true;
        return at(i) == 0xEF && at(i + 1) == 0xBB && at(i + 2) == 0xBF;
    };
    for (size_t i = 0; i < s.size();) {
        if (zeroWidth(i)) {
            i += 3;
        } else if (at(i) == 0x11) {
            i += 2;
        } else if (at(i) == 0xC2 && i + 1 < s.size() && at(i + 1) == 0xA7) {
            i += 3;
        } else if (at(i) == 0xC2 && i + 1 < s.size() && at(i + 1) == 0xA3) {
            size_t close = s.find("\xC2\xA3", i + 2);
            i = close == std::string_view::npos ? s.size() : close + 2;
        } else if (s[i] == ' ') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            ++i;
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }

inline unsigned char Fold(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') ? static_cast<unsigned char>(u + ('a' - 'A')) : u;
}

// Three-way natural comparison: digit runs compare by numeric value, everything else
// compares byte-wise with ASCII case folded. Returns <0, 0 or >0.
// Names that differ only in leading zeros or letter case compare equal.
inline int Compare(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (IsDigit(a[i]) && IsDigit(b[j])) {
            while (i < a.size() && a[i] == '0') ++i;
            while (j < b.size() && b[j] == '0') ++j;
            size_t si = i, sj = j;
            while (i < a.size() && IsDigit(a[i])) ++i;
            while (j < b.size() && IsDigit(b[j])) ++j;
            size_t la = i - si, lb = j - sj;
            if (la != lb) return la < lb ? -1 : 1;
            int c = a.substr(si, la).compare(b.substr(sj, lb));
            if (c != 0) return c;
            continue;
        }
        unsigned char ca = Fold(a[i]), cb = Fold(b[j]);
        if (ca != cb) return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

}  // namespace natsort
