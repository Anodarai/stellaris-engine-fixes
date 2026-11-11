// Issue #3: binary (scale 32768) to decimal (scale 100000) conversion.
// Keep the whole part in int64, round fractional ties away from zero as the game does,
// and saturate only if the decimal output itself cannot fit int64.
#pragma once
#include <array>
#include "locate.h"

namespace fixedpointoverflow {

#define SEF_Q15_INLINE \
    "49 8B C8 48 81 E1 00 80 FF FF 4D 85 C0 7F 16 49 8B C0 25 FF 7F 00 00 48 F7 D8 " \
    "48 1B C0 25 00 80 00 00 48 03 C8 48 C1 F9 0F 48 63 D1 48 8B C2 48 C1 E0 0F " \
    "4C 2B C0 B8 00 00 50 C3 4C 0F AF C0 49 C1 F8 0F 33 C0 4D 85 C0 0F 9E C0 4C 2B C0 " \
    "49 81 C0 00 40 00 00 49 C1 F8 0F 4D 63 C8 48 69 C2 A0 86 01 00 4C 03 C8"

constexpr char kInline[] = SEF_Q15_INLINE;
constexpr char kFleetWeight[] =
    "48 89 55 ?? 49 01 14 24 4D 8B 14 24 4D 8B 86 ?? ?? ?? ?? " SEF_Q15_INLINE
    " 4C 8B 05 ?? ?? ?? ?? 4B 8D 04 39 49 3B C5";
constexpr char kTechWeight[] =
    "48 89 55 ?? 4C 03 D2 4D 89 14 24 4D 8B 86 ?? ?? ?? ?? " SEF_Q15_INLINE
    " 4C 8B 05 ?? ?? ?? ?? 4B 8D 04 39 49 3B C5";
#undef SEF_Q15_INLINE

constexpr char kFunction[] =
    "48 8B 02 4C 8B C1 48 85 C0 7E 05 48 8B D0 EB 18 48 8B C8 81 E1 FF 7F 00 00 48 F7 D9 "
    "48 1B D2 81 E2 00 80 00 00 48 03 D0 48 C1 FA 0F 48 63 D2 48 8B CA 48 C1 E1 0F "
    "48 2B C1 B9 00 00 50 C3 48 0F AF C1 33 C9 48 C1 F8 0F 48 85 C0 0F 9E C1 48 2B C1 "
    "48 05 00 40 00 00 48 C1 F8 0F 48 63 C8 48 69 C2 A0 86 01 00 48 03 C8 49 8B C0 49 89 08 C3";

constexpr size_t kInlineSize = 102;
constexpr size_t kFunctionSize = 111;
struct Locations { uint8_t* function; uint8_t* fleetWeight; uint8_t* techWeight; };

inline bool Locate(const locate::Image& img, Locations& out, std::string& error) {
    out.function = locate::FindOne(img, "CFixedPoint::GetRounded(binary)", kFunction, error);
    if (!out.function) return false;
    uint8_t* fleet = locate::FindOne(img, "fleet diplomatic-weight conversion", kFleetWeight, error);
    if (!fleet) return false;
    uint8_t* tech = locate::FindOne(img, "tech diplomatic-weight conversion", kTechWeight, error);
    if (!tech) return false;
    // Both reads use the same country and adjacent cached fleet-diplo/tech fields; both
    // occur in CalcBaseDiplomacyWeight. Validate their relationship, not fixed field offsets.
    if (tech <= fleet || tech - fleet > 0x1000 ||
        static_cast<int64_t>(locate::Disp32(tech + 14)) != static_cast<int64_t>(locate::Disp32(fleet + 15)) + 8) {
        error = "diplomatic-weight conversion cross-check failed";
        return false;
    }
    out.fleetWeight = fleet + 19;
    out.techWeight = tech + 18;
    return true;
}

// Leaf x64 arithmetic: input R8, output R9. Clobbers only RAX/RDX and flags. R10 (the
// live diplomatic accumulator), R11 and all nonvolatile/XMM registers remain untouched.
// Both overflow branches go to a cap chosen from the original input sign. This fits inside
// the original 102-byte inline block; its only branch targets are inside this replacement.
inline std::array<uint8_t, 94> Arithmetic() {
    return {{
        0x49,0x8b,0xd0,                         // mov rdx,r8
        0x49,0x8b,0xc0,                         // mov rax,r8
        0x48,0xc1,0xf8,0x3f,                    // sar rax,63
        0x48,0xc1,0xe8,0x31,                    // shr rax,49: negative correction 32767
        0x48,0x03,0xd0,                         // add rdx,rax
        0x48,0xc1,0xfa,0x0f,                    // sar rdx,15: whole part, toward zero
        0x48,0x8b,0xc2,                         // mov rax,rdx
        0x48,0xc1,0xe0,0x0f,                    // shl rax,15
        0x4d,0x8b,0xc8,                         // mov r9,r8
        0x4c,0x2b,0xc8,                         // sub r9,rax: signed remainder
        0x4d,0x69,0xc9,0xa0,0x86,0x01,0x00,     // imul r9,r9,100000: fits for |remainder|<32768
        0x49,0x8b,0xc1,                         // mov rax,r9
        0x48,0xc1,0xf8,0x3f,                    // sar rax,63
        0x4c,0x03,0xc8,                         // add r9,rax: negative ties round away from zero
        0x49,0x81,0xc1,0x00,0x40,0x00,0x00,     // add r9,16384
        0x49,0xc1,0xf9,0x0f,                    // sar r9,15: rounded decimal fraction
        0x48,0x69,0xd2,0xa0,0x86,0x01,0x00,     // imul rdx,rdx,100000
        0x70,0x05,                              // jo cap
        0x4c,0x03,0xca,                         // add r9,rdx
        0x71,0x12,                              // jno done
        0x49,0xb9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f, // cap: mov r9,INT64_MAX
        0x4d,0x85,0xc0,                         // test r8,r8
        0x79,0x03,                              // jns done
        0x49,0xff,0xc1                          // inc r9: INT64_MIN
    }};
}

inline std::array<uint8_t, kInlineSize> InlinePatch() {
    std::array<uint8_t, kInlineSize> patch;
    patch.fill(0x90);
    auto code = Arithmetic();
    memcpy(patch.data(), code.data(), code.size());
    return patch;
}

inline std::array<uint8_t, kFunctionSize> FunctionPatch() {
    std::array<uint8_t, kFunctionSize> patch;
    patch.fill(0x90);
    const uint8_t prefix[] = {0x4c,0x8b,0x02, 0x4c,0x8b,0xd9}; // mov r8,[rdx]; mov r11,rcx
    const uint8_t suffix[] = {0x4d,0x89,0x0b, 0x49,0x8b,0xc3, 0xc3}; // mov [r11],r9; mov rax,r11; ret
    auto code = Arithmetic();
    memcpy(patch.data(), prefix, sizeof(prefix));
    memcpy(patch.data() + sizeof(prefix), code.data(), code.size());
    memcpy(patch.data() + sizeof(prefix) + code.size(), suffix, sizeof(suffix));
    return patch;
}

} // namespace fixedpointoverflow
