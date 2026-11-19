// The pattern matching every feature uses to find what it needs in stellaris.exe, so it keeps
// working when a game update moves code around. Struct offsets the game's code itself uses are read
// out of the matched instructions instead of being hard-coded.
//
// Works on a mapped image: the running game, or the file mapped as an image (tests). Patterns and
// offsets come from Ghidra (re/fleet_outliner_sort.md, re/sigs_gen.txt). Each feature's own patterns
// and locators are in a *_patterns.h next to it (performance/, ai/, storage/, interface/); this file
// keeps the matching, the image's sections and the game string functions every feature uses.
#pragma once
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "game.h"

namespace locate {

// --- Patterns -------------------------------------------------------------------------------

// A pattern: bytes, with -1 for "any byte".
using Pattern = std::vector<int>;

inline Pattern Parse(const char* text) {
    Pattern p;
    for (const char* s = text; *s;) {
        if (*s == ' ') {
            ++s;
        } else if (s[0] == '?') {
            p.push_back(-1);
            s += s[1] == '?' ? 2 : 1;
        } else {
            p.push_back(static_cast<int>(strtoul(std::string(s, 2).c_str(), nullptr, 16)));
            s += 2;
        }
    }
    return p;
}

inline bool MatchAt(const uint8_t* p, const Pattern& pat) {
    for (size_t i = 0; i < pat.size(); ++i) {
        if (pat[i] >= 0 && p[i] != pat[i])
            return false;
    }
    return true;
}

// All matches of `pat` in [begin, end), at most `limit`. `anchor` is the index of a fixed byte to
// search for first; the rarer that byte, the faster.
inline std::vector<uint8_t*> FindAll(uint8_t* begin, uint8_t* end, const Pattern& pat, size_t limit = 8,
                                     size_t anchor = 0) {
    std::vector<uint8_t*> hits;
    if (pat.empty() || end - begin < static_cast<ptrdiff_t>(pat.size()))
        return hits;
    if (pat[anchor] < 0) {
        for (uint8_t* p = begin; p + pat.size() <= end && hits.size() < limit; ++p) {
            if (MatchAt(p, pat))
                hits.push_back(p);
        }
        return hits;
    }
    const uint8_t* last = end - pat.size() + anchor; // the last place the anchor can match
    for (const uint8_t* a = begin + anchor; a <= last && hits.size() < limit; ++a) {
        a = static_cast<const uint8_t*>(memchr(a, pat[anchor], last - a + 1));
        if (!a)
            break;
        uint8_t* p = const_cast<uint8_t*>(a) - anchor;
        if (MatchAt(p, pat))
            hits.push_back(p);
    }
    return hits;
}

inline int32_t Disp32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// Target of a rel32/RIP-relative operand at `operand`, in an instruction ending at `next`.
inline uint8_t* RelTarget(const uint8_t* operand, const uint8_t* next) {
    return const_cast<uint8_t*>(next) + Disp32(operand);
}

// --- Image ----------------------------------------------------------------------------------

struct Section {
    uint8_t* begin;
    uint8_t* end;
};

struct Image {
    uint8_t* base = nullptr;
    uintptr_t pointerBase = 0;   // what absolute pointers in the image are relative to
    std::vector<Section> code;   // executable sections
    std::vector<Section> data;   // initialized, readable, not executable
    size_t byteCounts[256] = {}; // how common each byte is in the code, from a sample
};

inline Image Describe(uint8_t* base) {
    Image img;
    img.base = base;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    // The loader rewrites ImageBase when it relocates the image; a file mapped as an image
    // resource keeps the preferred base, matching its unrelocated pointers.
    img.pointerBase = static_cast<uintptr_t>(nt->OptionalHeader.ImageBase);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        Section s{base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            img.code.push_back(s);
            // A sample is enough to tell which bytes are rare.
            for (const uint8_t* p = s.begin; p < s.end; p += 61)
                ++img.byteCounts[*p];
        } else if ((sec->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) &&
                   (sec->Characteristics & IMAGE_SCN_MEM_READ)) {
            img.data.push_back(s);
        }
    }
    return img;
}

inline std::vector<uint8_t*> FindInCode(const Image& img, const char* text, size_t limit = 8) {
    Pattern pat = Parse(text);
    size_t anchor = 0;
    for (size_t i = 0; i < pat.size(); ++i) {
        if (pat[i] >= 0 && (pat[anchor] < 0 || img.byteCounts[pat[i]] < img.byteCounts[pat[anchor]]))
            anchor = i;
    }
    std::vector<uint8_t*> hits;
    for (const Section& s : img.code) {
        for (uint8_t* h : FindAll(s.begin, s.end, pat, limit - hits.size(), anchor))
            hits.push_back(h);
        if (hits.size() >= limit)
            break;
    }
    return hits;
}

// The one 8-byte slot in a data section holding a pointer to `fn` (a vtable entry), or null.
inline uintptr_t* FindPointer(const Image& img, const uint8_t* fn, size_t* count) {
    uintptr_t want = img.pointerBase + static_cast<uintptr_t>(fn - img.base);
    uintptr_t* found = nullptr;
    *count = 0;
    for (const Section& s : img.data) {
        auto p = reinterpret_cast<uintptr_t*>((reinterpret_cast<uintptr_t>(s.begin) + 7) & ~uintptr_t(7));
        for (; reinterpret_cast<uint8_t*>(p + 1) <= s.end; ++p) {
            if (*p == want) {
                found = p;
                ++*count;
            }
        }
    }
    return found;
}

// --- Finding code ---------------------------------------------------------------------------

// The single match of `pattern` in code, or null with `error` set.
inline uint8_t* FindOne(const Image& img, const char* what, const char* pattern, std::string& error) {
    auto hits = FindInCode(img, pattern, 2);
    if (hits.size() != 1) {
        error = std::string(what) + (hits.empty() ? ": not found" : ": more than one match");
        return nullptr;
    }
    return hits[0];
}

// Whether code at `begin` calls `target` within `size` bytes (any E8 byte that decodes to it).
inline bool CallsWithin(const Image& img, const uint8_t* begin, size_t size, const uint8_t* target) {
    for (const Section& s : img.code) {
        if (begin < s.begin || begin >= s.end)
            continue;
        const uint8_t* end = begin + size < s.end ? begin + size : s.end;
        for (const uint8_t* p = begin; p + 5 <= end; ++p) {
            if (*p == 0xE8 && RelTarget(p + 1, p + 5) == target)
                return true;
        }
    }
    return false;
}

// The match of `pattern` in [begin, begin + size), or null with `error` set.
inline uint8_t* FindWithin(uint8_t* begin, size_t size, const char* what, const char* pattern, std::string& error) {
    auto hits = FindAll(begin, begin + size, Parse(pattern), 1);
    if (hits.empty())
        error = std::string(what) + ": not found";
    return hits.empty() ? nullptr : hits[0];
}

// --- Game strings ---------------------------------------------------------------------------

// Game string helpers every feature uses.
struct StringLocations {
    uint8_t* buildString = nullptr; // CPersistentName::BuildString(name, out)
    uint8_t* gameFree = nullptr;    // the allocator's free
};

// CPersistentName::BuildString(name, out): +99 call free (its scratch array's buffer).
constexpr char kBuildString[] =
    "48 89 5C 24 08 57 48 83 EC 60 48 8B DA 0F 57 C0 0F 11 44 24 20 48 C7 44 24 30 08 00 00 00 "
    "48 8D 3D ?? ?? ?? ?? 48 89 7C 24 20 48 8D 44 24 38 48 89 44 24 28 45 33 C9 4C 8D 44 24 20 "
    "E8 ?? ?? ?? ?? 90 48 89 7C 24 20 C7 44 24 34 00 00 00 00 48 8D 44 24 38 48 8B 4C 24 28 48 3B C8 "
    "74 ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B C3";

inline bool LocateStrings(const Image& img, StringLocations& out, std::string& error) {
    if (!(out.buildString = FindOne(img, "CPersistentName::BuildString", kBuildString, error)))
        return false;
    out.gameFree = RelTarget(out.buildString + 100, out.buildString + 104);
    return true;
}

} // namespace locate
