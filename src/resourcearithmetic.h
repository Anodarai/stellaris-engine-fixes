#pragma once
#include "locate.h"
#include "numericmath.h"

namespace resourcearithmetic {
// CResourceTable::MultResource, including both the fast and signed quotient/remainder paths.
constexpr char kMultiply[] =
    "48 8B 01 4C 63 4A ?? BA 33 F3 04 B5 48 8B 08 4E 8D 14 C9 4D 8B 08 49 8B 0A 49 B8 66 E6 09 6A 01 "
    "00 00 00 48 8D 04 11 49 3B C0 77 2C 49 8D 04 11 49 3B C0 77 23 49 0F AF C9 48 B8 09 E1 D1 C6 11 "
    "6B F1 29 48 F7 E9 48 C1 FA 0E 48 8B C2 48 C1 E8 3F 48 03 D0 49 89 12 C3 4C 3B C9 4D 8B C1 49 BB "
    "09 E1 D1 C6 11 6B F1 29 4C 0F 4C C1 4C 0F 4F C9 49 8B C3 49 F7 E8 48 8B CA 48 C1 F9 0E 48 8B C1 "
    "48 C1 E8 3F 48 03 C8 48 69 C1 A0 86 01 00 49 0F AF C9 4C 2B C0 49 8B C3 4D 0F AF C1 49 F7 E8 48 "
    "C1 FA 0E 48 8B C2 48 C1 E8 3F 48 03 C2 48 03 C1 49 89 02 C3";
constexpr size_t kSize = 180, kStolen = 19;
using MultiplyFn = void(__fastcall*)(uint8_t*, const uint8_t*, const int64_t*);
struct Locations { uint8_t* multiply = nullptr; size_t resourceIndex = 0; };
inline bool Locate(const locate::Image& img, Locations& out, std::string& error) {
    out.multiply = locate::FindOne(img, "CResourceTable::MultResource", kMultiply, error);
    if (!out.multiply) return false;
    // The encoded disp8 is signed. Refuse an unexpected negative field displacement.
    if (out.multiply[6] >= 128) { error = "negative resource-index offset"; return false; }
    out.resourceIndex = out.multiply[6];
    return true;
}
inline void Multiply(const Locations& loc, uint8_t* table, const uint8_t* resource, const int64_t* multiplier) {
    auto inner = *reinterpret_cast<uint8_t**>(table);
    auto values = *reinterpret_cast<int64_t**>(inner);
    int index = *reinterpret_cast<const int*>(resource + loc.resourceIndex);
    int64_t amount = values[index], factor = *multiplier;
    values[index] = numeric::MulDiv(amount, factor, 100000);
}
} // namespace resourcearithmetic
