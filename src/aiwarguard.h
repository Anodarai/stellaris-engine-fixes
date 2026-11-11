#pragma once

#include <array>

#include "locate.h"

namespace aiwarguard {

constexpr uint32_t kDeclareWar = 0x2d52;
constexpr uint32_t kPreparing = 4;
constexpr uint32_t kNeedStrategicWarData = 0x100;
constexpr size_t kScoreStolen = 15;
constexpr size_t kRefreshStolen = 15;

struct Locations {
    uint8_t* score = nullptr;
    uint8_t* createEmptyAction = nullptr;
    uint8_t* initAction = nullptr;
    uint8_t* isPossible = nullptr;
    uint8_t* deleteAction = nullptr;
    uint8_t* refresh = nullptr;
    uint8_t** countryDb = nullptr;
    uint8_t relationCountry = 0;
    uint8_t countryId = 0;
    uint32_t aiFlags = 0;
};

inline bool Locate(const locate::Image& image, Locations& out, std::string& error) {
    auto one = [&](const char* what, const char* pattern) {
        return locate::FindOne(image, what, pattern, error);
    };
    const auto score = one("CalcGoingToWarScoreAgainst",
        "48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? "
        "45 33 F6 49 8B F1 41 F6 80 ?? ?? ?? ?? 01 49 8B D8 4C 8B FA 4C 8B E1 74 ??");
    if (!score) return false;
    const auto lookup = one("war score's target lookup",
        "48 8B 15 ?? ?? ?? ?? 48 85 D2 74 ?? 45 8B 40 ?? 41 8B C0 25 FF FF FF 00 3B 42 20 73 ?? "
        "8B C8 48 8B 42 18 48 03 C9 48 8B 7C C8 08 48 85 FF 74 ?? 44 39 47 ?? 74 ?? "
        "48 8B 3D ?? ?? ?? ?? 48 8B 87 ?? ?? ?? ?? 0F B6 48 48 C0 E9 07");
    if (!lookup) return false;
    const auto factoryCall = one("CreateDiplomaticAction's factory call",
        "8B 49 50 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 4C 8B 08 4C 8B C7 48 8B D6 48 8B C8 41 FF 51 38");
    if (!factoryCall) return false;
    const auto warCase = one("CreateEmptyAction's declare-war case",
        "B9 90 00 00 00 E8 ?? ?? ?? ?? 48 8B F8 48 89 45 ?? BA 52 2D 00 00 48 8B C8 "
        "E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 07 48 8D 4F 50 E8");
    if (!warCase) return false;
    const auto toggle = one("CAIPrepareWarCommand::Execute's preparation flag",
        "0F B6 43 ?? BA ?? ?? 00 00 48 8B BF ?? ?? ?? ?? F7 D8 "
        "33 87 ?? ?? ?? ?? 83 E0 04 31 87 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ??");
    if (!toggle) return false;
    const auto clear = one("CAIUpdateStrategicWarData::Execute's dirty flag",
        "81 A7 ?? ?? ?? ?? FF FE FF FF 48 8B 9F ?? ?? ?? ?? 48 85 DB 74 ?? "
        "E8 ?? ?? ?? ?? F6 40 38 01 74");
    if (!clear) return false;

    auto inSection = [](const auto& sections, const uint8_t* p, size_t size) {
        for (const auto& section : sections) {
            if (p >= section.begin && p < section.end && size <= static_cast<size_t>(section.end - p)) {
                return true;
            }
        }
        return false;
    };
    const auto factory = locate::RelTarget(factoryCall + 4, factoryCall + 8);
    const auto vtable = locate::RelTarget(warCase + 33, warCase + 37);
    const auto countryDb = locate::RelTarget(lookup + 3, lookup + 7);
    if (lookup != score + 0x35 || !inSection(image.code, factory, 1) ||
        warCase < factory || warCase - factory > 0x1200 ||
        !inSection(image.data, vtable, 0x58) ||
        !inSection(image.data, countryDb, sizeof(uint8_t*))) {
        error = "war score or declaration factory did not agree with its references";
        return false;
    }
    auto method = [&](size_t slot) -> uint8_t* {
        uintptr_t pointer;
        memcpy(&pointer, vtable + slot, sizeof(pointer));
        auto address = reinterpret_cast<uint8_t*>(pointer);
        if (inSection(image.code, address, 1)) return address;
        if (pointer < image.pointerBase || pointer - image.pointerBase > UINT32_MAX) return nullptr;
        address = image.base + (pointer - image.pointerBase);
        return inSection(image.code, address, 1) ? address : nullptr;
    };
    const auto destroy = method(0);
    const auto init = method(0x38);
    const auto possible = method(0x50);
    constexpr char kRuleCall[] =
        "4D 8B CE 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 0F B6 E0 84 C0";
    if (!destroy || !init || !possible || !inSection(image.code, possible, 0x100) ||
        locate::FindAll(possible, possible + 0x100, locate::Parse(kRuleCall), 2).size() != 1) {
        error = "declaration action vtable did not lead to the native war rule";
        return false;
    }
    const auto refresh = toggle + 18;
    const auto flags = locate::Disp32(refresh + 2);
    if (flags <= 0 || flags % alignof(uint32_t) != 0 ||
        locate::Disp32(refresh + 11) != flags || locate::Disp32(clear + 2) != flags) {
        error = "war preparation and strategic refresh disagree about the AI flags";
        return false;
    }
    out = {score, factory, init, possible, destroy, refresh, reinterpret_cast<uint8_t**>(countryDb),
           lookup[15], lookup[51], static_cast<uint32_t>(flags)};
    return true;
}

// Runs the game's flag change first. EAX then holds the changed bits. Request a strategic
// refresh only on a transition from preparing to idle, using the existing AI dirty flag.
inline std::array<uint8_t, 52> RefreshStub(const Locations& found, const void* resume) {
    std::array<uint8_t, 52> code = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xA8, kPreparing,                     // test al, preparing
        0x74, 0x13,                           // jz resume
        0xF6, 0x87, 0, 0, 0, 0, kPreparing,   // test byte ptr [rdi+flags], preparing
        0x75, 0x0A,                           // jnz resume
        0x81, 0x8F, 0, 0, 0, 0, 0, 0, 0, 0,  // or dword ptr [rdi+flags], need_strategic_war_data
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    memcpy(code.data(), found.refresh, kRefreshStolen);
    memcpy(code.data() + 21, &found.aiFlags, sizeof(found.aiFlags));
    memcpy(code.data() + 30, &found.aiFlags, sizeof(found.aiFlags));
    memcpy(code.data() + 34, &kNeedStrategicWarData, sizeof(kNeedStrategicWarData));
    memcpy(code.data() + 44, &resume, sizeof(resume));
    return code;
}

}  // namespace aiwarguard
