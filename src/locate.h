// Finds everything stellaris-engine-fixes uses in stellaris.exe by code patterns, so it keeps working when
// a game update moves code around. Struct offsets the game's code itself uses are read out of
// the matched instructions instead of being hard-coded.
//
// Works on a mapped image: the running game, or the file mapped as an image (tests).
// Patterns and offsets come from Ghidra (re/fleet_outliner_sort.md, re/sigs_gen.txt).
#pragma once
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "game.h"

namespace locate {

struct Locations {
    uint8_t* compare = nullptr;           // SSortFleets::Compare (hooked)
    uint8_t* fleetGroupUpdate = nullptr;  // COutlinerGroupFleet::UpdateInternal
    uintptr_t* fleetGroupVtblSlot = nullptr;
    uint8_t** fleetDb = nullptr;          // &TPdxRef<CFleet>::_pDatabase
    uint8_t** shipDb = nullptr;           // &TPdxRef<CShip>::_pDatabase
    size_t fleetIdOffset = 0;             // CFleet: uint32 id
    size_t shipIdOffset = 0;              // CShip: uint32 id
    size_t fleetShipsOffset = 0;          // CFleet: uint32* ship ids
    size_t fleetShipCountOffset = 0;      // CFleet: int ship count
    size_t fleetNameSubobject = 0;        // CFleet: sub-object whose vtable builds the display name
    size_t fleetNameVtblOffset = 0;       // byte offset of that vtable slot
    size_t shipDateOffset = 0;            // CShip: construction date
};

// Game string helpers both features use.
struct StringLocations {
    uint8_t* buildString = nullptr;  // CPersistentName::BuildString(name, out)
    uint8_t* gameFree = nullptr;     // the allocator's free
};

struct MegaLocations {
    uint8_t* groupUpdate = nullptr;       // COutlinerGroupMegastructure::UpdateInternal
    uintptr_t* groupVtblSlot = nullptr;
    uint8_t* entryUpdate = nullptr;       // COutlinerMemberMegastructure::UpdateEntry(entry, mega) (hooked)
    uint8_t* typeGetName = nullptr;       // CMegaStructureType::GetName(type, out)
    uint8_t** megaDb = nullptr;           // &TPdxRef<CMegaStructure>::_pDatabase
    size_t megaIdOffset = 0;              // CMegaStructure: uint32 id
    size_t megaTypeOffset = 0;            // CMegaStructure: CMegaStructureType*
    size_t megaCustomNameFlag = 0;        // CMegaStructure: non-null when it has a name of its own
    size_t megaCustomName = 0;            // CMegaStructure: that CPersistentName
    size_t typeIdOffset = 0;              // CMegaStructureType: uint32 id
};

// The console, the tick loop and frame rendering, for tick pacing and the benchmark command
// (re/simulation_speed.md).
struct TickLocations {
    int32_t* commandsPerTurn = nullptr;  // &NDefines::NGameplay::NUM_COMMANDS_PER_TURN: ticks queued per frame
    int32_t* ticksPerTurn = nullptr;     // &g_nTicksPerTurn (console ticks_per_turn; 1)
    uint8_t** gameState = nullptr;       // &g_CurrentGameState
    size_t idlerMultiplayer = 0;         // CInGameIdler: bool multiplayer
    uint8_t* execute = nullptr;      // CConsoleCmdManager::Execute(manager, SResult* out, const CPdxArray<CString>& args) (hooked)
    size_t consoleLockedFlag = 0;    // CConsoleCmdManager: bool, set in multiplayer and ironman
    uint8_t* runCommand = nullptr;   // CConsole::RunCommandNow(console, const CString& line)
    uint8_t* addToHistory = nullptr; // &g_bAddToHistory: RunCommandNow records the line in the history and console_history.txt
    uint8_t** console = nullptr;     // &CConsole::_pInstance
    uint8_t* checkTick = nullptr;    // CInGameIdler::CheckTick(idler, float dt): once per frame (hooked)
    size_t idlerSpeed = 0;           // CInGameIdler: int game speed index
    size_t idlerPaused = 0;          // CInGameIdler: bool paused
    float* speedTurnTimes = nullptr; // gGameSpeedTurnTimes: seconds between tick batches, per speed
    uint8_t* turnTick = nullptr;     // CGameState::HandleTurnTick(state, checksums): one tick (hooked)
    size_t stateTick = 0;            // CGameState: int tick of the day
    size_t stateDate = 0;            // CGameState: int date in hours
    uint8_t* renderFrame = nullptr;  // CGameState::RenderFrame: a frame drawn from inside the simulation (hooked)
    uint8_t* frameSmoothing = nullptr;  // &g_bFrameSmoothing (the console command `smooth`)
};

// Fleet automation's pathfinding and the intel lookup inside it (re/simulation_speed.md).
struct IntelLocations {
    uint8_t* lookup = nullptr;   // intel(country, system id, owner id): the country's intel on the owner's fleets there (hooked)
    uint8_t* search = nullptr;   // the shortest-path search fleet automation runs; 6 integer arguments (hooked)
    size_t selfObject = 0;       // CCountry: pointer to an object whose +selfId is the country's own id
    size_t selfId = 0;
    uint8_t* selfRelation = nullptr;     // the relation used for the country itself
    size_t relData = 0, relCount = 0;    // CCountry: relations, 0x70 bytes each, owner id first
    size_t relObject = 0;                // relation object within an entry
    uint8_t** defaultRelation = nullptr; // the relation used when there is none
    size_t relVirtual = 0;               // vtable byte offset of the relation's check
    size_t relFlags = 0, relSharedBit = 0;  // flags word and the bit for shared intel
    uint8_t** countryDb = nullptr;       // &TPdxRef<CCountry>::_pDatabase
    uint8_t** nullCountry = nullptr;     // &TPdxNullObject<CCountry>::_pInstance
    size_t countryId = 0;
    size_t ownerIntel = 0;               // CCountry: the hash table of intel per system id
    uint8_t* hashFind = nullptr;         // its lookup: int find(table, system id)
    size_t noOwnIntel = 0;               // CCountry: int, non-zero: no intel of its own
    uint8_t* ownIntelExpired = nullptr;  // bool f(country): its own intel is out of date
    size_t sysData = 0, sysCount = 0;    // CCountry: per-system entries, 0x28 bytes each
    size_t sysKey = 0;                   // entry: system id
    size_t subData = 0, subCount = 0;    // entry: sub-entries, 0x18 bytes each
    size_t subValue = 0;                 // sub-entry: int; the result is the largest, at least 0
};

// CGameState::MicroUpdate's per-tick map of fleet id -> SCombatParticipants (re/simulation_speed.md).
struct CombatLocations {
    uint8_t* insert = nullptr;  // the map's insert: (unused, {entry, inserted} out, hash, id*, value*) (hooked)
    int32_t* count = nullptr;   // the static map's entry count (MicroUpdate empties the map each tick)
    // SCombatParticipants: 7 arrays 0x18 bytes apart, count at +countFirst + 0x18 * i; a 16-bit
    // field at +tail. Read from MicroUpdate's copy of an entry.
    size_t countFirst = 0, countLast = 0, tail = 0;
};

// The AI's megastructure placement (re/simulation_speed.md): FindBestPositionForMegaStructure
// scores every reachable system, and each score's CanQueue calls CanAfford(country), which
// doesn't depend on the system.
struct MegaAffordLocations {
    uint8_t* canAfford = nullptr;  // CMegaStructureType::CanAfford(type, country, CString* why) (hooked)
    uint8_t* findBest = nullptr;   // CAIInteriorMinister::FindBestPositionForMegaStructure (hooked)
};

// CAIEconomicPlan::CalculateIncomeTarget's scaling loop (re/ai.md section 6). Each round adds the
// income of the plan's scaling subplans to the total until some resource's total passes the
// target; there is no limit on the rounds, and a large empire can need a billion of them.
struct IncomeTargetLocations {
    uint8_t* loop = nullptr;         // the round's start: the target check (rbx total, r12 target, r13d rounds)
    uint8_t* scalingCall = nullptr;  // call CalcScalingSubplans, the first thing in each round (redirected)
    uint8_t* calcScaling = nullptr;  // CAIEconomicPlan::CalcScalingSubplans(plan, out list, scope)
};

// The country AI's gameplay fixes, [ai_fixes] in stellaris-engine-fixes.ini (re/ai.md, sections 2 and 4).
// Each part is located on its own; a part that isn't found leaves only its fix off.
struct AiFixLocations {
    // 2.1 The build plan. CAIInteriorMinister::BuildAndUpgradeDistrictsAndBuildings walks the plan
    // CAIEconomicStrategy::CreateBuildPlan makes once a month, stops at the first item it can't
    // place, then clears the whole plan.
    uint8_t* planWalk = nullptr;        // BuildAndUpgradeDistrictsAndBuildings(minister) (hooked)
    size_t ministerStrategy = 0;        // CAIInteriorMinister: CAIEconomicStrategy*
    size_t planArray = 0;               // CAIEconomicStrategy: the plan, a CPdxArray of {CBuildableBase*, CFixedPoint}
    size_t arrayData = 0, arrayCount = 0;  // CPdxArray: data pointer, int count
    uint8_t* planClearCall = nullptr;   // the walk's `call Clear` at its end (redirected)
    uint8_t* planClear = nullptr;       // the plan's Clear(array): deletes every item, count = 0
    uint8_t** nullBuildable = nullptr;  // &TPdxNullObject<CNullBuildable>::_pInstance, which Clear doesn't delete
    uint8_t* resolveColony = nullptr;   // NBuildPlan::ResolveColony(buildable) -> CColony*
    size_t colonyCheckObject = 0;       // CColony: sub-object whose vtable slot says the colony is usable
    size_t colonyCheckSlot = 0;         // that slot's byte offset
    uint8_t* hasQueued = nullptr;       // CColony::HasQueuedBuildingOrDistrictConstruction(colony)

    // 2.3 The opportunist pass of CCountryAI::IsReadyForWarWith (replaced from its first byte to
    // `join`). Registers there: r14 our country, [rbp+targetSlot] the target, rsi the first pass's
    // sum, rdi the second's (0); after it r9, r10 and r11 hold what the game left in them.
    uint8_t* opportunist = nullptr;
    uint8_t* opportunistJoin = nullptr;
    size_t targetSlot = 0;
    size_t countryWars = 0, countryWarCount = 0;  // CCountry: uint32 war ids, int count
    size_t countryPersonality = 0;                // CCountry: personality
    size_t personalityOpportunist = 0;            // personality: bool opportunist
    size_t countryId = 0;                         // CCountry: uint32 id
    size_t countryPower = 0;                      // CCountry: int64 fleet power; half of it is added
    size_t warId = 0;                             // CWar: uint32 id
    size_t warAttackers = 0, warDefenders = 0;    // CWar: CPdxArray of participants (data +8, count +0x14)
    size_t memberStride = 0, memberId = 0;        // participant: uint32 country id at +memberId
    uint8_t** countryDb = nullptr;
    uint8_t** nullCountry = nullptr;
    uint8_t** warDb = nullptr;
    uint8_t** nullWar = nullptr;

    // 2.2 CAIForeignMinister::CalcGoingToWarScoreAgainst's distance malus:
    // cmp ebp,[DISTANCE]; jle; movsxd rax,ebp; imul rax,[MALUS] (kWarDistanceBytes, rewritten).
    uint8_t* warDistance = nullptr;
    int32_t* malusDistance = nullptr;  // &NDefines::NAI::WAR_DECLARATION_MALUS_DISTANCE
    int64_t* malus = nullptr;          // &NDefines::NAI::WAR_DECLARATION_MALUS, CFixedPoint
    uint8_t* malusSkip = nullptr;      // where the jle goes

    // 2.4 CAIForeignMinister::PrepareWar's `idiv dword [AI_AGGRESSIVENESS_BOXED_IN_MULT]` when
    // there is a truce; truceOperand is its rel32 (redirected).
    uint8_t* truceOperand = nullptr;
    int32_t* boxedInMult = nullptr;

    // 2.5 CCountryAI::CreateStrategicData: the `jl` taken while fewer than AI_BUDGETING_START_YEARS
    // have passed, which rebuilds the colonize data every month (made unconditional).
    uint8_t* expansionYears = nullptr;

    // 2.9 CAICore::Update's day counters, and the AI's own random generator.
    uint8_t* aiUpdate = nullptr;     // CAICore::Update(core) (hooked)
    size_t coreId = 0;               // CAICore: int id (the country's), set by the constructor
    size_t randomPosition = 0, randomSeed = 0;  // CAICore: the unsynced generator
    size_t weekCounter = 0, monthCounter = 0, yearCounter = 0;  // CAICore: int days left
    uint8_t* savedRandomJumps[2] = {};  // CCountryAI::ReadMember: `jz` past storing random_count / random_seed
    uint8_t* useSavedRandom = nullptr;  // &g_bUseSavedRandom (-loadrandomseed), which those test
};

// The token strings' table: CStaticLexer::RebuildLookup and what it uses (re/load_time.md).
struct LexerLocations {
    uint8_t* rebuild = nullptr;  // CStaticLexer::RebuildLookup() (hooked)
    uint8_t* instance = nullptr;  // returns the lexer
    uint8_t* setSize = nullptr;   // CPdxArray<CString>::SetSizeAndEmplace(array, size)
    uint8_t* assign = nullptr;    // CString::assign(string, text, length)
    size_t array = 0;             // lexer: the table, a CPdxArray<CString>
    size_t data = 0, count = 0;   // lexer: the table's strings and size
    size_t wanted = 0;            // lexer: how many there should be
    size_t dynData = 0, dynCount = 0;  // lexer: the dynamic tokens (CToken*), in id order
    size_t tokenName = 0;              // CToken: char*; its id is the int at +0
};

// CModifier::LogDefinitions: only builds the text of modifiers.log and logs it.
struct ModifierDocsLocations {
    uint8_t* logDefinitions = nullptr;
};

// The list of references the game resolves once all databases are loaded (re/load_time.md).
struct DeferredRefLocations {
    uint8_t* registerRef = nullptr;  // Register_Internal(resolver, owner, ...): the only code that adds to the list (hooked)
    uint8_t* unregister = nullptr;   // Unregister(resolver, owner): removes the owner's entries; scans the whole list (hooked)
    size_t data = 0, count = 0;      // resolver: the list and its size
    size_t entrySize = 0;            // the list's entries
    size_t owner = 0;                // entry: the owner
};

// The statically linked CRT's heap functions: the only code that touches the CRT heap (Ghidra:
// the only callers of HeapAlloc/HeapFree/HeapReAlloc/HeapSize on it). free, operator new,
// _aligned_malloc/_aligned_free, _recalloc and the rest all end in these five.
struct CrtHeapLocations {
    uint8_t* malloc = nullptr;   // _malloc_base(size)
    uint8_t* calloc = nullptr;   // _calloc_base(count, size)
    uint8_t* realloc = nullptr;  // _realloc_base(block, size)
    uint8_t* free = nullptr;     // _free_base(block)
    uint8_t* msize = nullptr;    // _msize_base(block)
    HANDLE* heap = nullptr;      // &__acrt_heap: null until the CRT initializes
};

// A pattern: bytes, with -1 for "any byte".
using Pattern = std::vector<int>;

inline Pattern Parse(const char* text) {
    Pattern p;
    for (const char* s = text; *s;) {
        if (*s == ' ') {
            ++s;
        } else if (s[0] == '?') {
            p.push_back(-1);
            s += s[1] == '?' ? 2 : 1;
        } else {
            p.push_back(static_cast<int>(strtoul(std::string(s, 2).c_str(), nullptr, 16)));
            s += 2;
        }
    }
    return p;
}

inline bool MatchAt(const uint8_t* p, const Pattern& pat) {
    for (size_t i = 0; i < pat.size(); ++i) {
        if (pat[i] >= 0 && p[i] != pat[i]) return false;
    }
    return true;
}

// All matches of `pat` in [begin, end), at most `limit`. `anchor` is the index of a fixed byte to
// search for first; the rarer that byte, the faster.
inline std::vector<uint8_t*> FindAll(uint8_t* begin, uint8_t* end, const Pattern& pat, size_t limit = 8,
                                     size_t anchor = 0) {
    std::vector<uint8_t*> hits;
    if (pat.empty() || end - begin < static_cast<ptrdiff_t>(pat.size())) return hits;
    if (pat[anchor] < 0) {
        for (uint8_t* p = begin; p + pat.size() <= end && hits.size() < limit; ++p) {
            if (MatchAt(p, pat)) hits.push_back(p);
        }
        return hits;
    }
    const uint8_t* last = end - pat.size() + anchor;  // the last place the anchor can match
    for (const uint8_t* a = begin + anchor; a <= last && hits.size() < limit; ++a) {
        a = static_cast<const uint8_t*>(memchr(a, pat[anchor], last - a + 1));
        if (!a) break;
        uint8_t* p = const_cast<uint8_t*>(a) - anchor;
        if (MatchAt(p, pat)) hits.push_back(p);
    }
    return hits;
}

inline int32_t Disp32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// Target of a rel32/RIP-relative operand at `operand`, in an instruction ending at `next`.
inline uint8_t* RelTarget(const uint8_t* operand, const uint8_t* next) {
    return const_cast<uint8_t*>(next) + Disp32(operand);
}

// --- Patterns ------------------------------------------------------------------------------

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
constexpr size_t kFleetDbLoadSearch = 0x800;  // bytes of UpdateInternal searched

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

// CPersistentName::BuildString(name, out): +99 call free (its scratch array's buffer).
constexpr char kBuildString[] =
    "48 89 5C 24 08 57 48 83 EC 60 48 8B DA 0F 57 C0 0F 11 44 24 20 48 C7 44 24 30 08 00 00 00 "
    "48 8D 3D ?? ?? ?? ?? 48 89 7C 24 20 48 8D 44 24 38 48 89 44 24 28 45 33 C9 4C 8D 44 24 20 "
    "E8 ?? ?? ?? ?? 90 48 89 7C 24 20 C7 44 24 34 00 00 00 00 48 8D 44 24 38 48 8B 4C 24 28 48 3B C8 "
    "74 ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B C3";

// The fleet's name function (the vtable slot): this += name offset, then BuildString.
constexpr char kFleetNameThunk[] = "40 53 48 83 EC 20 48 81 C1 ?? ?? ?? ?? 48 8B DA E8";

// COutlinerGroupMegastructure::UpdateInternal: prologue (frame sizes masked).
constexpr char kMegaGroupUpdate[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 44 0F B6 E2 4C 8B F9 48 8D 05";
constexpr size_t kMegaGroupSearch = 0x400;  // bytes of UpdateInternal searched by the next three

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

// CConsoleCmdManager::Execute. Its first 15 bytes (three `mov [rsp+N], reg`) are moved by the hook.
//   +82 cmp [rcx+releaseFlag],r12b   +91 cmp [rcx+lockedFlag],r12b (multiplayer or ironman)
constexpr char kConsoleExecute[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC ?? ?? ?? ?? "
    "49 8B F8 48 8B F2 48 8B D9 C6 45 ?? 00 45 33 E4 44 89 65 ?? 0F 57 C0 F3 0F 7F 45 ?? 66 0F 6F 05 ?? ?? ?? ?? "
    "F3 0F 7F 45 ?? 44 88 65 ?? 44 38 A1 ?? ?? ?? ?? 74 ?? 44 38 A1 ?? ?? ?? ?? 74";
constexpr size_t kConsoleExecuteStolenBytes = 15;

// OnExecute_UnchartedSpace runs another command: RunCommandNow(CConsole::_pInstance, "TerraIncognita").
//   +64 mov rcx,[rip+console]   +71 call RunCommandNow
constexpr char kRunTerraIncognita[] =
    "48 C7 44 24 48 0F 00 00 00 48 C7 44 24 40 0E 00 00 00 F2 0F 10 05 ?? ?? ?? ?? F2 0F 11 44 24 30 "
    "8B 05 ?? ?? ?? ?? 89 44 24 38 0F B7 05 ?? ?? ?? ?? 66 89 44 24 3C 40 88 7C 24 3E 48 8D 54 24 20 "
    "48 8B 0D ?? ?? ?? ?? E8";
//   +44 cmp byte [rip+g_bAddToHistory],r15b
constexpr char kRunCommandPrologue[] =
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B DA 4C 8B F1 "
    "45 33 FF 41 8B F7 44 89 7D ?? 44 38 3D ?? ?? ?? ?? 0F 84";
constexpr size_t kRunCommandSearch = 0x200;  // RunCommandNow calls Execute within this many bytes

// CInGameIdler::CheckTick. Its first 16 bytes (mov [rsp+8],rbx and seven pushes) are moved by the hook.
//   +92 cmp byte [rcx+paused],0
constexpr char kCheckTick[] =
    "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 0F 29 74 24 50 0F 28 F1 48 8B E9 "
    "4C 8B 15 ?? ?? ?? ?? 4D 85 D2 0F 84 ?? ?? ?? ?? 41 80 BA A0 00 00 00 00 0F 84 ?? ?? ?? ?? "
    "48 8B 81 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 80 ?? ?? ?? ?? C1 E8 06 A8 01 0F 85 ?? ?? ?? ?? "
    "80 B9 ?? ?? ?? ?? 00 0F 85";
constexpr size_t kCheckTickStolenBytes = 16;
// Inside CheckTick: +0 movsxd rax,[rbp+speed]  +7 lea rcx,[rip+gGameSpeedTurnTimes]  +14 movss xmm1,[rcx+rax*4]
constexpr char kCheckTickSpeed[] = "48 63 85 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? F3 0F 10 0C 81";
// Inside CheckTick, the multiplayer check: +0 movzx edi,byte [rbp+multiplayer]  +23 mov eax,[r10+tick]
constexpr char kCheckTickPlayers[] = "0F B6 BD ?? ?? ?? ?? 49 8B D5 48 8B D9 41 8B 82 ?? ?? ?? ?? 8D 0C 80 41 8B 42 ?? 8D 34 48";
// Inside CheckTick, the tick count: max(min(NUM_COMMANDS_PER_TURN, 10), g_nTicksPerTurn)
//   +0 lea rax,[rip+commands]  +14 cmp [rip+commands],10  +25 mov eax,[rip+ticksPerTurn]  +31 lea rcx,[rip+ticksPerTurn]
constexpr char kCheckTickBatch[] =
    "48 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 0A 48 0F 4C D0 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? "
    "3B 02 48 0F 4C CA 44 8B 21";
constexpr size_t kCheckTickSearch = 0x200;

// CGameState::HandleTurnTick. Its first 16 bytes (mov [rsp+10h],rbx and seven pushes) are moved by the hook.
//   +125 mov rax,[rip+g_CurrentInGameIdler]  +139 mov byte [rax+inTick],1  +158 call RenderFrame
//   +190 inc dword [rsi+tick]  +204 mov ebx,[rsi+tick]  +207 lea rcx,[rsi+date]
constexpr char kTurnTick[] =
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
    "48 8B DA 48 8B F1 45 33 E4 44 89 A5 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0F 48 8B 89 40 01 00 00 "
    "48 8B 01 FF 50 70 EB 03 49 8B C4 44 38 A0 ?? ?? ?? ?? 75 ?? 44 39 63 14 74 ?? 48 8B D3 48 8B CE "
    "E8 ?? ?? ?? ?? 84 C0 74 06 66 C7 46 40 01 00 48 8D 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 07 C6 80 ?? ?? ?? ?? 01 33 D2 48 8B CE E8 ?? ?? ?? ?? 84 C0 74 05 "
    "E8 ?? ?? ?? ?? 45 32 ED 44 88 A5 ?? ?? ?? ?? 45 32 FF 44 88 BD ?? ?? ?? ?? 44 88 A5 ?? ?? ?? ?? "
    "FF 46 ?? E8 ?? ?? ?? ?? F6 40 38 01 74 ?? 8B 5E ?? 48 8D 8E ?? ?? ?? ??";
constexpr size_t kTurnTickStolenBytes = 16;

// CGameState::RenderFrame. Its first 15 bytes (three `mov [rsp+N], reg`) are moved by the hook.
//   +26 mov rax,[rip+g_CurrentInGameIdler]  +40 movzx ebx,byte [rax+inTick]
constexpr char kRenderFrame[] =
    "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 48 89 54 24 10 41 56 48 83 EC 40 48 8B 05 ?? ?? ?? ?? "
    "4C 8B B0 40 01 00 00 0F B6 98 ?? ?? ?? ?? C6 80 ?? ?? ?? ?? 01";
constexpr size_t kRenderFrameStolenBytes = 15;

// CGameState::RenderFrameBetweenCommands: +56 cmp byte [rip+g_bFrameSmoothing],0  +65 cmp [rip+g_nTicksPerTurn],1
constexpr char kRenderBetweenCommands[] =
    "40 53 48 83 EC 20 80 B9 A0 00 00 00 00 48 8B D9 74 ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 80 B9 ?? ?? ?? ?? 00 "
    "75 ?? F2 0F 10 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 80 3D ?? ?? ?? ?? 00 74 ?? 83 3D ?? ?? ?? ?? 01";

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

// CalculateIncomeTarget's round: for each resource i, leave if (target >= 0 || total > 0) &&
// target < total (r9 the target's table from [r12], r10 the total's from [rbx]); leave if the
// table is empty or the last round added nothing (sil); else sil = 0 and call CalcScalingSubplans
// (+100). The three exits (+57, +73, +82) go to one place; the round ends with inc r13d / jmp here.
constexpr char kIncomeTargetLoop[] =
    "33 C9 4D 8B 0C 24 45 8B 41 0C 45 85 C0 7E ?? 4D 8B 09 48 8B 03 4C 8B 10 0F 1F 84 00 00 00 00 00 "
    "48 63 C1 49 8B 14 C1 48 85 D2 79 ?? 49 83 3C C2 00 7E ?? 49 3B 14 C2 0F 8C ?? ?? ?? ?? FF C1 41 3B C8 "
    "7C ?? 45 85 C0 0F 84 ?? ?? ?? ?? 40 84 F6 0F 84 ?? ?? ?? ?? 40 32 F6 4D 8B C7 48 8D 55 ?? 48 8B 4D ?? E8";
constexpr size_t kIncomeTargetCall = 100;
constexpr char kIncomeTargetRoundEnd[] = "41 FF C5 E9";  // inc r13d / jmp the loop
constexpr size_t kIncomeTargetRoundSearch = 0x200;

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

// CModifier::LogDefinitions: the prologue (15 bytes of spills taken), then the first appends to
// its text; +92 is the lea of the first one, the documentation's heading.
constexpr char kLogDefinitions[] =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
    "45 33 ED 41 8B F5 44 89 AD ?? ?? ?? ?? 44 89 6C 24 ?? 0F 57 C0 F3 0F 7F 44 24 ?? 66 0F 6F 05 ?? ?? ?? ?? "
    "F3 0F 7F 44 24 ?? 44 88 6C 24 ?? 45 8D 45 ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ??";
constexpr size_t kLogDefinitionsStolenBytes = 15;
constexpr char kLogDefinitionsHeading[] = "\n == MODIFIER DOCUMENTATION ==";

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

// --- Image ---------------------------------------------------------------------------------

struct Section {
    uint8_t* begin;
    uint8_t* end;
};

struct Image {
    uint8_t* base = nullptr;
    uintptr_t pointerBase = 0;  // what absolute pointers in the image are relative to
    std::vector<Section> code;  // executable sections
    std::vector<Section> data;  // initialized, readable, not executable
    size_t byteCounts[256] = {};  // how common each byte is in the code, from a sample
};

inline Image Describe(uint8_t* base) {
    Image img;
    img.base = base;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    // The loader rewrites ImageBase when it relocates the image; a file mapped as an image
    // resource keeps the preferred base, matching its unrelocated pointers.
    img.pointerBase = static_cast<uintptr_t>(nt->OptionalHeader.ImageBase);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        Section s{base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            img.code.push_back(s);
            // A sample is enough to tell which bytes are rare.
            for (const uint8_t* p = s.begin; p < s.end; p += 61) ++img.byteCounts[*p];
        } else if ((sec->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) &&
                   (sec->Characteristics & IMAGE_SCN_MEM_READ)) {
            img.data.push_back(s);
        }
    }
    return img;
}

inline std::vector<uint8_t*> FindInCode(const Image& img, const char* text, size_t limit = 8) {
    Pattern pat = Parse(text);
    size_t anchor = 0;
    for (size_t i = 0; i < pat.size(); ++i) {
        if (pat[i] >= 0 && (pat[anchor] < 0 || img.byteCounts[pat[i]] < img.byteCounts[pat[anchor]])) anchor = i;
    }
    std::vector<uint8_t*> hits;
    for (const Section& s : img.code) {
        for (uint8_t* h : FindAll(s.begin, s.end, pat, limit - hits.size(), anchor)) hits.push_back(h);
        if (hits.size() >= limit) break;
    }
    return hits;
}

// The one 8-byte slot in a data section holding a pointer to `fn` (a vtable entry), or null.
inline uintptr_t* FindPointer(const Image& img, const uint8_t* fn, size_t* count) {
    uintptr_t want = img.pointerBase + static_cast<uintptr_t>(fn - img.base);
    uintptr_t* found = nullptr;
    *count = 0;
    for (const Section& s : img.data) {
        auto p = reinterpret_cast<uintptr_t*>((reinterpret_cast<uintptr_t>(s.begin) + 7) & ~uintptr_t(7));
        for (; reinterpret_cast<uint8_t*>(p + 1) <= s.end; ++p) {
            if (*p == want) {
                found = p;
                ++*count;
            }
        }
    }
    return found;
}

// --- The AI fixes (AiFixLocations) ---------------------------------------------------------

// BuildAndUpgradeDistrictsAndBuildings: prologue (21 bytes taken: 8 pushes and lea rbp), then the
// strategy (+44, again +69) and the plan's count (+76) read for the HasEconomicPlan check.
constexpr char kPlanWalk[] =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 4C 8B E9 45 33 F6 "
    "44 89 B5 ?? ?? ?? ?? 48 8B 81 ?? ?? ?? ?? 48 8B 48 08 48 8B 01 FF 50 08 84 C0 0F 84 ?? ?? ?? ?? 49 8B 85 "
    "?? ?? ?? ?? 44 39 70 ?? 75 ?? 44 39 70 ?? 0F 84";
constexpr size_t kPlanWalkStolenBytes = 21;
// In the walk: rcx = strategy + planArray (+10), kept at [rsp+X] (+15); count read at +0x14.
constexpr char kPlanWalkArray[] = "49 8B 8D ?? ?? ?? ?? 48 83 C1 ?? 48 89 4C 24 ?? 45 33 E4 48 63 41 14";
// The walk's end: a destructor call, then rcx = [rsp+X] (+13) and `call Clear` (+14), the epilogue.
constexpr char kPlanWalkClear[] =
    "48 8D 4D ?? E8 ?? ?? ?? ?? 48 8B 4C 24 ?? E8 ?? ?? ?? ?? 48 81 C4 ?? ?? ?? ?? 41 5F 41 5E 41 5D 41 5C 5F 5E 5B "
    "5D C3";
constexpr size_t kPlanWalkSearch = 0x800;
// Clear(array): count at +13, data at +35, the null buildable at +43.
constexpr char kPlanClear[] =
    "48 89 74 24 10 57 48 83 EC 20 48 63 79 14 48 8B F1 48 85 FF 7E ?? 48 89 5C 24 30 33 DB 0F 1F 00 48 8B 46 08 "
    "48 8B 0C 03 48 3B 0D ?? ?? ?? ?? 74 ?? 48 85 C9 74 ?? 48 8B 01 BA 01 00 00 00 FF 10";
// The walk's item check: ResolveColony (+10), the colony's check (+20, slot +27), then
// HasQueuedBuildingOrDistrictConstruction (+40); either failing stops the walk.
constexpr char kPlanItemCheck[] =
    "4C 8B F2 4C 8B E1 48 8B 0A E8 ?? ?? ?? ?? 4C 8B F8 48 8D 48 ?? 4C 8B 01 41 FF 50 ?? 84 C0 0F 84 ?? ?? ?? ?? "
    "49 8B CF E8 ?? ?? ?? ?? 84 C0 0F 85";

// IsReadyForWarWith: the opportunist pass, from its start (15 bytes there are replaced) into its
// inner loop, GetAllies inlined. Offsets in LocateOpportunist.
constexpr char kOpportunistPass[] =
    "4C 8B 9D ?? ?? ?? ?? 41 83 BB ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 49 8B 86 ?? ?? ?? ?? 80 78 ?? 00 0F 84 ?? ?? ?? ?? "
    "41 8B 86 ?? ?? ?? ?? 85 C0 0F 8E ?? ?? ?? ?? 41 8B 4B ?? 4C 8B E2 44 8B E8 48 8B 35 ?? ?? ?? ?? EB 02 33 D2 "
    "48 85 F6 74 ?? 49 8B 86 ?? ?? ?? ?? 45 8B 04 04 41 8B C0 25 FF FF FF 00 3B 46 20 73 ?? 8B D0 48 03 D2 48 8B 46 "
    "18 4C 8B 5C D0 08 4D 85 DB 74 ?? 45 39 43 ?? 75 ?? 33 D2 EB ?? 33 D2 4D 8B DA 44 8B C2 49 63 83 ?? ?? ?? ?? 85 "
    "C0 7E ?? 4C 8B D0 49 8B 83 ?? ?? ?? ?? 48 83 C0 ?? 39 08 74 ?? 41 FF C0 48 FF C2 48 83 C0 ?? 49 3B D2 7C ?? EB "
    "?? 41 83 F8 FF 41 BF ?? ?? ?? ?? 75 ?? 41 BF ?? ?? ?? ?? 43 8B 44 3B ?? 85 C0 7E ?? 45 33 D2 44 8B F0 4D 85 C9 "
    "74 ?? 4B 8B 44 3B ?? 45 8B 44 02 ?? 41 8B C0 25 FF FF FF 00 41 3B 41 20 73 ?? 8B D0 48 03 D2 49 8B 41 18 48 8B "
    "44 D0 08 48 85 C0 74 ?? 44 39 40 ?? 74 ?? 48 8B C3 48 8B 80 ?? ?? ?? ??";
constexpr size_t kOpportunistStolenBytes = 15;
// Before the first pass: r9 = country database, rbx = null country, r10 = null war.
constexpr char kOpportunistGlobals[] =
    "41 8B 86 ?? ?? ?? ?? 4C 8B 0D ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 4C 8B 15 ?? ?? ?? ?? 85 C0 0F 8E";
constexpr size_t kOpportunistGlobalsSearch = 0x200;  // bytes before the pass
// Where both passes join: mov rsi,[rsp+X]; mov r11,[rbp+target] (+12 is the join).
constexpr char kOpportunistJoin[] =
    "48 8B 74 24 ?? 4C 8B 9D ?? ?? ?? ?? 48 8B 95 ?? ?? ?? ?? 48 8B 0A 48 03 CF 48 8B 42 10 48 03 C6";

// CalcGoingToWarScoreAgainst: `cmovnz ebp,r14d` (no distance when bordering), then the malus.
constexpr char kWarDistance[] =
    "40 84 F6 41 B9 A0 86 01 00 41 0F 45 EE 3B 2D ?? ?? ?? ?? 7E ?? 48 63 C5 48 0F AF 05 ?? ?? ?? ?? 4C 2B C8 49 "
    "8B C1 4C 8B 0D ?? ?? ?? ?? 4C 3B C8 4C 0F 4C C8";
constexpr size_t kWarDistanceAt = 13, kWarDistanceBytes = 19;

// PrepareWar: imul ebx,[BOXED_IN_MULT] (boxed in, no truce), then the colony count branch and the
// truce's division (+41: idiv dword [BOXED_IN_MULT]).
constexpr char kTruceDivide[] =
    "0F AF 1D ?? ?? ?? ?? EB ?? 8B 0D ?? ?? ?? ?? 3B F1 0F 8D ?? ?? ?? ?? 2B CE 8B C3 99 F7 F9 8B D8 80 7D ?? 00 74 "
    "?? 8B C3 99 F7 3D ?? ?? ?? ?? 8B D8";

// CreateStrategicData: id % 12 against the month, then the year check (+60: jl) and the flags.
constexpr char kExpansionRefresh[] =
    "8B 4F ?? B8 AB AA AA AA F7 E1 C1 EA 03 8D 04 52 C1 E0 02 2B C8 48 8B 05 ?? ?? ?? ?? 3B 88 ?? ?? ?? ?? 74 ?? 48 "
    "8D 88 ?? ?? ?? ?? E8 ?? ?? ?? ?? 2B 05 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 7C ?? 8B 8B ?? ?? ?? ?? 8B C1 C1 E8 05 A8 01 "
    "74 ?? C1 E9 0B F6 C1 01 74";

// CAICore::Update: the three counters (dec, cmp 0, jg, call the update, reset to 7 / 30 / 360).
constexpr char kAiCounters[] =
    "FF 50 ?? FF 8F ?? ?? ?? ?? 83 BF ?? ?? ?? ?? 00 7F ?? 48 8B 07 48 8B CF FF 50 ?? C7 87 ?? ?? ?? ?? 07 00 00 00 "
    "FF 8F ?? ?? ?? ?? 83 BF ?? ?? ?? ?? 00 7F ?? 48 8B 07 48 8B CF FF 50 ?? C7 87 ?? ?? ?? ?? 1E 00 00 00 FF 8F ?? "
    "?? ?? ?? 83 BF ?? ?? ?? ?? 00 7F ?? 48 8B 07 48 8B CF FF 50 ?? C7 87 ?? ?? ?? ?? 68 01 00 00";
// Its prologue (18 bytes taken), searched for backwards from the counters.
constexpr char kAiUpdatePrologue[] = "48 89 4C 24 08 55 41 54 41 55 41 56 41 57 48 83 EC ?? 48 8D 6C 24 ??";
constexpr size_t kAiUpdateStolenBytes = 18;
constexpr size_t kAiUpdateSearch = 0x600;
// CAICore::CAICore: id, position and seed all set to the id argument, then the week counter id % 7.
constexpr char kAiCoreInit[] =
    "89 9F ?? ?? ?? ?? 89 9F ?? ?? ?? ?? 89 9F ?? ?? ?? ?? 44 8B 87 ?? ?? ?? ?? 45 85 C0 7E ?? B8 93 24 49 92 41 F7 "
    "E8 41 03 D0 C1 FA 02 8B C2 C1 E8 1F 03 D0 6B C2 07 41 8B C8 2B C8 89 8F";
// CCountryAI::ReadMember, random_count and random_seed: cmp [g_bUseSavedRandom],bl; jz (+6, 6
// bytes); store. Two matches.
constexpr char kSavedRandom[] = "38 1D ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 8B 45 ?? 89 47 ??";

// --- Locate --------------------------------------------------------------------------------

// The single match of `pattern` in code, or null with `error` set.
inline uint8_t* FindOne(const Image& img, const char* what, const char* pattern, std::string& error) {
    auto hits = FindInCode(img, pattern, 2);
    if (hits.size() != 1) {
        error = std::string(what) + (hits.empty() ? ": not found" : ": more than one match");
        return nullptr;
    }
    return hits[0];
}

inline bool LocateStrings(const Image& img, StringLocations& out, std::string& error) {
    if (!(out.buildString = FindOne(img, "CPersistentName::BuildString", kBuildString, error))) return false;
    out.gameFree = RelTarget(out.buildString + 100, out.buildString + 104);
    return true;
}

// Fills `out` for the fleet outliner; on failure returns false and says what was not found in `error`.
inline bool Locate(const Image& img, Locations& out, std::string& error) {
    auto one = [&](const char* what, const char* pattern) { return FindOne(img, what, pattern, error); };

    if (!(out.compare = one("SSortFleets::Compare", kCompare))) return false;
    if (!(out.fleetGroupUpdate = one("COutlinerGroupFleet::UpdateInternal", kFleetGroupUpdate))) return false;

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
    if (!entry) return false;
    out.fleetShipsOffset = static_cast<size_t>(Disp32(entry + 3));
    out.fleetShipCountOffset = static_cast<size_t>(Disp32(entry + 10));
    out.shipDb = reinterpret_cast<uint8_t**>(RelTarget(entry + 39, entry + 43));
    out.shipIdOffset = entry[83];
    out.fleetNameSubobject = entry[117];
    out.fleetNameVtblOffset = entry[124];

    uint8_t* date = one("CShip::SetConstructionDate", kSetConstructionDate);
    if (!date) return false;
    out.shipDateOffset = static_cast<size_t>(Disp32(date + 22));
    return true;
}

// Fills `out` for the megastructure outliner (independent of Locate); false and `error` on failure.
inline bool LocateMega(const Image& img, MegaLocations& out, std::string& error) {
    auto in = [&](const char* what, uint8_t* begin, size_t size, const char* pattern) -> uint8_t* {
        auto hits = FindAll(begin, begin + size, Parse(pattern), 1);
        if (hits.empty()) error = std::string(what) + ": not found";
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
    if (!db) return false;
    out.megaDb = reinterpret_cast<uint8_t**>(RelTarget(db + 21, db + 25));
    out.megaIdOffset = db[52];

    uint8_t* call = in("UpdateEntry call", out.groupUpdate, kMegaGroupSearch, kMegaEntryCall);
    if (!call) return false;
    out.entryUpdate = RelTarget(call + 21, call + 25);
    if (!MatchAt(out.entryUpdate, Parse(kMegaEntryPrologue))) {
        error = "COutlinerMemberMegastructure::UpdateEntry: unexpected prologue";
        return false;
    }

    uint8_t* ids = in("UpdateEntry type id", out.entryUpdate, kMegaEntrySearch, kMegaTypeId);
    if (!ids) return false;
    if (ids[2] != out.megaIdOffset) {
        error = "UpdateEntry and UpdateInternal disagree on the megastructure id offset";
        return false;
    }
    out.typeIdOffset = ids[12];

    uint8_t* name = in("UpdateEntry name", out.entryUpdate, kMegaEntrySearch, kMegaName);
    if (!name) return false;
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

// Whether code at `begin` calls `target` within `size` bytes (any E8 byte that decodes to it).
inline bool CallsWithin(const Image& img, const uint8_t* begin, size_t size, const uint8_t* target) {
    for (const Section& s : img.code) {
        if (begin < s.begin || begin >= s.end) continue;
        const uint8_t* end = begin + size < s.end ? begin + size : s.end;
        for (const uint8_t* p = begin; p + 5 <= end; ++p) {
            if (*p == 0xE8 && RelTarget(p + 1, p + 5) == target) return true;
        }
    }
    return false;
}

// Fills `out` for the benchmark command; false and `error` on failure.
inline bool LocateTicks(const Image& img, TickLocations& out, std::string& error) {
    auto one = [&](const char* what, const char* pattern) { return FindOne(img, what, pattern, error); };
    auto in = [&](const char* what, uint8_t* begin, size_t size, const char* pattern) -> uint8_t* {
        auto hits = FindAll(begin, begin + size, Parse(pattern), 1);
        if (hits.empty()) error = std::string(what) + ": not found";
        return hits.empty() ? nullptr : hits[0];
    };

    if (!(out.execute = one("CConsoleCmdManager::Execute", kConsoleExecute))) return false;
    out.consoleLockedFlag = static_cast<size_t>(Disp32(out.execute + 91));

    uint8_t* run = one("OnExecute_UnchartedSpace's RunCommandNow call", kRunTerraIncognita);
    if (!run) return false;
    out.console = reinterpret_cast<uint8_t**>(RelTarget(run + 67, run + 71));
    out.runCommand = RelTarget(run + 72, run + 76);
    if (!MatchAt(out.runCommand, Parse(kRunCommandPrologue))) {
        error = "CConsole::RunCommandNow: unexpected prologue";
        return false;
    }
    out.addToHistory = RelTarget(out.runCommand + 47, out.runCommand + 51);
    bool callsExecute = false;
    for (uint8_t* p = out.runCommand; p < out.runCommand + kRunCommandSearch && !callsExecute; ++p) {
        callsExecute = *p == 0xE8 && RelTarget(p + 1, p + 5) == out.execute;
    }
    if (!callsExecute) {
        error = "CConsole::RunCommandNow does not call the Execute found";
        return false;
    }

    if (!(out.checkTick = one("CInGameIdler::CheckTick", kCheckTick))) return false;
    out.idlerPaused = static_cast<size_t>(Disp32(out.checkTick + 92));
    uint8_t* speed = in("CheckTick game speed", out.checkTick, kCheckTickSearch, kCheckTickSpeed);
    if (!speed) return false;
    out.idlerSpeed = static_cast<size_t>(Disp32(speed + 3));
    out.speedTurnTimes = reinterpret_cast<float*>(RelTarget(speed + 10, speed + 14));
    out.gameState = reinterpret_cast<uint8_t**>(RelTarget(out.checkTick + 34, out.checkTick + 38));
    uint8_t* players = in("CheckTick multiplayer check", out.checkTick, kCheckTickSearch, kCheckTickPlayers);
    if (!players) return false;
    out.idlerMultiplayer = static_cast<size_t>(Disp32(players + 3));
    uint8_t* batch = in("CheckTick tick count", out.checkTick, kCheckTickSearch, kCheckTickBatch);
    if (!batch) return false;
    out.commandsPerTurn = reinterpret_cast<int32_t*>(RelTarget(batch + 3, batch + 7));
    out.ticksPerTurn = reinterpret_cast<int32_t*>(RelTarget(batch + 27, batch + 31));
    if (reinterpret_cast<int32_t*>(RelTarget(batch + 16, batch + 21)) != out.commandsPerTurn ||
        reinterpret_cast<int32_t*>(RelTarget(batch + 34, batch + 38)) != out.ticksPerTurn) {
        error = "CheckTick's tick count reads two different globals";
        return false;
    }

    if (!(out.turnTick = one("CGameState::HandleTurnTick", kTurnTick))) return false;
    out.stateTick = out.turnTick[192];
    out.stateDate = static_cast<size_t>(Disp32(out.turnTick + 210));
    if (out.turnTick[206] != out.stateTick || players[26] != out.stateTick) {
        error = "HandleTurnTick and CheckTick read the tick count at different offsets";
        return false;
    }

    if (!(out.renderFrame = one("CGameState::RenderFrame", kRenderFrame))) return false;
    if (RelTarget(out.turnTick + 159, out.turnTick + 163) != out.renderFrame ||
        RelTarget(out.turnTick + 128, out.turnTick + 132) != RelTarget(out.renderFrame + 29, out.renderFrame + 33) ||
        Disp32(out.turnTick + 139) != Disp32(out.renderFrame + 43)) {
        error = "HandleTurnTick and RenderFrame disagree (call target, idler or in-tick flag)";
        return false;
    }

    uint8_t* between = one("CGameState::RenderFrameBetweenCommands", kRenderBetweenCommands);
    if (!between) return false;
    out.frameSmoothing = RelTarget(between + 58, between + 63);
    return true;
}

// Fills `out` for the fast intel lookup; false and `error` on failure.
inline bool LocateIntel(const Image& img, IntelLocations& out, std::string& error) {
    uint8_t* f = FindOne(img, "fleet intel lookup", kIntelLookup, error);
    if (!f) return false;
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
    if (out.relSharedBit > 31 || f[344] != out.subValue) {  // cmovl reads the same field as the cmp
        error = "fleet intel lookup: unexpected operands";
        return false;
    }

    uint8_t* call = FindOne(img, "fleet automation's search call", kSearchCall, error);
    if (!call) return false;
    out.search = RelTarget(call + 36, call + 40);
    if (!MatchAt(out.search, Parse(kSearchPrologue))) {
        error = "fleet automation's search: unexpected prologue";
        return false;
    }
    return true;
}

// Fills `out` with MicroUpdate's combat participants map insert and the entry layout.
inline bool LocateCombat(const Image& img, CombatLocations& out, std::string& error) {
    uint8_t* call = FindOne(img, "combat participants insert call", kCombatInsertCall, error);
    if (!call) return false;
    out.insert = RelTarget(call + 39, call + 43);
    if (!MatchAt(out.insert, Parse(kCombatInsertPrologue))) {
        error = "combat participants insert: unexpected prologue";
        return false;
    }
    // The prologue loads the hash mask (+41 movsxd rdx, [rip+X]); the count is the int before it.
    out.count = reinterpret_cast<int32_t*>(RelTarget(out.insert + 44, out.insert + 48) - 4);
    uint8_t* first = FindOne(img, "combat participants copy (first array)", kCombatCopyFirst, error);
    if (!first) return false;
    uint8_t* last = FindOne(img, "combat participants copy (last array)", kCombatCopyLast, error);
    if (!last) return false;
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

// Fills `out` with the CRT heap functions; false and `error` if any is missing or they disagree.
inline bool LocateIncomeTarget(const Image& img, IncomeTargetLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "CalculateIncomeTarget's scaling loop", kIncomeTargetLoop, error);
    if (!p) return false;
    uint8_t* exit = RelTarget(p + 57, p + 61);
    if (RelTarget(p + 73, p + 77) != exit || RelTarget(p + 82, p + 86) != exit || exit < p + 105) {
        error = "CalculateIncomeTarget's scaling loop: its exits go to different places";
        return false;
    }
    out.loop = p;
    out.scalingCall = p + kIncomeTargetCall;
    out.calcScaling = RelTarget(p + 101, p + 105);
    // The round ends with inc r13d and a jump back here: r13d is the round counter.
    auto ends = FindAll(p + 105, p + 105 + kIncomeTargetRoundSearch, Parse(kIncomeTargetRoundEnd), 4);
    if (ends.size() != 1 || RelTarget(ends[0] + 4, ends[0] + 8) != p) {
        error = "CalculateIncomeTarget's scaling loop: the round's end not found";
        return false;
    }
    return true;
}

inline bool LocateMegaAfford(const Image& img, MegaAffordLocations& out, std::string& error) {
    uint8_t* call = FindOne(img, "megastructure CanAfford call", kCanAffordCall, error);
    if (!call) return false;
    out.canAfford = RelTarget(call + 24, call + 28);
    if (!MatchAt(out.canAfford, Parse(kCanAffordPrologue))) {
        error = "megastructure CanAfford: unexpected prologue";
        return false;
    }
    out.findBest = FindOne(img, "FindBestPositionForMegaStructure", kFindBestPosition, error);
    return out.findBest != nullptr;
}

inline bool LocateLexer(const Image& img, LexerLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "CStaticLexer::RebuildLookup", kRebuildLookup, error);
    if (!p) return false;
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

inline bool LocateDeferredRefs(const Image& img, DeferredRefLocations& out, std::string& error) {
    out.registerRef = FindOne(img, "deferred reference Register_Internal", kDeferredRegister, error);
    if (!out.registerRef) return false;
    out.unregister = FindOne(img, "deferred reference Unregister", kDeferredUnregister, error);
    if (!out.unregister) return false;
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

// The match of `pattern` in [begin, begin + size), or null with `error` set.
inline uint8_t* FindWithin(uint8_t* begin, size_t size, const char* what, const char* pattern, std::string& error) {
    auto hits = FindAll(begin, begin + size, Parse(pattern), 1);
    if (hits.empty()) error = std::string(what) + ": not found";
    return hits.empty() ? nullptr : hits[0];
}

inline bool LocateModifierDocs(const Image& img, ModifierDocsLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "CModifier::LogDefinitions", kLogDefinitions, error);
    if (!p) return false;
    const uint8_t* heading = RelTarget(p + 92, p + 96);
    size_t n = sizeof(kLogDefinitionsHeading) - 1;
    bool found = false;
    for (const Section& section : img.data) {
        if (heading >= section.begin && heading + n <= section.end) found = memcmp(heading, kLogDefinitionsHeading, n) == 0;
    }
    if (!found) {
        error = "CModifier::LogDefinitions: its first text isn't the documentation's heading";
        return false;
    }
    out.logDefinitions = p;
    return true;
}

inline bool LocateCrtHeap(const Image& img, CrtHeapLocations& out, std::string& error) {
    struct Fn {
        const char* what;
        const char* pattern;
        uint8_t** at;
    } fns[] = {
        {"_malloc_base", kCrtMalloc, &out.malloc},  {"_calloc_base", kCrtCalloc, &out.calloc},
        {"_realloc_base", kCrtRealloc, &out.realloc}, {"_free_base", kCrtFree, &out.free},
        {"_msize_base", kCrtMsize, &out.msize},
    };
    const Pattern heapLoad = Parse("48 8B 0D");
    for (Fn& fn : fns) {
        if (!(*fn.at = FindOne(img, fn.what, fn.pattern, error))) return false;
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

// --- The AI fixes ---------------------------------------------------------------------------

inline bool LocateBuildPlan(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "BuildAndUpgradeDistrictsAndBuildings", kPlanWalk, error);
    if (!p) return false;
    out.planWalk = p;
    out.ministerStrategy = static_cast<size_t>(Disp32(p + 44));
    size_t count = p[76];
    uint8_t* array = FindWithin(p, kPlanWalkSearch, "the build plan walk's array", kPlanWalkArray, error);
    if (!array) return false;
    out.planArray = array[10];
    uint8_t* end = FindWithin(p, kPlanWalkSearch, "the build plan walk's Clear call", kPlanWalkClear, error);
    if (!end) return false;
    out.planClearCall = end + 14;
    out.planClear = RelTarget(end + 15, end + 19);
    if (!MatchAt(out.planClear, Parse(kPlanClear))) {
        error = "the build plan's Clear: unexpected code";
        return false;
    }
    out.arrayCount = out.planClear[13];
    out.arrayData = out.planClear[35];
    out.nullBuildable = reinterpret_cast<uint8_t**>(RelTarget(out.planClear + 43, out.planClear + 47));
    // The check reads the same strategy and count; the array walked is the one cleared.
    if (static_cast<size_t>(Disp32(p + 69)) != out.ministerStrategy ||
        static_cast<size_t>(Disp32(array + 3)) != out.ministerStrategy || count != out.planArray + out.arrayCount ||
        array[15] != end[13] || out.arrayCount != 0x14 || out.arrayData != 8) {
        error = "the build plan walk: its reads of the plan don't agree";
        return false;
    }
    uint8_t* check = FindOne(img, "the build plan's item check", kPlanItemCheck, error);
    if (!check) return false;
    out.resolveColony = RelTarget(check + 10, check + 14);
    out.colonyCheckObject = check[20];
    out.colonyCheckSlot = check[27];
    out.hasQueued = RelTarget(check + 40, check + 44);
    if (!CallsWithin(img, p, kPlanWalkSearch, out.resolveColony)) {
        error = "the build plan walk doesn't call the ResolveColony found";
        return false;
    }
    return true;
}

inline bool LocateOpportunist(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "IsReadyForWarWith's opportunist pass", kOpportunistPass, error);
    if (!p) return false;
    out.opportunist = p;
    out.targetSlot = static_cast<size_t>(Disp32(p + 3));
    out.countryWarCount = static_cast<size_t>(Disp32(p + 10));
    out.opportunistJoin = RelTarget(p + 17, p + 21);
    out.countryPersonality = static_cast<size_t>(Disp32(p + 24));
    out.personalityOpportunist = p[30];
    out.countryId = p[56];
    out.warDb = reinterpret_cast<uint8_t**>(RelTarget(p + 66, p + 70));
    out.countryWars = static_cast<size_t>(Disp32(p + 82));
    out.warId = p[125];
    size_t attackersCount = static_cast<size_t>(Disp32(p + 143));
    size_t attackersData = static_cast<size_t>(Disp32(p + 157));
    out.memberId = p[164];
    out.memberStride = p[178];
    out.warAttackers = static_cast<size_t>(Disp32(p + 192));
    out.warDefenders = static_cast<size_t>(Disp32(p + 200));
    size_t sideCount = p[208], sideData = p[228];
    out.countryPower = static_cast<size_t>(Disp32(p + 279));
    if (RelTarget(p + 34, p + 38) != out.opportunistJoin ||
        static_cast<size_t>(Disp32(p + 41)) != out.countryWarCount || attackersData != out.warAttackers + sideData ||
        attackersCount != out.warAttackers + sideCount || p[233] != out.memberId || p[270] != out.countryId ||
        sideData != 8 || sideCount != 0x14) {
        error = "IsReadyForWarWith's opportunist pass: its offsets don't agree";
        return false;
    }
    auto globals = FindAll(p - kOpportunistGlobalsSearch, p, Parse(kOpportunistGlobals), 2);
    if (globals.size() != 1 || static_cast<size_t>(Disp32(globals[0] + 3)) != out.countryWarCount) {
        error = "IsReadyForWarWith: the first pass's globals not found";
        return false;
    }
    out.countryDb = reinterpret_cast<uint8_t**>(RelTarget(globals[0] + 10, globals[0] + 14));
    out.nullCountry = reinterpret_cast<uint8_t**>(RelTarget(globals[0] + 17, globals[0] + 21));
    out.nullWar = reinterpret_cast<uint8_t**>(RelTarget(globals[0] + 24, globals[0] + 28));
    const uint8_t* join = out.opportunistJoin - 12;
    if (!MatchAt(join, Parse(kOpportunistJoin)) || static_cast<size_t>(Disp32(join + 8)) != out.targetSlot) {
        error = "IsReadyForWarWith: the passes' join is not where expected";
        return false;
    }
    return true;
}

inline bool LocateWarDistance(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "CalcGoingToWarScoreAgainst's distance malus", kWarDistance, error);
    if (!p) return false;
    out.warDistance = p + kWarDistanceAt;
    out.malusDistance = reinterpret_cast<int32_t*>(RelTarget(p + 15, p + 19));
    out.malusSkip = p + 21 + static_cast<int8_t>(p[20]);
    out.malus = reinterpret_cast<int64_t*>(RelTarget(p + 28, p + 32));
    return true;
}

inline bool LocateTruce(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "PrepareWar's truce division", kTruceDivide, error);
    if (!p) return false;
    out.boxedInMult = reinterpret_cast<int32_t*>(RelTarget(p + 3, p + 7));
    out.truceOperand = p + 43;
    if (reinterpret_cast<int32_t*>(RelTarget(p + 43, p + 47)) != out.boxedInMult) {
        error = "PrepareWar: the truce divides by something other than the boxed-in multiplier";
        return false;
    }
    return true;
}

inline bool LocateExpansionRefresh(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "CreateStrategicData's colonize data check", kExpansionRefresh, error);
    if (!p) return false;
    // The month check's jz and the year check's jl go to the same place: the rebuild.
    if (p + 36 + static_cast<int8_t>(p[35]) != p + 62 + static_cast<int8_t>(p[61])) {
        error = "CreateStrategicData: the month and year checks jump to different places";
        return false;
    }
    out.expansionYears = p + 60;
    return true;
}

inline bool LocateAiSchedule(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* c = FindOne(img, "CAICore::Update's day counters", kAiCounters, error);
    if (!c) return false;
    out.weekCounter = static_cast<size_t>(Disp32(c + 5));
    out.monthCounter = static_cast<size_t>(Disp32(c + 39));
    out.yearCounter = static_cast<size_t>(Disp32(c + 73));
    const size_t uses[3][2] = {{11, 29}, {45, 63}, {79, 97}};
    const size_t fields[3] = {out.weekCounter, out.monthCounter, out.yearCounter};
    for (int i = 0; i < 3; ++i) {
        if (static_cast<size_t>(Disp32(c + uses[i][0])) != fields[i] ||
            static_cast<size_t>(Disp32(c + uses[i][1])) != fields[i]) {
            error = "CAICore::Update: a counter is read and reset at different offsets";
            return false;
        }
    }
    auto starts = FindAll(c - kAiUpdateSearch, c, Parse(kAiUpdatePrologue), 4);
    if (starts.size() != 1) {
        error = "CAICore::Update: prologue not found";
        return false;
    }
    out.aiUpdate = starts[0];
    uint8_t* init = FindOne(img, "CAICore::CAICore's counters", kAiCoreInit, error);
    if (!init) return false;
    out.coreId = static_cast<size_t>(Disp32(init + 2));
    out.randomPosition = static_cast<size_t>(Disp32(init + 8));
    out.randomSeed = static_cast<size_t>(Disp32(init + 14));
    if (static_cast<size_t>(Disp32(init + 21)) != out.coreId ||
        static_cast<size_t>(Disp32(init + 61)) != out.weekCounter) {
        error = "CAICore::CAICore: its fields don't agree with Update's";
        return false;
    }
    return true;
}

inline bool LocateSavedRandom(const Image& img, AiFixLocations& out, std::string& error) {
    auto hits = FindInCode(img, kSavedRandom, 3);
    if (hits.size() != 2) {
        error = "CCountryAI::ReadMember's saved random checks: not found";
        return false;
    }
    uint8_t* a = hits[0];
    uint8_t* b = hits[1];
    // One flag, one local, one exit; the two stores 4 bytes apart (position, then seed).
    if (RelTarget(a + 2, a + 6) != RelTarget(b + 2, b + 6) || a[14] != b[14] ||
        RelTarget(a + 8, a + 12) != RelTarget(b + 8, b + 12) ||
        static_cast<int8_t>(b[17]) - static_cast<int8_t>(a[17]) != 4) {
        error = "CCountryAI::ReadMember's saved random checks don't agree";
        return false;
    }
    out.useSavedRandom = RelTarget(a + 2, a + 6);
    out.savedRandomJumps[0] = a + 6;
    out.savedRandomJumps[1] = b + 6;
    return true;
}

// True if `fn` is the fleet name function: `this += k; call BuildString`.
inline bool IsFleetNameThunk(const StringLocations& str, const uint8_t* fn) {
    Pattern pat = Parse(kFleetNameThunk);
    if (!MatchAt(fn, pat)) return false;
    return RelTarget(fn + pat.size(), fn + pat.size() + 4) == str.buildString;
}

// ---- Script storage (README, "Script storage") ----

// Saving and loading, so the save store follows the save.
struct SaveLocations {
    uint8_t* startSave = nullptr;  // CSaveGameTaskManager::StartSaveToPath(manager, const CString& path, bool, bool, bool) -> 0 when started (hooked)
    uint8_t* load = nullptr;       // CGameState::Load(state, CFile*, const CString& path) (hooked)
    uint8_t* newGame = nullptr;    // CGameState::OnNewGameStarted(state) (hooked)
    uint8_t* readingSave = nullptr;  // &g_bIsReadingSaveFile, which Load sets
};

// CSaveGameTaskManager::StartSaveToPath: the prologue (15 bytes of spills taken), then
//   +41 movzx r12d,r8b  +44 mov r13,rcx  +60 lea r14,[rdx+10h] and cmp qword [r14+18h],10h: the path
//   is a CString (buffer at +0x10, capacity at +0x28)
constexpr char kStartSaveToPath[] =
    "48 89 5C 24 10 44 88 4C 24 20 44 88 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 45 0F B6 E0 4C 8B E9 45 33 FF 44 89 BD ?? ?? ?? ?? 4C 8D 72 10 49 8B D6 49 83 7E 18 10 "
    "72 03 49 8B 16";
constexpr size_t kStartSaveToPathStolenBytes = 15;
// CGameState::Load(state, CFile*, const CString& path): the prologue (15 bytes of spills taken), then
//   +39 mov r12,r8 (the path)  +56 lea rax,[g_bIsReadingSaveFile]  +67 movzx eax,byte [it]  +77 mov byte [it],1
constexpr char kGameStateLoad[] =
    "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 4D 8B E0 48 8B F2 4C 8B F9 33 D2 89 95 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 45 ?? "
    "0F B6 05 ?? ?? ?? ?? 88 45 ?? C6 05 ?? ?? ?? ?? 01";
constexpr size_t kGameStateLoadStolenBytes = 15;
// CGameState::OnNewGameStarted: the prologue (15 bytes of spills taken), then
//   +42 movzx ebx,byte [g_AllowGetPlayer]  +55 mov byte [g_AllowGetPlayer],0  +62 call
constexpr char kNewGameStarted[] =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 4C 8B F1 0F B6 1D ?? ?? ?? ?? 88 9D ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 E8";
constexpr size_t kNewGameStartedStolenBytes = 15;

constexpr char kEffectCommand[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 4C 8B F2 48 8B F9 45 33 ED 41 8B DD 89 5C 24 30 48 39 1D ?? ?? ?? ?? 75 ?? "
    "44 89 6C 24 58 4C 89 6C 24 60 48 C7 45 80 0F 00 00 00 48 C7 44 24 78 0D 00 00 00 F2 0F 10 0D";
constexpr size_t kEffectCommandSearch = 0x290;  // it calls ReadAndExecuteEffect within this many bytes
constexpr char kReadAndExecuteEffect[] =
    "48 89 5C 24 08 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 4C 8B F1";
// Inside it: +0 mov rcx,[rip+g_CurrentGameState]  +7 call CGameState::GetLocalObserved  +12 mov rsi,rax
constexpr char kCallLocalObserved[] = "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F0";
constexpr size_t kReadAndExecuteEffectSearch = 0x100;
constexpr size_t kReadAndExecuteEffectStringSearch = 0x1000;

// CLogEffect::ExecuteActual and CLogTrigger::ActualEvaluate, compiled alike but for their text:
//   +99 mov ecx,[rdi+text+0x20] (its size)  +106 lea rax,[rdi+text+0x10]
constexpr char kLogScript[] =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
    "48 8B DA 48 8B F9 45 33 FF 44 89 BD ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 8D 8D ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? 48 89 9D ?? ?? ?? ?? 44 89 BD ?? ?? ?? ?? 48 89 9D ?? ?? ?? ?? 8B 8F ?? ?? ?? ?? 48 8D 87 ?? ?? ?? ?? 48 83";
constexpr size_t kLogScriptStolenBytes = 15;

// The console's `effect` command, OnExecute_Effect, and the ReadAndExecuteEffect it calls; `gameState` is
// TickLocations::gameState. Script storage runs stored text through it.
inline bool LocateEffectCommand(const Image& img, uint8_t** gameState, uint8_t** effectCommand, uint8_t** readExecute,
                                std::string& error) {
    struct Found {
        uint8_t* effectCommand = nullptr;
        uint8_t* readExecuteEffect = nullptr;
    } out;
    // The console's `trigger` command is compiled to the same bytes up to its call, so the call tells them apart.
    Pattern runner = Parse(kReadAndExecuteEffect);
    uint8_t* run = nullptr;
    for (uint8_t* command : FindInCode(img, kEffectCommand)) {
        for (uint8_t* p = command; p < command + kEffectCommandSearch; ++p) {
            if (*p != 0xE8) continue;
            uint8_t* target = RelTarget(p + 1, p + 5);  // not always a call: the byte may be part of another instruction
            bool inCode = false;
            for (const Section& s : img.code) {
                inCode = inCode || (target >= s.begin && target + kReadAndExecuteEffectStringSearch <= s.end);
            }
            if (!inCode || !MatchAt(target, runner)) continue;
            if (run) {
                error = "OnExecute_Effect: more than one match";
                return false;
            }
            run = target;
            out.effectCommand = command;
            out.readExecuteEffect = target;
            break;
        }
    }
    if (!run) {
        error = "OnExecute_Effect: not found";
        return false;
    }
    static const char kExecuting[] = "Executing effect script.";
    bool executes = false;
    for (uint8_t* p = run; p < run + kReadAndExecuteEffectStringSearch && !executes; ++p) {
        const uint8_t* text = RelTarget(p, p + 4);
        for (const Section& s : img.data) {
            if (text >= s.begin && text + sizeof(kExecuting) <= s.end &&
                memcmp(text, kExecuting, sizeof(kExecuting) - 1) == 0) {
                executes = true;
            }
        }
    }
    if (!executes) {
        error = "ReadAndExecuteEffect does not use its message";
        return false;
    }
    if (reinterpret_cast<uint8_t**>(RelTarget(out.effectCommand + 58, out.effectCommand + 62)) != gameState) {
        error = "OnExecute_Effect checks a different game state";
        return false;
    }
    const char* noGameState = reinterpret_cast<const char*>(RelTarget(out.effectCommand + 95, out.effectCommand + 99));
    if (memcmp(noGameState, "No game state", 13) != 0) {
        error = "OnExecute_Effect's message is not the one expected";
        return false;
    }
    auto observed = FindAll(run, run + kReadAndExecuteEffectSearch, Parse(kCallLocalObserved), 1);
    if (observed.empty() || reinterpret_cast<uint8_t**>(RelTarget(observed[0] + 3, observed[0] + 7)) != gameState) {
        error = "ReadAndExecuteEffect does not get the local country as expected";
        return false;
    }
    *effectCommand = out.effectCommand;
    *readExecute = out.readExecuteEffect;
    return true;
}

// `allowGetPlayer`, if known, is the flag OnNewGameStarted must clear; null: it must clear one flag.
inline bool LocateSaves(const Image& img, const uint8_t* allowGetPlayer, SaveLocations& out, std::string& error) {
    out.startSave = FindOne(img, "CSaveGameTaskManager::StartSaveToPath", kStartSaveToPath, error);
    if (!out.startSave) return false;
    out.load = FindOne(img, "CGameState::Load", kGameStateLoad, error);
    if (!out.load) return false;
    const uint8_t* p = out.load;
    out.readingSave = RelTarget(p + 59, p + 63);
    if (RelTarget(p + 70, p + 74) != out.readingSave || RelTarget(p + 79, p + 84) != out.readingSave) {
        error = "CGameState::Load: it doesn't save and set one flag";
        return false;
    }
    out.newGame = FindOne(img, "CGameState::OnNewGameStarted", kNewGameStarted, error);
    if (!out.newGame) return false;
    p = out.newGame;
    const uint8_t* flag = RelTarget(p + 45, p + 49);
    if ((allowGetPlayer && flag != allowGetPlayer) || RelTarget(p + 57, p + 62) != flag) {
        error = "CGameState::OnNewGameStarted: it doesn't clear g_AllowGetPlayer";
        return false;
    }
    return true;
}

// Script variables, as set_variable, is_variable_set and check_variable reach them.
struct VariableLocations {
    uint8_t* access = nullptr;  // AccessVariablePointer(CEventScope&, const CString& name) -> CVariables*, as set_variable
    uint8_t* get = nullptr;     // GetVariablePointer(const CEventScope&, const CString& name) -> const CVariables*
    uint8_t* find = nullptr;    // the variables' map: find(map, SMapEntry** out, const CString& key)
    uint8_t* set = nullptr;     // CVariables::SetVariable(variables, const CString& name, CFixedPoint)
    size_t map = 0;             // CVariables: the map
    size_t entryEmpty = 0;      // SMapEntry: byte, 0xFF when nothing is there
    size_t entryValue = 0;      // SMapEntry: CFixedPoint value
    size_t effectName = 0;      // CSetVariableEffect, CClearVariableEffect: CString name, right after the CEffect base
    size_t triggerName = 0;     // CIsVariableSetTrigger: CString name, right after the CTrigger base
};

// CSetVariableEffect::ExecuteActual: the value, then
//   +46 lea rdx,[rdi+name]  +56 call AccessVariablePointer  +69 lea rdx,[rdi+name]  +79 call CVariables::SetVariable
constexpr char kSetVariableEffect[] =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 4C 8B C2 48 81 C1 ?? ?? ?? ?? 48 8D 54 24 30 "
    "E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8D 97 ?? ?? ?? ?? 48 8B CE E8 ?? ?? ?? ?? 48 85 C0 74 12 4C 8B C3 48 8D 97 "
    "?? ?? ?? ?? 48 8B C8 E8";
// CClearVariableEffect::ExecuteActual:
//   +13 lea rbx,[rcx+name]  +26 call AccessVariablePointer  +39 lea rcx,[rax+map]  +51 call find
//   +61 cmp byte [rcx+empty],0FFh  +73 call CVariables::RemoveVariable
constexpr char kClearVariableEffect[] =
    "48 89 5C 24 10 57 48 83 EC 20 48 8B C2 48 8D 99 ?? ?? ?? ?? 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 "
    "74 27 48 8D 48 ?? 4C 8B C3 48 8D 54 24 30 E8 ?? ?? ?? ?? 48 8B 4C 24 30 80 79 ?? FF 74 0B 48 8B D3 48 8B CF E8";
// CIsVariableSetTrigger::ActualEvaluate:
//   +9 lea rbx,[rcx+name]  +22 call GetVariablePointer  +38 lea rcx,[rax+map]  +50 call find  +60 cmp byte [rax+empty],0FFh
constexpr char kIsVariableSetTrigger[] =
    "40 53 48 83 EC 20 48 8B C2 48 8D 99 ?? ?? ?? ?? 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 75 06 48 83 C4 20 5B C3 "
    "48 8D 48 ?? 4C 8B C3 48 8D 54 24 30 E8 ?? ?? ?? ?? 48 8B 44 24 30 80 78 ?? FF 0F 95 C0";
// CCheckVariableTrigger::ActualEvaluate: +31 call GetVariablePointer  +86 lea rcx,[rbp+map]  +101 call find
//   +111 cmp byte [rcx+empty],0FFh  +121 mov rcx,[rcx+value]
constexpr char kCheckVariableTrigger[] =
    "48 89 5C 24 20 55 56 57 48 83 EC 20 48 8B FA 48 8D B1 ?? ?? ?? ?? 48 8B D9 48 8B D6 48 8B CF E8 ?? ?? ?? ?? "
    "48 8B E8 48 85 C0 75 0D 48 8B 5C 24 58 48 83 C4 20 5F 5E 5D C3 4C 89 74 24 48 48 8D 8B ?? ?? ?? ?? 4C 8B C7 "
    "48 8D 54 24 50 4C 8D 73 ?? E8 ?? ?? ?? ?? 48 8D 4D ?? 4C 8B C6 48 8D 54 24 40 48 8B D8 E8 ?? ?? ?? ?? "
    "48 8B 4C 24 40 80 79 ?? FF 75 04 33 C9 EB 04 48 8B 49 ??";

inline bool LocateVariables(const Image& img, VariableLocations& out, std::string& error) {
    const uint8_t* p = FindOne(img, "CSetVariableEffect::ExecuteActual", kSetVariableEffect, error);
    if (!p) return false;
    out.effectName = static_cast<size_t>(Disp32(p + 49));
    out.access = RelTarget(p + 57, p + 61);
    out.set = RelTarget(p + 80, p + 84);
    if (static_cast<size_t>(Disp32(p + 72)) != out.effectName) {
        error = "CSetVariableEffect: two different names";
        return false;
    }
    if (!(p = FindOne(img, "CClearVariableEffect::ExecuteActual", kClearVariableEffect, error))) return false;
    out.map = p[42];
    out.find = RelTarget(p + 52, p + 56);
    out.entryEmpty = p[63];
    if (static_cast<size_t>(Disp32(p + 16)) != out.effectName || RelTarget(p + 27, p + 31) != out.access) {
        error = "CClearVariableEffect: a different name or variables than set_variable's";
        return false;
    }
    if (!(p = FindOne(img, "CIsVariableSetTrigger::ActualEvaluate", kIsVariableSetTrigger, error))) return false;
    out.triggerName = static_cast<size_t>(Disp32(p + 12));
    out.get = RelTarget(p + 23, p + 27);
    if (p[41] != out.map || RelTarget(p + 51, p + 55) != out.find || p[62] != out.entryEmpty) {
        error = "CIsVariableSetTrigger: a different map than clear_variable's";
        return false;
    }
    if (!(p = FindOne(img, "CCheckVariableTrigger::ActualEvaluate", kCheckVariableTrigger, error))) return false;
    out.entryValue = p[124];
    if (RelTarget(p + 32, p + 36) != out.get || p[89] != out.map || RelTarget(p + 102, p + 106) != out.find ||
        p[113] != out.entryEmpty || out.entryValue % 8 != 0) {
        error = "CCheckVariableTrigger: a different map than is_variable_set's";
        return false;
    }
    return true;
}

// CLogEffect::ExecuteActual and CLogTrigger::ActualEvaluate, and the call that builds the text they log.
struct LogScriptLocations {
    uint8_t* logEffect = nullptr;      // CLogEffect::ExecuteActual(effect, scope) (hooked)
    uint8_t* logTrigger = nullptr;     // CLogTrigger::ActualEvaluate(trigger, scope) (hooked)
    size_t logEffectText = 0;          // CLogEffect: CString text
    size_t logTriggerText = 0;         // CLogTrigger: CString text
    uint8_t* textCall = nullptr;       // in the log effect: call CTextBase::ProcessString (redirected while recording)
    uint8_t* processString = nullptr;  // CTextBase::ProcessString(text, CString* out, SStringView* in, bool, ...)
};

// In both, after the text's view is built: +24 call ProcessString(gameText, &out, &view, false, 0, 0).
constexpr char kLogTextCall[] = "4C 89 7C 24 20 45 33 C9 4C 8D 44 24 60 48 8D 55 30 48 8D 8D ?? ?? ?? ?? E8";
constexpr size_t kLogTextCallAt = 0x8B;

// The two log functions. `effectText` is where the log effect's text must be (the CEffect base's size),
// `triggerText` the trigger's, or 0 for whichever of the two isn't the effect.
inline bool LocateLogScript(const Image& img, size_t effectText, size_t triggerText, LogScriptLocations& out,
                            std::string& error) {
    auto logs = FindInCode(img, kLogScript, 4);
    if (logs.size() != 2) {
        error = "CLogEffect and CLogTrigger: " + std::to_string(logs.size()) + " matches";
        return false;
    }
    Pattern call = Parse(kLogTextCall);
    for (uint8_t* hit : logs) {
        size_t buf = static_cast<size_t>(Disp32(hit + 106)), size = static_cast<size_t>(Disp32(hit + 99));
        if (size != buf + offsetof(game::String, size) - offsetof(game::String, buf) || buf < offsetof(game::String, buf)) {
            error = "CLogEffect or CLogTrigger reads its text unexpectedly";
            return false;
        }
        size_t text = buf - offsetof(game::String, buf);
        uint8_t* at = hit + kLogTextCallAt;
        if (!MatchAt(at, call)) {
            error = "CLogEffect or CLogTrigger: the call that builds the text is not where expected";
            return false;
        }
        uint8_t* process = RelTarget(at + 25, at + 29);
        if (out.processString && process != out.processString) {
            error = "CLogEffect and CLogTrigger build their text differently";
            return false;
        }
        out.processString = process;
        if (text == effectText) {
            out.logEffect = hit;
            out.logEffectText = text;
            out.textCall = at + 24;
        } else if (triggerText == 0 || text == triggerText) {
            out.logTrigger = hit;
            out.logTriggerText = text;
        }
    }
    if (!out.logEffect || !out.logTrigger) {
        error = "CLogEffect and CLogTrigger: their texts are not right after the CEffect and CTrigger bases";
        return false;
    }
    return true;
}

// What the is_multiplayer trigger reads (CIsMultiplayerTrigger::ActualEvaluate): multiplayer when the object at
// `*session` exists and its byte at `flag` is set.
struct MultiplayerLocations {
    uint8_t** session = nullptr;
    size_t flag = 0;
};
//   +0 mov rax,[rip+session]  +7 test rax,rax  +12 cmp byte [rax+flag],0
constexpr char kIsMultiplayerTrigger[] =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 1B 80 B8 ?? ?? ?? ?? 00 74 12 0F B6 81 ?? ?? ?? ?? BA 01 00 00 00 3B D0 0F 94 C0 C3";

inline bool LocateMultiplayer(const Image& img, MultiplayerLocations& out, std::string& error) {
    const uint8_t* p = FindOne(img, "CIsMultiplayerTrigger::ActualEvaluate", kIsMultiplayerTrigger, error);
    if (!p) return false;
    out.session = reinterpret_cast<uint8_t**>(RelTarget(p + 3, p + 7));
    out.flag = static_cast<size_t>(Disp32(p + 14));
    return true;
}

}  // namespace locate
