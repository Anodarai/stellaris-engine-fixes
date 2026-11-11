#pragma once

#include "locate.h"

namespace aifleettrace {

struct Locations {
    uint8_t* scan;
    uint8_t* collect;
    uint8_t* wanted;
    uint8_t* assign;
    uint8_t* add;
    uint8_t* country;
    uint8_t* armedConflict;
    int32_t systemFleetArray;
    int32_t aiFlags;
};

constexpr size_t kScanStolen = 15;
constexpr size_t kCollectStolen = 15;
constexpr size_t kWantedStolen = 14;
constexpr size_t kAssignStolen = 15;
constexpr size_t kAddStolen = 15;

inline bool Locate(const locate::Image& image, Locations& out) {
    auto unique = [&](const char* pattern) -> uint8_t* {
        auto hits = locate::FindInCode(image, pattern, 2);
        return hits.size() == 1 ? hits[0] : nullptr;
    };
    const auto scan = unique(
        "48 89 5C 24 20 44 88 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 40 "
        "48 8B FA 48 8B D9 48 8B 49 08 48 8B 01 FF 50 28 4C 8B F8");
    const auto collect = unique(
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B F2 48 8B E9 "
        "E8 ?? ?? ?? ?? 48 8B 98 ?? ?? ?? ?? 48 63 80 ?? ?? ?? ?? 48 8D 3C 83 48 3B DF");
    const auto wanted = unique(
        "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? "
        "49 8B F9 4D 63 F8 48 8B F2 4C 8B E9 33 DB 89 5D ?? 4C 8B 75 ?? 4D 85 F6");
    const auto assign = unique(
        "48 89 5C 24 08 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 "
        "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 4D 8B E9 4D 8B F8 48 8B FA 4C 8B E1 33 F6 "
        "41 39 70 14 0F 84 ?? ?? ?? ?? 48 89 74 24 20 45 8B 41 30 48 8D 54 24 ?? E8");
    const auto add = unique(
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 "
        "48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 49 63 F1 48 8B DA 4C 8B E9");
    if (!scan || !collect || !wanted || !assign || !add) return false;

    auto inside = [&](uint8_t* start, size_t length, const char* pattern) -> uint8_t* {
        for (const auto& section : image.code) {
            if (start < section.begin || start >= section.end) continue;
            const auto end = start + (std::min)(length, static_cast<size_t>(section.end - start));
            auto hits = locate::FindAll(start, end, locate::Parse(pattern), 2);
            return hits.size() == 1 ? hits[0] : nullptr;
        }
        return nullptr;
    };
    const auto fleetArray = inside(scan, 0x285,
        "48 8B B7 ?? ?? ?? ?? 48 63 8F ?? ?? ?? ?? 4C 8D 24 8E 49 3B F4 0F 84");
    const auto conflictCall = inside(scan, 0x285,
        "48 C1 E9 0C F6 C1 01 74 0C 48 8B CD E8 ?? ?? ?? ?? 84 C0 75 ?? "
        "41 8B 87 ?? ?? ?? ?? C1 E8 02 A8 01 75");
    const auto countryCall = inside(wanted, 0x100,
        "48 89 1A 48 89 5A 08 E8 ?? ?? ?? ?? 4C 8B E0 49 BB 09 E1 D1 C6 11 6B F1 29");
    const auto strategyReader = unique(
        "48 8B 49 08 48 8B 01 FF 50 28 49 8B CF 48 8B D8 E8 ?? ?? ?? ?? 4C 8B 03 BA 0E 00 00 00 "
        "48 8B CB 48 89 84 24 ?? ?? ?? ?? 41 FF 50 20 4C 8B E8 4C 63 70 14");
    const auto addCall = inside(assign, 0x37b,
        "4C 89 7C 24 20 44 8B 8C 24 ?? ?? ?? ?? 49 8B 94 24 ?? ?? ?? ?? 49 8B CD "
        "E8 ?? ?? ?? ?? 85 C0 74 ?? 83 F8 06");
    if (!fleetArray || !conflictCall || !countryCall || !strategyReader || !addCall) return false;
    if (locate::RelTarget(assign + 0x4e, assign + 0x52) != wanted ||
        locate::RelTarget(addCall + 25, addCall + 29) != add) return false;
    const auto country = locate::RelTarget(countryCall + 8, countryCall + 12);
    if (locate::RelTarget(strategyReader + 17, strategyReader + 21) != country ||
        locate::RelTarget(collect + 27, collect + 31) != country) return false;
    const auto arrayOffset = locate::Disp32(fleetArray + 3);
    if (locate::Disp32(fleetArray + 10) != arrayOffset + 12) return false;
    out = {scan, collect, wanted, assign, add, country,
           locate::RelTarget(conflictCall + 13, conflictCall + 17), arrayOffset,
           locate::Disp32(conflictCall + 24)};
    return true;
}

}  // namespace aifleettrace
