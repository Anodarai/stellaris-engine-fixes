// Locations for the heap replacement (allocator.h): the five CRT heap functions and __acrt_heap.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "../core/locate.h"

namespace locate {

// The statically linked CRT's heap functions: the only code that touches the CRT heap (Ghidra:
// the only callers of HeapAlloc/HeapFree/HeapReAlloc/HeapSize on it). free, operator new,
// _aligned_malloc/_aligned_free, _recalloc and the rest all end in these five.
struct CrtHeapLocations {
    uint8_t* malloc = nullptr;  // _malloc_base(size)
    uint8_t* calloc = nullptr;  // _calloc_base(count, size)
    uint8_t* realloc = nullptr; // _realloc_base(block, size)
    uint8_t* free = nullptr;    // _free_base(block)
    uint8_t* msize = nullptr;   // _msize_base(block)
    HANDLE* heap = nullptr;     // &__acrt_heap: null until the CRT initializes
};

// The UCRT heap functions (the same code in stellaris.exe 4.5.2 and in programs built with
// VS 2022's static CRT). Each loads __acrt_heap with `mov rcx,[rip+heap]` (48 8B 0D).
constexpr char kCrtMalloc[] =
    "40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 77 3C 48 85 C9 B8 01 00 00 00 48 0F 44 D8 EB 15 E8 ?? ?? ?? ?? "
    "85 C0 74 25 48 8B CB E8 ?? ?? ?? ?? 85 C0 74 19 48 8B 0D ?? ?? ?? ?? 4C 8B C3 33 D2 FF 15";

constexpr char kCrtCalloc[] =
    "40 53 48 83 EC 20 4C 8B C2 48 8B D9 48 85 C9 74 0E 33 D2 48 8D 42 E0 48 F7 F3 49 3B C0 72 43 49 0F AF D8 "
    "B8 01 00 00 00 48 85 DB 48 0F 44 D8 EB 15 E8 ?? ?? ?? ?? 85 C0 74 28 48 8B CB E8 ?? ?? ?? ?? 85 C0 74 1C "
    "48 8B 0D ?? ?? ?? ?? 4C 8B C3 BA 08 00 00 00 FF 15";

// +25 call _malloc_base (block null), +37 call _free_base (size 0)
constexpr char kCrtRealloc[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 C9 75 0A 48 8B CA E8 ?? ?? ?? ?? EB 1F 48 85 DB "
    "75 07 E8 ?? ?? ?? ?? EB 11 48 83 FB E0 76 2D E8 ?? ?? ?? ?? C7 00 0C 00 00 00 33 C0 48 8B 5C 24 30 "
    "48 83 C4 20 5F C3 E8 ?? ?? ?? ?? 85 C0 74 DF 48 8B CB E8 ?? ?? ?? ?? 85 C0 74 D3 48 8B 0D ?? ?? ?? ?? "
    "4C 8B CB 4C 8B C7 33 D2 FF 15";

constexpr char kCrtFree[] = "48 85 C9 74 ?? 53 48 83 EC 20 4C 8B C1 33 D2 48 8B 0D ?? ?? ?? ?? FF 15";

constexpr char kCrtMsize[] =
    "48 83 EC 28 48 85 C9 75 19 E8 ?? ?? ?? ?? C7 00 16 00 00 00 E8 ?? ?? ?? ?? 48 83 C8 FF 48 83 C4 28 C3 "
    "4C 8B C1 33 D2 48 8B 0D ?? ?? ?? ?? 48 83 C4 28 48 FF 25";

inline bool LocateCrtHeap(const Image& img, CrtHeapLocations& out, std::string& error) {
    struct Fn {
        const char* what;
        const char* pattern;
        uint8_t** at;
    } fns[] = {
        {"_malloc_base", kCrtMalloc, &out.malloc},    {"_calloc_base", kCrtCalloc, &out.calloc},
        {"_realloc_base", kCrtRealloc, &out.realloc}, {"_free_base", kCrtFree, &out.free},
        {"_msize_base", kCrtMsize, &out.msize},
    };
    const Pattern heapLoad = Parse("48 8B 0D");
    for (Fn& fn : fns) {
        if (!(*fn.at = FindOne(img, fn.what, fn.pattern, error)))
            return false;
        // The pattern ends past the heap load, so its first 48 8B 0D is inside the match.
        auto load = FindAll(*fn.at, *fn.at + Parse(fn.pattern).size(), heapLoad, 1);
        auto heap = reinterpret_cast<HANDLE*>(RelTarget(load[0] + 3, load[0] + 7));
        if (out.heap && heap != out.heap) {
            error = std::string(fn.what) + " uses a different heap handle";
            return false;
        }
        out.heap = heap;
    }
    if (RelTarget(out.realloc + 25, out.realloc + 29) != out.malloc ||
        RelTarget(out.realloc + 37, out.realloc + 41) != out.free) {
        error = "_realloc_base does not call the _malloc_base/_free_base found";
        return false;
    }
    return true;
}

} // namespace locate
