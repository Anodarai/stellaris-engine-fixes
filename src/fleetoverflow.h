// Complete, cross-checked Windows fleet-power paths. Ship power production itself remains vanilla.
#pragma once
#include "locate.h"
#include "numericmath.h"

namespace fleetoverflow {
constexpr char kPower[] =
    "40 56 57 41 56 48 83 EC 20 44 8B 91 ?? ?? ?? ?? 45 0F B6 F1 41 8B C2 48 8B F2 D1 E8 A8 01 0F 84 "
    "A8 00 00 00 80 B9 ?? ?? ?? ?? 0A 0F 85 B2 00 00 00 41 C1 EA 09 41 F6 C2 01 0F 84 A4 00 00 00 48 "
    "63 81 ?? ?? ?? ?? 33 FF 48 89 5C 24 48 48 8B 99 ?? ?? ?? ?? 48 89 6C 24 50 48 8D 2C 83 48 3B DD "
    "74 51 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 24 8B 13 8B C2 25 FF FF FF 00 3B 41 20 73 16 48 8B 49 18 "
    "48 03 C0 48 8B 4C C1 08 48 85 C9 74 05 39 51 ?? 74 07 48 8B 0D ?? ?? ?? ?? 45 0F B6 C6 48 8D 54 "
    "24 40 E8 ?? ?? ?? ?? 48 83 C3 04 48 03 38 48 3B DD 75 AF 48 8B 6C 24 50 48 8B C6 48 8B 5C 24 48 "
    "48 89 3E 48 83 C4 20 41 5E 5F 5E C3 80 B9 ?? ?? ?? ?? 04 0F 85 66 FF FF FF 41 83 F8 01 0F 84 5C "
    "FF FF FF 33 FF 48 8B C6 48 89 3A 48 83 C4 20 41 5E 5F 5E C3";
constexpr char kHealthPower[] =
    "40 57 41 56 48 83 EC 28 4C 8B D1 4C 8B F2 8B 89 ?? ?? ?? ?? 8B C1 D1 E8 A8 01 74 4C 41 80 BA ?? "
    "?? ?? ?? 0A 75 4C C1 E9 09 F6 C1 01 74 44 49 8B 8A ?? ?? ?? ?? 33 FF 48 89 5C 24 48 48 89 6C 24 "
    "50 48 89 74 24 20 48 85 C9 74 37 49 8B 82 ?? ?? ?? ?? 48 C1 E0 0F 48 99 48 F7 F9 4C 8B C0 48 85 "
    "C0 7E 22 48 8B C8 EB 34 41 80 BA ?? ?? ?? ?? 04 75 BC 33 FF 49 8B C6 48 89 3A 48 83 C4 28 41 5E "
    "5F C3 4C 8B C7 49 8B C0 25 FF 7F 00 00 48 F7 D8 48 1B C9 81 E1 00 80 00 00 49 03 C8 49 8B B2 ?? "
    "?? ?? ?? 49 BB 09 E1 D1 C6 11 6B F1 29 48 C1 F9 0F 48 63 D1 48 8B C2 48 C1 E0 0F 4C 2B C0 B8 00 "
    "00 50 C3 4C 0F AF C0 48 8B C7 49 C1 F8 0F 4D 85 C0 0F 9E C0 4C 2B C0 49 81 C0 00 40 00 00 48 69 "
    "C2 A0 86 01 00 49 C1 F8 0F 4D 63 C0 4C 03 C0 49 8B C3 49 F7 E8 49 8B C3 4C 8B CA 49 C1 F9 0E 49 "
    "8B C9 48 C1 E9 3F 4C 03 C9 49 69 C9 A0 86 01 00 49 C1 E1 0F 4C 2B C1 49 C1 E0 1E 49 F7 E8 48 8B "
    "DA 48 C1 FB 1D 48 8B C3 48 C1 E8 3F 48 03 D8 49 63 82 ?? ?? ?? ?? 49 03 D9 48 8D 2C 86 48 3B F5 "
    "74 5B 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 24 8B 16 8B C2 25 FF FF FF 00 3B 41 20 73 16 48 8B 49 18 "
    "48 03 C0 48 8B 4C C1 08 48 85 C9 74 05 39 51 ?? 74 07 48 8B 0D ?? ?? ?? ?? 41 B0 01 48 8D 54 24 "
    "40 E8 ?? ?? ?? ?? 48 8B CB 48 83 C6 04 48 0F AF 08 48 C1 F9 0F 48 03 F9 48 3B F5 75 A5 48 8B 74 "
    "24 20 49 8B C6 48 8B 6C 24 50 48 8B 5C 24 48 49 89 3E 48 83 C4 28 41 5E 5F C3";

constexpr size_t kPowerSize = 244, kHealthSize = 442;
constexpr size_t kPowerStolen = 16, kHealthStolen = 14; // complete position-independent prologue instructions
using PowerFn = int64_t* (__fastcall*)(uint8_t*, int64_t*, int, bool);
using HealthFn = int64_t* (__fastcall*)(uint8_t*, int64_t*);
using ShipFn = int64_t* (__fastcall*)(uint8_t*, int64_t*, bool);
struct Locations {
    uint8_t* power = nullptr;
    uint8_t* healthPower = nullptr;
    uint8_t** shipDb = nullptr;
    uint8_t** nullShip = nullptr;
    ShipFn shipPower = nullptr;
    size_t flags = 0, type = 0, ships = 0, count = 0, currentHealth = 0, maxHealth = 0, shipId = 0;
};

inline bool Locate(const locate::Image& img, Locations& out, std::string& error) {
    uint8_t* a = locate::FindOne(img, "CFleet::CalcMilitaryPower", kPower, error);
    if (!a) return false;
    uint8_t* b = locate::FindOne(img, "CFleet::CalcMilitaryPowerIncludeHealth", kHealthPower, error);
    if (!b) return false;
    auto target = [](uint8_t* p, size_t operand) { return locate::RelTarget(p+operand, p+operand+4); };
    if (locate::Disp32(a+12) != locate::Disp32(b+16) ||
        locate::Disp32(a+38) != locate::Disp32(a+206) ||
        locate::Disp32(a+38) != locate::Disp32(b+31) || locate::Disp32(b+31) != locate::Disp32(b+107) ||
        locate::Disp32(a+80) != locate::Disp32(b+159) || locate::Disp32(a+66) != locate::Disp32(b+306) ||
        a[143] >= 128 || a[143] != b[367] || target(a,101) != target(b,325) ||
        target(a,149) != target(b,373) || target(a,163) != target(b,386)) {
        error = "fleet-power functions disagree about layout or callees"; return false;
    }
    for (int offset : {locate::Disp32(a+12),locate::Disp32(a+38),locate::Disp32(a+80),
                       locate::Disp32(a+66),locate::Disp32(b+78),locate::Disp32(b+49)}) {
        if (offset < 0 || offset > 0x10000) { error = "implausible fleet field offset"; return false; }
    }
    out.power = a; out.healthPower = b;
    out.flags = locate::Disp32(a+12); out.type = locate::Disp32(a+38);
    out.ships = locate::Disp32(a+80); out.count = locate::Disp32(a+66);
    out.currentHealth = locate::Disp32(b+78); out.maxHealth = locate::Disp32(b+49); out.shipId = a[143];
    out.shipDb = reinterpret_cast<uint8_t**>(target(a,101));
    out.nullShip = reinterpret_cast<uint8_t**>(target(a,149));
    out.shipPower = reinterpret_cast<ShipFn>(target(a,163));
    return true;
}

inline uint8_t* Ship(const Locations& loc, uint32_t id) {
    uint8_t* db = *loc.shipDb;
    uint32_t index = id & 0xffffff;
    if (db && index < *reinterpret_cast<uint32_t*>(db + game::kDbCountOffset)) {
        uint8_t* entries = *reinterpret_cast<uint8_t**>(db + game::kDbEntriesOffset);
        uint8_t* ship = *reinterpret_cast<uint8_t**>(entries + 8 + size_t(index)*16);
        if (ship && *reinterpret_cast<uint32_t*>(ship + loc.shipId) == id) return ship;
    }
    return *loc.nullShip;
}

// The outer hook provides SEH. Keep the output untouched until every input was read.
inline bool Compute(const Locations& loc, uint8_t* fleet, int mode, bool cached, bool health, int64_t& result) {
    uint32_t flags = *reinterpret_cast<uint32_t*>(fleet + loc.flags);
    uint8_t type = fleet[loc.type];
    bool excluded = (flags & 2) ? (type != 10 || !(flags & 0x200)) : (type == 4 && (health || mode != 1));
    if (excluded) { result = 0; return true; }
    int32_t count = *reinterpret_cast<int32_t*>(fleet + loc.count);
    if (count < 0) return false;
    int64_t factor = 32768;
    if (health) {
        int64_t current = *reinterpret_cast<int64_t*>(fleet + loc.currentHealth);
        int64_t maximum = *reinterpret_cast<int64_t*>(fleet + loc.maxHealth);
        int64_t ratio = maximum ? numeric::MulDiv(current, 32768, maximum) : 0;
        // Vanilla goes through decimal rounding and back before scaling each ship. Preserve it.
        factor = numeric::DecimalToBinary(numeric::BinaryToDecimal(ratio));
    }
    auto ids = *reinterpret_cast<const uint32_t**>(fleet + loc.ships);
    numeric::Sum total;
    for (int32_t i = 0; i < count; ++i) {
        int64_t power;
        loc.shipPower(Ship(loc, ids[i]), &power, health ? true : cached);
        if (health) power = numeric::MulDiv(power, factor, 32768, numeric::Rounding::Floor);
        total.Add(power);
    }
    result = total.Clamp();
    return true;
}
} // namespace fleetoverflow
