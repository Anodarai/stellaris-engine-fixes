// Issue #3: CEconomyCountryModule::AddResource narrows its whole-unit amount to int32.
// Keep the quotient in int64 instead. This is not a general saturating-arithmetic fix:
// the subsequent stockpile addition and the resource converter still use vanilla arithmetic.
#pragma once

#include "locate.h"

namespace resourceoverflow {

// Both branches of the same AddResource, including their division by 100000,
// multiplication back to raw units, and the jump joining them. Only the call displacement
// is masked. Requiring this entire sequence once cross-checks the two patch sites.
constexpr char kRounding[] =
    "48 B8 09 E1 D1 C6 11 6B F1 29 48 F7 EB 48 C1 FA 0E 48 8B C2 48 C1 E8 3F 48 03 C2 "
    "48 98 4C 69 C8 A0 86 01 00 4C 89 7C 24 20 4C 8B C6 48 8B 57 10 49 8B CE E8 ?? ?? ?? ?? EB 37 "
    "48 B8 09 E1 D1 C6 11 6B F1 29 48 F7 EB 48 C1 FA 0E 48 8B C2 48 C1 E8 3F 48 03 D0 "
    "4C 63 46 18 48 8B 45 00 48 8B 08 4E 8D 0C C1 48 63 C2 48 69 C8 A0 86 01 00 49 01 09";
constexpr size_t kSize = 113;
constexpr size_t kSituationNarrow = 27;
constexpr size_t kStockpileNarrow = 100;

inline uint8_t* Locate(const locate::Image& img, std::string& error) {
    return locate::FindOne(img, "AddResource whole-unit rounding", kRounding, error);
}

// Prepare both changes before touching game memory. The caller writes this block in place:
// no instruction moves, so the relative call/jump and all branch destinations stay valid.
inline bool Prepare(const uint8_t* original, uint8_t (&patched)[kSize]) {
    if (!locate::MatchAt(original, locate::Parse(kRounding))) return false;
    memcpy(patched, original, kSize);
    patched[kSituationNarrow] = 0x66;      // cdqe -> two-byte nop
    patched[kSituationNarrow + 1] = 0x90;
    patched[kStockpileNarrow + 1] = 0x8b;  // movsxd rax,edx -> mov rax,rdx
    return true;
}

}  // namespace resourceoverflow
