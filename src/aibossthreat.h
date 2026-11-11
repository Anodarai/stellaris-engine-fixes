#pragma once

#include <array>

#include "locate.h"

namespace aibossthreat {

constexpr size_t kPatchBytes = 8;

struct Locations {
    uint8_t* site = nullptr;
    const uint8_t* resume = nullptr;
    int32_t cachedPower = 0;
};

inline bool Locate(const locate::Image& image, Locations& out, std::string& error) {
    const auto boss = locate::FindOne(image, "CalcMilitaryPowerOfHostileFleet's boss branch",
        "F6 83 ?? ?? ?? ?? C0 74 5C 48 8B 0D ?? ?? ?? ?? 49 BA 09 E1 D1 C6 11 6B F1 29 "
        "49 8B C2 48 F7 E9 49 8B C2 4C 8B CA 49 C1 F9 0E 4D 8B C1 49 C1 E8 3F 4D 03 C8 "
        "4D 69 C1 A0 86 01 00 49 C1 E1 0F 49 2B C8 48 C1 E1 1E 48 F7 E9 48 8B C2 "
        "48 C1 F8 1D 48 8B C8 48 C1 E9 3F 48 03 C1 49 03 C1 49 89 06 E9 ?? ?? ?? ??", error);
    if (!boss) return false;
    const auto getter = locate::FindOne(image, "CFleet::GetMilitaryPower's cached read",
        "44 8B 89 ?? ?? ?? ?? 41 8B C1 D1 E8 A8 01 74 21 "
        "80 B9 ?? ?? ?? ?? 0A 75 27 41 C1 E9 09 41 F6 C1 01 74 1D "
        "48 8B 81 ?? ?? ?? ?? 48 89 02 48 8B C2 C3 "
        "80 B9 ?? ?? ?? ?? 04 75 E9 41 83 F8 01 74 E3 48 C7 02 00 00 00 00 48 8B C2 C3", error);
    if (!getter) return false;

    auto site = boss + 0x5d;
    const auto resume = locate::RelTarget(site + 4, site + kPatchBytes);
    const auto epilogue = locate::Parse("48 8B 5C 24 40 49 8B C6 48 8B 6C 24 50 48 83 C4 20 41 5E 5F 5E C3");
    bool foundReturn = false;
    for (const auto& section : image.code) {
        if (resume >= site + kPatchBytes && resume >= section.begin && resume < section.end &&
            static_cast<size_t>(section.end - resume) >= epilogue.size() &&
            locate::MatchAt(resume, epilogue)) {
            foundReturn = true;
            break;
        }
    }
    const auto power = locate::Disp32(getter + 0x26);
    if (!foundReturn || power <= 0 || power % alignof(int64_t) != 0 || power >= locate::Disp32(boss + 2)) {
        error = "boss branch did not agree with the fleet layout or native return";
        return false;
    }
    out = {site, resume, power};
    return true;
}

// RAX holds the estimate in the same fixed-point units as the fleet's cached power.
// Only the boss branch reaches this stub, after native hostility and visibility checks.
inline std::array<uint8_t, 31> ThreatStub(const Locations& found) {
    std::array<uint8_t, 31> code = {
        0x48, 0x8B, 0x8B, 0, 0, 0, 0,  // mov rcx, [rbx+cachedPower]
        0x48, 0x3B, 0xC1,              // cmp rax, rcx
        0x48, 0x0F, 0x4C, 0xC1,        // cmovl rax, rcx
        0x49, 0x89, 0x06,              // mov [r14], rax
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    memcpy(code.data() + 3, &found.cachedPower, sizeof(found.cachedPower));
    memcpy(code.data() + 23, &found.resume, sizeof(found.resume));
    return code;
}

}  // namespace aibossthreat
