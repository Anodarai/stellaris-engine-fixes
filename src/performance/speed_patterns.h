// Locations for the [speed] savings (simulation_runtime.inl, loadtime_runtime.inl): fleet
// automation's intel lookup, the combat participants map, the AI's megastructure placement, the
// lexer's token strings, the modifier documentation and the deferred references.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "../core/locate.h"

namespace locate {

// --- Fleet automation's intel lookup (fast_intel_lookup) ------------------------------------

// Fleet automation's pathfinding and the intel lookup inside it (re/simulation_speed.md).
struct IntelLocations {
    uint8_t* lookup =
        nullptr; // intel(country, system id, owner id): the country's intel on the owner's fleets there (hooked)
    uint8_t* search = nullptr; // the shortest-path search fleet automation runs; 6 integer arguments (hooked)
    size_t selfObject = 0;     // CCountry: pointer to an object whose +selfId is the country's own id
    size_t selfId = 0;
    uint8_t* selfRelation = nullptr;       // the relation used for the country itself
    size_t relData = 0, relCount = 0;      // CCountry: relations, 0x70 bytes each, owner id first
    size_t relObject = 0;                  // relation object within an entry
    uint8_t** defaultRelation = nullptr;   // the relation used when there is none
    size_t relVirtual = 0;                 // vtable byte offset of the relation's check
    size_t relFlags = 0, relSharedBit = 0; // flags word and the bit for shared intel
    uint8_t** countryDb = nullptr;         // &TPdxRef<CCountry>::_pDatabase
    uint8_t** nullCountry = nullptr;       // &TPdxNullObject<CCountry>::_pInstance
    size_t countryId = 0;
    size_t ownerIntel = 0;              // CCountry: the hash table of intel per system id
    uint8_t* hashFind = nullptr;        // its lookup: int find(table, system id)
    size_t noOwnIntel = 0;              // CCountry: int, non-zero: no intel of its own
    uint8_t* ownIntelExpired = nullptr; // bool f(country): its own intel is out of date
    size_t sysData = 0, sysCount = 0;   // CCountry: per-system entries, 0x28 bytes each
    size_t sysKey = 0;                  // entry: system id
    size_t subData = 0, subCount = 0;   // entry: sub-entries, 0x18 bytes each
    size_t subValue = 0;                // sub-entry: int; the result is the largest, at least 0
};

// The intel lookup, whole (re/simulation_speed.md, "Fleet automation"). Its first 15 bytes (three
// `mov [rsp+N], reg`) are moved by the hook. Strides (0x70, 0x28, 0x18) are fixed in the pattern;
// the offsets read from it are listed in LocateIntel.
constexpr char kIntelLookup[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 81 ?? ?? ?? ?? 41 8B D8 8B FA 48 8B F1 "
    "44 3B 40 ?? 75 ?? 4C 8D 35 ?? ?? ?? ?? EB ?? 48 8B 91 ?? ?? ?? ?? 48 63 81 ?? ?? ?? ?? 4C 6B C0 70 4C 03 C2 "
    "49 3B D0 74 ?? 0F 1F 00 39 1A 74 ?? 48 83 C2 70 49 3B D0 75 ?? 49 3B D0 4C 8D 72 ?? 4C 0F 44 35 ?? ?? ?? ?? "
    "49 8B 06 49 8B CE FF 50 ?? 84 C0 74 ?? 41 8B 46 ?? 48 C1 E8 ?? A8 01 74 ?? 48 8B 15 ?? ?? ?? ?? 48 85 D2 74 ?? "
    "8B C3 25 FF FF FF 00 3B 42 20 73 ?? 8B C8 48 8B 42 18 48 03 C9 48 8B 4C C8 08 48 85 C9 74 ?? 39 59 ?? 74 ?? "
    "48 8B 0D ?? ?? ?? ?? 48 81 C1 ?? ?? ?? ?? 8B D7 E8 ?? ?? ?? ?? EB ?? 83 BE ?? ?? ?? ?? 00 75 ?? 48 8B CE "
    "E8 ?? ?? ?? ?? 84 C0 75 ?? 48 63 86 ?? ?? ?? ?? 48 8B 96 ?? ?? ?? ?? 48 8D 0C 80 48 8D 04 CA 48 3B D0 74 ?? "
    "0F 1F 40 00 39 7A ?? 74 ?? 48 83 C2 28 48 3B D0 75 ?? 33 C0 48 8B 5C 24 30 48 8B 74 24 38 48 8B 7C 24 40 "
    "48 83 C4 20 41 5E C3 48 85 D2 74 ?? 48 63 42 ?? 45 33 C0 4C 8B 4A ?? 48 8D 0C 40 49 8D 04 C9 4C 3B C8 74 ?? "
    "0F 1F 40 00 66 0F 1F 84 00 00 00 00 00 45 3B 41 ?? 45 0F 4C 41 ?? 49 83 C1 18 4C 3B C8 75 ?? 41 8B C0 EB";

constexpr size_t kIntelLookupStolenBytes = 15;

// Fleet automation's call of the search (+35 call): six arguments, the last two on the stack.
constexpr char kSearchCall[] =
    "4C 8B 42 08 48 8D 4C 24 ?? 48 8B 12 4C 8B CF 48 89 44 24 ?? 48 8D 44 24 ?? 48 89 44 24 28 4C 89 54 24 20 "
    "E8 ?? ?? ?? ?? EB";

// The search's prologue: its first 15 bytes (three `mov [rsp+N], reg`) are moved by the hook. Seven
// template copies share it, hence the call site above.
constexpr char kSearchPrologue[] =
    "4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? "
    "48 8D 6C 24 ?? 0F 29 75 ?? 4C 8B E1 48 8B 05 ?? ?? ?? ?? 4C 63 80 ?? ?? ?? ?? 44 89 45 ?? 49 8B D8";

constexpr size_t kSearchStolenBytes = 15;

// Fills `out` for the fast intel lookup; false and `error` on failure.
inline bool LocateIntel(const Image& img, IntelLocations& out, std::string& error) {
    uint8_t* f = FindOne(img, "fleet intel lookup", kIntelLookup, error);
    if (!f)
        return false;
    out.lookup = f;
    out.selfObject = static_cast<size_t>(Disp32(f + 24));
    out.selfId = f[39];
    out.selfRelation = RelTarget(f + 45, f + 49);
    out.relData = static_cast<size_t>(Disp32(f + 54));
    out.relCount = static_cast<size_t>(Disp32(f + 61));
    out.relObject = f[99];
    out.defaultRelation = reinterpret_cast<uint8_t**>(RelTarget(f + 104, f + 108));
    out.relVirtual = f[116];
    out.relFlags = f[124];
    out.relSharedBit = f[128];
    out.countryDb = reinterpret_cast<uint8_t**>(RelTarget(f + 136, f + 140));
    out.countryId = f[178];
    out.nullCountry = reinterpret_cast<uint8_t**>(RelTarget(f + 184, f + 188));
    out.ownerIntel = static_cast<size_t>(Disp32(f + 191));
    out.hashFind = RelTarget(f + 198, f + 202);
    out.noOwnIntel = static_cast<size_t>(Disp32(f + 206));
    out.ownIntelExpired = RelTarget(f + 217, f + 221);
    out.sysCount = static_cast<size_t>(Disp32(f + 228));
    out.sysData = static_cast<size_t>(Disp32(f + 235));
    out.sysKey = f[258];
    out.subCount = f[302];
    out.subData = f[309];
    out.subValue = f[339];
    if (out.relSharedBit > 31 || f[344] != out.subValue) { // cmovl reads the same field as the cmp
        error = "fleet intel lookup: unexpected operands";
        return false;
    }

    uint8_t* call = FindOne(img, "fleet automation's search call", kSearchCall, error);
    if (!call)
        return false;
    out.search = RelTarget(call + 36, call + 40);
    if (!MatchAt(out.search, Parse(kSearchPrologue))) {
        error = "fleet automation's search: unexpected prologue";
        return false;
    }
    return true;
}

// --- Combat participants map (skip_empty_combat_entries) ------------------------------------

// CGameState::MicroUpdate's per-tick map of fleet id -> SCombatParticipants (re/simulation_speed.md).
struct CombatLocations {
    uint8_t* insert = nullptr; // the map's insert: (unused, {entry, inserted} out, hash, id*, value*) (hooked)
    int32_t* count = nullptr;  // the static map's entry count (MicroUpdate empties the map each tick)
    // SCombatParticipants: 7 arrays 0x18 bytes apart, count at +countFirst + 0x18 * i; a 16-bit
    // field at +tail. Read from MicroUpdate's copy of an entry.
    size_t countFirst = 0, countLast = 0, tail = 0;
};

// MicroUpdate, first fleet loop: the end of the fleet id's FNV hash, then the insert of the fleet's
// combat participants (+38 call), then the test of the result's "inserted" flag.
constexpr char kCombatInsertCall[] =
    "41 C1 E8 18 44 33 C0 45 69 C0 93 01 00 01 48 8D 44 24 ?? 48 89 44 24 20 4C 8D 8D ?? ?? ?? ?? "
    "48 8D 95 ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 BD ?? ?? ?? ?? 00 75";

// The insert's prologue: its first 15 bytes (three `mov [rsp+N], reg`) are moved by the hook.
constexpr char kCombatInsertPrologue[] =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 48 8B F2 4D 63 F8 48 63 15";

constexpr size_t kCombatInsertStolenBytes = 15;

// MicroUpdate, second fleet loop: the copy of a found entry. The first array's count and data
// (+33, +37); the last array's count and data (+3, +10) and the 16-bit field (+31).
constexpr char kCombatCopyFirst[] =
    "48 85 FF 0F 84 ?? ?? ?? ?? 4C 89 AD ?? ?? ?? ?? 48 89 B5 ?? ?? ?? ?? 48 89 B5 ?? ?? ?? ?? "
    "44 8B 4F ?? 4C 8B 47 ?? 33 D2";

constexpr char kCombatCopyLast[] =
    "44 8B 8F ?? ?? ?? ?? 4C 8B 87 ?? ?? ?? ?? 33 D2 48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "0F B7 87 ?? ?? ?? ?? 66 89 85";

// Fills `out` with MicroUpdate's combat participants map insert and the entry layout.
inline bool LocateCombat(const Image& img, CombatLocations& out, std::string& error) {
    uint8_t* call = FindOne(img, "combat participants insert call", kCombatInsertCall, error);
    if (!call)
        return false;
    out.insert = RelTarget(call + 39, call + 43);
    if (!MatchAt(out.insert, Parse(kCombatInsertPrologue))) {
        error = "combat participants insert: unexpected prologue";
        return false;
    }
    // The prologue loads the hash mask (+41 movsxd rdx, [rip+X]); the count is the int before it.
    out.count = reinterpret_cast<int32_t*>(RelTarget(out.insert + 44, out.insert + 48) - 4);
    uint8_t* first = FindOne(img, "combat participants copy (first array)", kCombatCopyFirst, error);
    if (!first)
        return false;
    uint8_t* last = FindOne(img, "combat participants copy (last array)", kCombatCopyLast, error);
    if (!last)
        return false;
    // Both copies are in the second loop, after the insert, in this order.
    if (first < call || first - call > 0x1000 || last < first || last - first > 0x200) {
        error = "combat participants copy: not where expected in MicroUpdate";
        return false;
    }
    size_t firstData = first[37], lastData = static_cast<size_t>(Disp32(last + 10));
    out.countFirst = first[33];
    out.countLast = static_cast<size_t>(Disp32(last + 3));
    out.tail = static_cast<size_t>(Disp32(last + 31));
    // Arrays: data pointer then count 0xc further; 7 of them 0x18 apart; the 16-bit field after.
    if (out.countFirst != firstData + 0xc || out.countLast != lastData + 0xc ||
        out.countLast != out.countFirst + 6 * 0x18 || out.tail != out.countLast + 4) {
        error = "combat participants: unexpected entry layout";
        return false;
    }
    return true;
}

// --- AI megastructure placement (mega_afford_cache) -----------------------------------------

// The AI's megastructure placement (re/simulation_speed.md): FindBestPositionForMegaStructure
// scores every reachable system, and each score's CanQueue calls CanAfford(country), which
// doesn't depend on the system.
struct MegaAffordLocations {
    uint8_t* canAfford = nullptr; // CMegaStructureType::CanAfford(type, country, CString* why) (hooked)
    uint8_t* findBest = nullptr;  // CAIInteriorMinister::FindBestPositionForMegaStructure (hooked)
};

// CMegaStructureType::CanQueue's call of CanAfford (+23), unless told to skip the cost:
//   cmp byte [rsp+X],0 / jnz / mov r8,rbp (why) / mov rdx,rbx (country) / mov rcx,rsi (type) / call
constexpr char kCanAffordCall[] =
    "80 BC 24 ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 4C 8B C5 48 8B D3 48 8B CE E8 ?? ?? ?? ?? 84 C0 0F 85";

// CanAfford: its first 15 bytes (push rbp / sub rsp,70h / lea rbp / mov [rbp+60h],rbx) are taken.
constexpr char kCanAffordPrologue[] =
    "40 55 48 83 EC 70 48 8D 6C 24 20 48 89 5D 60 48 89 75 68 48 89 7D 70 49 8B F0 48 8B DA 4C 8B C1 48 8B 05";

constexpr size_t kCanAffordStolenBytes = 15;

// FindBestPositionForMegaStructure: the prologue (15 bytes of spills taken), CollectReachableSystem,
// then the inlined CCrudeRandom seeding.
constexpr char kFindBestPosition[] =
    "48 89 5C 24 20 4C 89 44 24 18 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 4D 8B F9 48 8B F2 4C 8B F1 49 8B D1 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 90 45 33 E4 "
    "44 89 A5 ?? ?? ?? ?? 69 85 ?? ?? ?? ?? B3 85 D6 4A";

constexpr size_t kFindBestPositionStolenBytes = 15;

inline bool LocateMegaAfford(const Image& img, MegaAffordLocations& out, std::string& error) {
    uint8_t* call = FindOne(img, "megastructure CanAfford call", kCanAffordCall, error);
    if (!call)
        return false;
    out.canAfford = RelTarget(call + 24, call + 28);
    if (!MatchAt(out.canAfford, Parse(kCanAffordPrologue))) {
        error = "megastructure CanAfford: unexpected prologue";
        return false;
    }
    out.findBest = FindOne(img, "FindBestPositionForMegaStructure", kFindBestPosition, error);
    return out.findBest != nullptr;
}

// --- Token strings (fast_token_rebuild) -----------------------------------------------------

// The token strings' table: CStaticLexer::RebuildLookup and what it uses (re/load_time.md).
struct LexerLocations {
    uint8_t* rebuild = nullptr;       // CStaticLexer::RebuildLookup() (hooked)
    uint8_t* instance = nullptr;      // returns the lexer
    uint8_t* setSize = nullptr;       // CPdxArray<CString>::SetSizeAndEmplace(array, size)
    uint8_t* assign = nullptr;        // CString::assign(string, text, length)
    size_t array = 0;                 // lexer: the table, a CPdxArray<CString>
    size_t data = 0, count = 0;       // lexer: the table's strings and size
    size_t wanted = 0;                // lexer: how many there should be
    size_t dynData = 0, dynCount = 0; // lexer: the dynamic tokens (CToken*), in id order
    size_t tokenName = 0;             // CToken: char*; its id is the int at +0
};

// CStaticLexer::RebuildLookup, from its start through the loop over the dynamic tokens:
//   +19 call instance / +32 data / +36 count / clear every string / +99 wanted / +106 array /
//   +108 call SetSizeAndEmplace / static tokens, +173 call assign / +199 dynamic count /
//   +211 dynamic data / +226 the token's name / +236 its id / +252 call assign
// Its first 14 bytes (spills, push rsi, rdi, r14) are taken.
constexpr char kRebuildLookup[] =
    "48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 83 EC 20 E8 ?? ?? ?? ?? 45 33 F6 48 8B F8 48 8B 50 ?? 48 63 48 ?? "
    "4C 8D 0C 49 49 C1 E1 04 4C 03 CA 49 3B D1 74 ?? 66 66 66 0F 1F 84 00 00 00 00 00 4C 8B C2 48 83 C2 30 "
    "49 83 78 28 10 49 8D 40 10 72 03 48 8B 00 4D 89 70 20 44 88 30 49 3B D1 75 ?? 8B 97 ?? ?? ?? ?? 48 8D 4F ?? "
    "E8 ?? ?? ?? ?? 49 8B F6 E8 ?? ?? ?? ?? 48 8B 5C 30 10 E8 ?? ?? ?? ?? 49 C7 C0 FF FF FF FF 0F 1F 80 00 00 00 00 "
    "49 FF C0 46 38 34 03 75 F7 48 63 04 30 48 8B D3 48 8D 0C 40 48 C1 E1 04 48 03 4F ?? E8 ?? ?? ?? ?? "
    "48 81 C6 20 01 00 00 48 81 FE ?? ?? ?? ?? 7C ?? 41 8B F6 44 39 77 ?? 7E ?? 49 8B DE 0F 1F 00 48 8B 47 ?? "
    "49 C7 C0 FF FF FF FF 48 8B 0C 03 48 8B 51 ?? 49 FF C0 46 38 34 02 75 F7 48 63 01 48 8D 0C 40 48 C1 E1 04 "
    "48 03 4F ?? E8 ?? ?? ?? ??";

constexpr size_t kRebuildLookupStolenBytes = 14;

inline bool LocateLexer(const Image& img, LexerLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "CStaticLexer::RebuildLookup", kRebuildLookup, error);
    if (!p)
        return false;
    out.rebuild = p;
    out.instance = RelTarget(p + 19, p + 23);
    out.data = p[32];
    out.count = p[36];
    out.wanted = static_cast<size_t>(Disp32(p + 99));
    out.array = p[106];
    out.setSize = RelTarget(p + 108, p + 112);
    out.assign = RelTarget(p + 173, p + 177);
    out.dynCount = p[199];
    out.dynData = p[211];
    out.tokenName = p[226];
    // Both assigns are one function, and every use of the table is the same field.
    if (RelTarget(p + 252, p + 256) != out.assign || p[171] != out.data || p[250] != out.data) {
        error = "CStaticLexer::RebuildLookup: the calls or fields don't agree";
        return false;
    }
    return true;
}

// --- Modifier documentation (skip_modifier_docs) --------------------------------------------

// CModifier::LogDefinitions: only builds the text of modifiers.log and logs it.
struct ModifierDocsLocations {
    uint8_t* logDefinitions = nullptr;
};

// CModifier::LogDefinitions: the prologue (15 bytes of spills taken), then the first appends to
// its text; +92 is the lea of the first one, the documentation's heading.
constexpr char kLogDefinitions[] =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
    "45 33 ED 41 8B F5 44 89 AD ?? ?? ?? ?? 44 89 6C 24 ?? 0F 57 C0 F3 0F 7F 44 24 ?? 66 0F 6F 05 ?? ?? ?? ?? "
    "F3 0F 7F 44 24 ?? 44 88 6C 24 ?? 45 8D 45 ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ??";

constexpr size_t kLogDefinitionsStolenBytes = 15;

constexpr char kLogDefinitionsHeading[] = "\n == MODIFIER DOCUMENTATION ==";

inline bool LocateModifierDocs(const Image& img, ModifierDocsLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "CModifier::LogDefinitions", kLogDefinitions, error);
    if (!p)
        return false;
    const uint8_t* heading = RelTarget(p + 92, p + 96);
    size_t n = sizeof(kLogDefinitionsHeading) - 1;
    bool found = false;
    for (const Section& section : img.data) {
        if (heading >= section.begin && heading + n <= section.end)
            found = memcmp(heading, kLogDefinitionsHeading, n) == 0;
    }
    if (!found) {
        error = "CModifier::LogDefinitions: its first text isn't the documentation's heading";
        return false;
    }
    out.logDefinitions = p;
    return true;
}

// --- Deferred references (fast_deferred_unregister) -----------------------------------------

// The list of references the game resolves once all databases are loaded (re/load_time.md).
struct DeferredRefLocations {
    uint8_t* registerRef =
        nullptr; // Register_Internal(resolver, owner, ...): the only code that adds to the list (hooked)
    uint8_t* unregister =
        nullptr; // Unregister(resolver, owner): removes the owner's entries; scans the whole list (hooked)
    size_t data = 0, count = 0; // resolver: the list and its size
    size_t entrySize = 0;       // the list's entries
    size_t owner = 0;           // entry: the owner
};

// CGlobalDeferredDatabaseObjectResolver::Register_Internal, from its prologue (15 bytes of spills
// taken) to the read of its first string.
constexpr char kDeferredRegister[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8D 6C 24 C1 48 81 EC E0 00 00 00 49 8B F1 4C 8B F2 48 8B F9 45 33 FF "
    "44 89 7C 24 20 4C 89 7D 87 4C 89 7D 8F 66 0F 6F 05 ?? ?? ?? ?? 66 0F 7F 45 9F 44 88 7D 8F 44 89 7D AF 4C 89 7D B7 4C 89 7D BF "
    "66 0F 7F 45 CF 44 88 7D BF 4C 89 7D DF 4C 89 7D 1F 49 8D 50 10 48 83 7A 18 10";

constexpr size_t kDeferredRegisterStolenBytes = 15;

// CGlobalDeferredDatabaseObjectResolver::Unregister, from its prologue (14 bytes taken) to the
// comparison of each entry's owner: +64 count, +81 entry size, +115 data, +120 owner.
constexpr char kDeferredUnregister[] =
    "48 89 54 24 10 48 89 4C 24 08 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 58 0F 29 74 24 40 48 8D 05 ?? ?? ?? ?? 48 89 44 24 20 "
    "45 33 C9 4C 89 4C 24 28 45 33 C0 4C 89 44 24 30 33 DB 44 8B 71 14 41 83 EE 01 0F 88 ?? ?? ?? ?? 49 63 C6 4C 69 F8 A8 00 00 00 "
    "49 8B EF 4C 89 BC 24 B8 00 00 00 4D 8D 51 FF F3 0F 10 35 ?? ?? ?? ?? 0F 1F 40 00 48 8B 41 08 4A 39 54 38 60";

constexpr size_t kDeferredUnregisterStolenBytes = 14;

inline bool LocateDeferredRefs(const Image& img, DeferredRefLocations& out, std::string& error) {
    out.registerRef = FindOne(img, "deferred reference Register_Internal", kDeferredRegister, error);
    if (!out.registerRef)
        return false;
    out.unregister = FindOne(img, "deferred reference Unregister", kDeferredUnregister, error);
    if (!out.unregister)
        return false;
    // Neighbours in the same source file.
    if (out.unregister < out.registerRef || out.unregister - out.registerRef > 0x1000) {
        error = "deferred references: Register_Internal and Unregister are not where expected";
        return false;
    }
    const uint8_t* p = out.unregister;
    out.count = p[64];
    out.entrySize = static_cast<size_t>(Disp32(p + 81));
    out.data = p[115];
    out.owner = p[120];
    return true;
}

} // namespace locate
