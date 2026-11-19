// Locations for the outliner's sorting (outliner_runtime.inl): the fleet comparator and group
// update, and the megastructure group and entry updates. Patterns and offsets come from Ghidra
// (re/fleet_outliner_sort.md).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "../core/locate.h"

namespace locate {

// --- Fleets ---------------------------------------------------------------------------------

struct Locations {
    uint8_t* compare = nullptr;          // SSortFleets::Compare (hooked)
    uint8_t* fleetGroupUpdate = nullptr; // COutlinerGroupFleet::UpdateInternal
    uintptr_t* fleetGroupVtblSlot = nullptr;
    uint8_t** fleetDb = nullptr;     // &TPdxRef<CFleet>::_pDatabase
    uint8_t** shipDb = nullptr;      // &TPdxRef<CShip>::_pDatabase
    size_t fleetIdOffset = 0;        // CFleet: uint32 id
    size_t shipIdOffset = 0;         // CShip: uint32 id
    size_t fleetShipsOffset = 0;     // CFleet: uint32* ship ids
    size_t fleetShipCountOffset = 0; // CFleet: int ship count
    size_t fleetNameSubobject = 0;   // CFleet: sub-object whose vtable builds the display name
    size_t fleetNameVtblOffset = 0;  // byte offset of that vtable slot
    size_t shipDateOffset = 0;       // CShip: construction date
};

// SSortFleets::Compare. Its first 15 bytes (three `mov [rsp+N], reg`) are moved by the hook.
constexpr char kCompare[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 50 48 8B 01 41 8B F8 "
    "8B DA 4C 8B F1 48 8B 70 08 48 63 40 14 48 8D 2C C6";

constexpr size_t kCompareStolenBytes = 15;

// COutlinerGroupFleet::UpdateInternal: prologue with its __chkstk call.
constexpr char kFleetGroupUpdate[] =
    "48 89 5C 24 20 88 54 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 44 0F B6 FA";

// Inside UpdateInternal: a fleet TPdxRef lookup.
//   +0  mov r9,[rip+fleetDb]   ... +53 cmp [rax+fleetIdOffset],edx
constexpr char kFleetDbLoad[] =
    "4C 8B 0D ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 4D 85 C9 74 ?? 8B 16 8B C2 25 FF FF FF 00 41 3B 41 20 73 ?? "
    "8B C8 48 03 C9 49 8B 41 18 48 8B 44 C8 08 48 85 C0 74 ?? 39 50 ??";

constexpr size_t kFleetDbLoadSearch = 0x800; // bytes of UpdateInternal searched

// COutlinerGroupFleet::COutlinerMemberFleet::UpdateEntry: the ship loop, then the name call.
//   +0   mov rbx,[rsi+ships]       +7   movsxd rax,[rsi+count]     +36  mov r8,[rip+shipDb]
//   +81  cmp [rcx+shipId],eax      +114 lea rcx,[rsi+nameSub]      +121 mov r8,[rdx+vtblOffset]
constexpr char kFleetEntryName[] =
    "48 8B 9E ?? ?? ?? ?? 48 63 86 ?? ?? ?? ?? 4C 8D 24 83 49 3B DC 74 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? "
    "8B 03 4C 8B 05 ?? ?? ?? ?? 4D 85 C0 74 ?? 8B C8 81 E1 FF FF FF 00 41 3B 48 20 73 ?? 8B D1 48 03 D2 "
    "49 8B 48 18 48 8B 4C D1 08 48 85 C9 74 ?? 39 41 ?? 74 ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F B6 F8 "
    "84 C0 74 ?? 48 83 C3 04 49 3B DC 75 ?? 48 8D 4E ?? 48 8B 11 4C 8B 42 ?? 48 8D 55 30 41 FF D0";

// CShip::SetConstructionDate: +20 mov [rcx+dateOffset],eax
constexpr char kSetConstructionDate[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 02 48 8B F9 89 81 ?? ?? ?? ?? 48 8B F2 48 8B 81 "
    "?? ?? ?? ?? 48 81 C1";

// Fills `out` for the fleet outliner; on failure returns false and says what was not found in `error`.
inline bool Locate(const Image& img, Locations& out, std::string& error) {
    auto one = [&](const char* what, const char* pattern) { return FindOne(img, what, pattern, error); };

    if (!(out.compare = one("SSortFleets::Compare", kCompare)))
        return false;
    if (!(out.fleetGroupUpdate = one("COutlinerGroupFleet::UpdateInternal", kFleetGroupUpdate)))
        return false;

    size_t slots = 0;
    out.fleetGroupVtblSlot = FindPointer(img, out.fleetGroupUpdate, &slots);
    if (slots != 1) {
        error = "COutlinerGroupFleet vtable slot: " + std::to_string(slots) + " pointers to UpdateInternal";
        return false;
    }

    auto load = FindAll(out.fleetGroupUpdate, out.fleetGroupUpdate + kFleetDbLoadSearch, Parse(kFleetDbLoad), 1);
    if (load.empty()) {
        error = "fleet database load in UpdateInternal: not found";
        return false;
    }
    out.fleetDb = reinterpret_cast<uint8_t**>(RelTarget(load[0] + 3, load[0] + 7));
    out.fleetIdOffset = load[0][55];

    uint8_t* entry = one("COutlinerMemberFleet::UpdateEntry ship loop", kFleetEntryName);
    if (!entry)
        return false;
    out.fleetShipsOffset = static_cast<size_t>(Disp32(entry + 3));
    out.fleetShipCountOffset = static_cast<size_t>(Disp32(entry + 10));
    out.shipDb = reinterpret_cast<uint8_t**>(RelTarget(entry + 39, entry + 43));
    out.shipIdOffset = entry[83];
    out.fleetNameSubobject = entry[117];
    out.fleetNameVtblOffset = entry[124];

    uint8_t* date = one("CShip::SetConstructionDate", kSetConstructionDate);
    if (!date)
        return false;
    out.shipDateOffset = static_cast<size_t>(Disp32(date + 22));
    return true;
}

// The fleet's name function (the vtable slot): this += name offset, then BuildString.
constexpr char kFleetNameThunk[] = "40 53 48 83 EC 20 48 81 C1 ?? ?? ?? ?? 48 8B DA E8";

// True if `fn` is the fleet name function: `this += k; call BuildString`.
inline bool IsFleetNameThunk(const StringLocations& str, const uint8_t* fn) {
    Pattern pat = Parse(kFleetNameThunk);
    if (!MatchAt(fn, pat))
        return false;
    return RelTarget(fn + pat.size(), fn + pat.size() + 4) == str.buildString;
}

// --- Megastructures -------------------------------------------------------------------------

struct MegaLocations {
    uint8_t* groupUpdate = nullptr; // COutlinerGroupMegastructure::UpdateInternal
    uintptr_t* groupVtblSlot = nullptr;
    uint8_t* entryUpdate = nullptr; // COutlinerMemberMegastructure::UpdateEntry(entry, mega) (hooked)
    uint8_t* typeGetName = nullptr; // CMegaStructureType::GetName(type, out)
    uint8_t** megaDb = nullptr;     // &TPdxRef<CMegaStructure>::_pDatabase
    size_t megaIdOffset = 0;        // CMegaStructure: uint32 id
    size_t megaTypeOffset = 0;      // CMegaStructure: CMegaStructureType*
    size_t megaCustomNameFlag = 0;  // CMegaStructure: non-null when it has a name of its own
    size_t megaCustomName = 0;      // CMegaStructure: that CPersistentName
    size_t typeIdOffset = 0;        // CMegaStructureType: uint32 id
};

// COutlinerGroupMegastructure::UpdateInternal: prologue (frame sizes masked).
constexpr char kMegaGroupUpdate[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 44 0F B6 E2 4C 8B F9 48 8D 05";

constexpr size_t kMegaGroupSearch = 0x400; // bytes of UpdateInternal searched by the next three

// The country's megastructure list: +0 mov rdi,[r14+list]  +7 movsxd rax,[r14+count]
constexpr char kMegaList[] = "49 8B BE ?? ?? ?? ?? 49 63 86 ?? ?? ?? ?? 4C 8D 34 87 49 3B FE 0F 84";

// A megastructure TPdxRef lookup: +18 mov rcx,[rip+megaDb]  ... +50 cmp [rbx+megaId],eax
constexpr char kMegaDb[] =
    "48 83 3D ?? ?? ?? ?? 00 74 ?? 8B D0 81 E2 FF FF FF 00 48 8B 0D ?? ?? ?? ?? 3B 51 20 73 ?? 44 8B C2 "
    "4D 03 C0 48 8B 51 18 4A 8B 5C C2 08 48 85 DB 74 ?? 39 43 ??";

// entry = this->GetOrCreateEntry(i); UpdateEntry(entry, mega):  +20 call UpdateEntry
constexpr char kMegaEntryCall[] = "49 8B 07 8B D6 49 8B CF FF 90 ?? ?? ?? ?? 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? FF C6";

// UpdateEntry's first 14 bytes (moved by the hook): mov [rsp+18h],rbx; push rbp/rsi/rdi/r12/r13/r14
constexpr char kMegaEntryPrologue[] = "48 89 5C 24 18 55 56 57 41 54 41 55 41 56";

constexpr size_t kMegaEntryStolenBytes = 14;

// Inside UpdateEntry, the inlined CMegaStructure::GetLocalizedName:
//   +0  cmp [rsi+customFlag],r12   +14 mov rcx,[rsi+type]   +21 call CMegaStructureType::GetName
//   +37 lea rcx,[rsi+customName]   (then BuildString inlined)
constexpr char kMegaName[] =
    "4C 39 A6 ?? ?? ?? ?? 75 ?? 48 8D 54 24 ?? 48 8B 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? 41 BE 01 00 00 00 "
    "E9 ?? ?? ?? ?? 48 8D 8E ?? ?? ?? ?? 0F 57 C0";

// UpdateEntry's change check: +0 mov edx,[rdx+megaId]  +3 mov rax,[rsi+type]  +10 mov ecx,[rax+typeId]
constexpr char kMegaTypeId[] = "8B 52 ?? 48 8B 86 ?? ?? ?? ?? 8B 48 ??";

constexpr size_t kMegaEntrySearch = 0x200;

// Fills `out` for the megastructure outliner (independent of Locate); false and `error` on failure.
inline bool LocateMega(const Image& img, MegaLocations& out, std::string& error) {
    auto in = [&](const char* what, uint8_t* begin, size_t size, const char* pattern) -> uint8_t* {
        auto hits = FindAll(begin, begin + size, Parse(pattern), 1);
        if (hits.empty())
            error = std::string(what) + ": not found";
        return hits.empty() ? nullptr : hits[0];
    };

    auto hits = FindInCode(img, kMegaGroupUpdate, 2);
    if (hits.size() != 1) {
        error = std::string("COutlinerGroupMegastructure::UpdateInternal: ") +
                (hits.empty() ? "not found" : "more than one match");
        return false;
    }
    out.groupUpdate = hits[0];
    size_t slots = 0;
    out.groupVtblSlot = FindPointer(img, out.groupUpdate, &slots);
    if (slots != 1) {
        error = "COutlinerGroupMegastructure vtable slot: " + std::to_string(slots) + " pointers to UpdateInternal";
        return false;
    }

    uint8_t* db = in("megastructure database load", out.groupUpdate, kMegaGroupSearch, kMegaDb);
    if (!db)
        return false;
    out.megaDb = reinterpret_cast<uint8_t**>(RelTarget(db + 21, db + 25));
    out.megaIdOffset = db[52];

    uint8_t* call = in("UpdateEntry call", out.groupUpdate, kMegaGroupSearch, kMegaEntryCall);
    if (!call)
        return false;
    out.entryUpdate = RelTarget(call + 21, call + 25);
    if (!MatchAt(out.entryUpdate, Parse(kMegaEntryPrologue))) {
        error = "COutlinerMemberMegastructure::UpdateEntry: unexpected prologue";
        return false;
    }

    uint8_t* ids = in("UpdateEntry type id", out.entryUpdate, kMegaEntrySearch, kMegaTypeId);
    if (!ids)
        return false;
    if (ids[2] != out.megaIdOffset) {
        error = "UpdateEntry and UpdateInternal disagree on the megastructure id offset";
        return false;
    }
    out.typeIdOffset = ids[12];

    uint8_t* name = in("UpdateEntry name", out.entryUpdate, kMegaEntrySearch, kMegaName);
    if (!name)
        return false;
    out.megaCustomNameFlag = static_cast<size_t>(Disp32(name + 3));
    out.megaTypeOffset = static_cast<size_t>(Disp32(name + 17));
    out.typeGetName = RelTarget(name + 22, name + 26);
    out.megaCustomName = static_cast<size_t>(Disp32(name + 40));
    if (static_cast<size_t>(Disp32(ids + 6)) != out.megaTypeOffset) {
        error = "UpdateEntry reads the megastructure type at two different offsets";
        return false;
    }
    return true;
}

} // namespace locate
