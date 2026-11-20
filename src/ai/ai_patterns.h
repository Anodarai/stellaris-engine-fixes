// Locations for the AI's income target shortcut and the [ai_fixes] settings (aifixes_runtime.inl;
// re/ai.md).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "../core/locate.h"

namespace locate {

// --- The income target loop (fast_income_target) --------------------------------------------

// CAIEconomicPlan::CalculateIncomeTarget's scaling loop (re/ai.md section 6). Each round adds the
// income of the plan's scaling subplans to the total until some resource's total passes the
// target; there is no limit on the rounds, and a large empire can need a billion of them.
struct IncomeTargetLocations {
    uint8_t* loop = nullptr;        // the round's start: the target check (rbx total, r12 target, r13d rounds)
    uint8_t* scalingCall = nullptr; // call CalcScalingSubplans, the first thing in each round (redirected)
    uint8_t* calcScaling = nullptr; // CAIEconomicPlan::CalcScalingSubplans(plan, out list, scope)
};

// CalculateIncomeTarget's round: for each resource i, leave if (target >= 0 || total > 0) &&
// target < total (r9 the target's table from [r12], r10 the total's from [rbx]); leave if the
// table is empty or the last round added nothing (sil); else sil = 0 and call CalcScalingSubplans
// (+100). The three exits (+57, +73, +82) go to one place; the round ends with inc r13d / jmp here.
constexpr char kIncomeTargetLoop[] =
    "33 C9 4D 8B 0C 24 45 8B 41 0C 45 85 C0 7E ?? 4D 8B 09 48 8B 03 4C 8B 10 0F 1F 84 00 00 00 00 00 "
    "48 63 C1 49 8B 14 C1 48 85 D2 79 ?? 49 83 3C C2 00 7E ?? 49 3B 14 C2 0F 8C ?? ?? ?? ?? FF C1 41 3B C8 "
    "7C ?? 45 85 C0 0F 84 ?? ?? ?? ?? 40 84 F6 0F 84 ?? ?? ?? ?? 40 32 F6 4D 8B C7 48 8D 55 ?? 48 8B 4D ?? E8";

constexpr size_t kIncomeTargetCall = 100;

constexpr char kIncomeTargetRoundEnd[] = "41 FF C5 E9"; // inc r13d / jmp the loop

constexpr size_t kIncomeTargetRoundSearch = 0x200;

// Fills `out` with the CRT heap functions; false and `error` if any is missing or they disagree.
inline bool LocateIncomeTarget(const Image& img, IncomeTargetLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "CalculateIncomeTarget's scaling loop", kIncomeTargetLoop, error);
    if (!p)
        return false;
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

// --- The AI fixes (AiFixLocations) ----------------------------------------------------------

// The country AI's gameplay fixes, [ai_fixes] in stellaris-engine-fixes.ini (re/ai.md, sections 2 and 4).
// Each part is located on its own; a part that isn't found leaves only its fix off.
struct AiFixLocations {
    // 2.1 The build plan. CAIInteriorMinister::BuildAndUpgradeDistrictsAndBuildings walks the plan
    // CAIEconomicStrategy::CreateBuildPlan makes once a month, stops at the first item it can't
    // place, then clears the whole plan.
    uint8_t* planWalk = nullptr; // BuildAndUpgradeDistrictsAndBuildings(minister) (hooked)
    size_t ministerStrategy = 0; // CAIInteriorMinister: CAIEconomicStrategy*
    size_t planArray = 0;        // CAIEconomicStrategy: the plan, a CPdxArray of {CBuildableBase*, CFixedPoint}
    size_t arrayData = 0, arrayCount = 0; // CPdxArray: data pointer, int count
    uint8_t* planClearCall = nullptr;     // the walk's `call Clear` at its end (redirected)
    uint8_t* planClear = nullptr;         // the plan's Clear(array): deletes every item, count = 0
    uint8_t** nullBuildable = nullptr;    // &TPdxNullObject<CNullBuildable>::_pInstance, which Clear doesn't delete
    uint8_t* resolveColony = nullptr;     // NBuildPlan::ResolveColony(buildable) -> CColony*
    size_t colonyCheckObject = 0;         // CColony: sub-object whose vtable slot says the colony is usable
    size_t colonyCheckSlot = 0;           // that slot's byte offset
    uint8_t* hasQueued = nullptr;         // CColony::HasQueuedBuildingOrDistrictConstruction(colony)
    // 2.16 The walk's `jz` taken when an item fails its check (to the Clear call: the walk stops),
    // and the loop's continue, which reloads the registers the check call clobbered.
    uint8_t* planStopJump = nullptr;
    uint8_t* planContinue = nullptr;

    // 2.17 CAIInteriorMinister::BuildColonizer's count of idle colony ships: `mov eax,[rdi+ships];
    // add r15d,eax` (9 bytes, replaced), rdi the fleet. Volatile registers are dead after it.
    uint8_t* colonizerIdle = nullptr;
    size_t colonizerShipCount = 0; // CFleet: int ship count, as read there

    // 2.18 CAIInteriorMinister::BuildStation(minister, fleet, ...) (hooked), and the two loads of
    // NDefines::NAI::CONSTRUCTION_SHIP_DISTANCE_PENALTY (7 bytes each, replaced): into r10 in
    // FindPlanetForStarbase, into r8 in FindDepositHolderForOrbitalStation.
    uint8_t* buildStation = nullptr;
    uint8_t* penaltyLoadOutpost = nullptr;
    uint8_t* penaltyLoadStation = nullptr;
    int64_t* stationPenalty = nullptr; // the define, CFixedPoint

    // 2.15 CAIInteriorMinister::MonthlyUpdate: `jne` over the clearing of the survey blacklist,
    // taken unless the month is country id % 12.
    uint8_t* blacklistJump = nullptr;

    // 2.3 The opportunist pass of CCountryAI::IsReadyForWarWith (replaced from its first byte to
    // `join`). Registers there: r14 our country, [rbp+targetSlot] the target, rsi the first pass's
    // sum, rdi the second's (0); after it r9, r10 and r11 hold what the game left in them.
    uint8_t* opportunist = nullptr;
    uint8_t* opportunistJoin = nullptr;
    size_t targetSlot = 0;
    size_t countryWars = 0, countryWarCount = 0; // CCountry: uint32 war ids, int count
    size_t countryPersonality = 0;               // CCountry: personality
    size_t personalityOpportunist = 0;           // personality: bool opportunist
    size_t countryId = 0;                        // CCountry: uint32 id
    size_t countryPower = 0;                     // CCountry: int64 fleet power; half of it is added
    size_t warId = 0;                            // CWar: uint32 id
    size_t warAttackers = 0, warDefenders = 0;   // CWar: CPdxArray of participants (data +8, count +0x14)
    size_t memberStride = 0, memberId = 0;       // participant: uint32 country id at +memberId
    uint8_t** countryDb = nullptr;
    uint8_t** nullCountry = nullptr;
    uint8_t** warDb = nullptr;
    uint8_t** nullWar = nullptr;

    // 2.2 CAIForeignMinister::CalcGoingToWarScoreAgainst's distance malus:
    // cmp ebp,[DISTANCE]; jle; movsxd rax,ebp; imul rax,[MALUS] (kWarDistanceBytes, rewritten).
    uint8_t* warDistance = nullptr;
    int32_t* malusDistance = nullptr; // &NDefines::NAI::WAR_DECLARATION_MALUS_DISTANCE
    int64_t* malus = nullptr;         // &NDefines::NAI::WAR_DECLARATION_MALUS, CFixedPoint
    uint8_t* malusSkip = nullptr;     // where the jle goes

    // 2.4 CAIForeignMinister::PrepareWar's `idiv dword [AI_AGGRESSIVENESS_BOXED_IN_MULT]` when
    // there is a truce; truceOperand is its rel32 (redirected).
    uint8_t* truceOperand = nullptr;
    int32_t* boxedInMult = nullptr;

    // 2.5 CCountryAI::CreateStrategicData: the `jl` taken while fewer than AI_BUDGETING_START_YEARS
    // have passed, which rebuilds the colonize data every month (made unconditional).
    uint8_t* expansionYears = nullptr;

    // 2.9 CAICore::Update's day counters, and the AI's own random generator.
    uint8_t* aiUpdate = nullptr;               // CAICore::Update(core) (hooked)
    size_t coreId = 0;                         // CAICore: int id (the country's), set by the constructor
    size_t randomPosition = 0, randomSeed = 0; // CAICore: the unsynced generator
    size_t weekCounter = 0, monthCounter = 0, yearCounter = 0; // CAICore: int days left
    uint8_t* savedRandomJumps[2] = {}; // CCountryAI::ReadMember: `jz` past storing random_count / random_seed
    uint8_t* useSavedRandom = nullptr; // &g_bUseSavedRandom (-loadrandomseed), which those test
};

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

// In the walk: the item check's call (+7) and the `jz` taken when it fails (+14, target +16).
constexpr char kPlanWalkCheckCall[] = "49 8B D7 48 8D 4D ?? E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 49 8B 0F E8";

// In the walk: the loop's continue, rcx = [rsp+X] (+4), rdx reloaded, then the index is incremented.
constexpr char kPlanWalkContinue[] = "48 8B 4C 24 ?? 48 8B 95 ?? ?? ?? ?? 41 FF C4";

// BuildColonizer: the end of a colony fleet's order walk (an order type compared, the next order
// taken, its vtable asked), then the idle fleet's ships added: mov eax,[rdi+ships] (+30, the
// offset +32), add r15d,eax (+36), and the code after them (+39). The game also jumps to the add
// (eax = another fleet's count), so only the 6-byte mov is replaced.
constexpr char kColonizerIdle[] =
    "81 7B 18 ?? ?? 00 00 0F 84 ?? ?? ?? ?? 48 8B 5B 08 48 8B 03 48 8B CB FF 50 38 84 C0 75 ?? 8B 87 ?? ?? ?? ?? "
    "44 03 F8 4C 8B 05";

constexpr size_t kColonizerIdleBytes = 6;

// MonthlyUpdate: the month read, country id % 12 (by multiplication), r14d = 0, then
// cmp ecx,r8d; jne +4 (+34); mov [rsi+count],r14d.
constexpr char kSurveyBlacklistClear[] =
    "44 8B 83 ?? ?? ?? ?? 8B 4D ?? B8 AB AA AA AA F7 E1 C1 EA 03 8D 04 52 C1 E0 02 2B C8 45 33 F6 41 3B C8 75 04 "
    "44 89 76 ??";

// BuildStation: its prologue (15 bytes taken), the arguments kept, the first call.
constexpr char kBuildStation[] =
    "4C 89 44 24 18 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8D 6C 24 ?? 48 89 9D "
    "?? ?? ?? ?? 41 0F B6 F1 49 8B F8 4C 8B EA 48 8B D9 E8";

constexpr size_t kBuildStationStolenBytes = 15;

constexpr size_t kBuildStationSearch = 0x180; // its calls of the two searches are within this

constexpr size_t kStationSearchSize = 0x1000; // a search's penalty load is this close to its start

// FindPlanetForStarbase: jumps * 1.0 (fixed point), then the penalty into r10 (+27).
constexpr char kPenaltyLoadOutpost[] =
    "48 63 C8 48 69 D1 A0 86 01 00 48 8B 85 ?? ?? ?? ?? 48 63 00 48 69 C8 A0 86 01 00 4C 8B 15 ?? ?? ?? ?? 41 BB "
    "33 F3 04 B5";

// FindDepositHolderForOrbitalStation: jumps * -1.0, then the penalty into r8 (+10).
constexpr char kPenaltyLoadStation[] = "48 63 C8 48 69 D1 60 79 FE FF 4C 8B 05 ?? ?? ?? ?? B9 33 F3 04 B5";

constexpr size_t kPenaltyLoadBytes = 7;

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

constexpr size_t kOpportunistGlobalsSearch = 0x200; // bytes before the pass

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

inline bool LocateBuildPlan(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "BuildAndUpgradeDistrictsAndBuildings", kPlanWalk, error);
    if (!p)
        return false;
    out.planWalk = p;
    out.ministerStrategy = static_cast<size_t>(Disp32(p + 44));
    size_t count = p[76];
    uint8_t* array = FindWithin(p, kPlanWalkSearch, "the build plan walk's array", kPlanWalkArray, error);
    if (!array)
        return false;
    out.planArray = array[10];
    uint8_t* end = FindWithin(p, kPlanWalkSearch, "the build plan walk's Clear call", kPlanWalkClear, error);
    if (!end)
        return false;
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
    if (!check)
        return false;
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

// 2.16; needs LocateBuildPlan's results.
inline bool LocatePlanSkip(const Image& img, AiFixLocations& out, std::string& error) {
    if (!out.planWalk && !LocateBuildPlan(img, out, error))
        return false;
    uint8_t* call =
        FindWithin(out.planWalk, kPlanWalkSearch, "the build plan walk's item check call", kPlanWalkCheckCall, error);
    if (!call)
        return false;
    uint8_t* cont =
        FindWithin(out.planWalk, kPlanWalkSearch, "the build plan walk's continue", kPlanWalkContinue, error);
    if (!cont)
        return false;
    // The call is of the function LocateBuildPlan's check pattern lies in; a failed check goes to
    // the `mov rcx,[rsp+X]` before the Clear call; the continue reloads that same slot.
    std::string ignored;
    uint8_t* check = FindOne(img, "the build plan's item check", kPlanItemCheck, ignored);
    uint8_t* target = RelTarget(call + 8, call + 12);
    if (!check || target > check || check - target > 0x80 || RelTarget(call + 16, call + 20) != out.planClearCall - 5 ||
        cont[4] != out.planClearCall[-1] || cont < call) {
        error = "the build plan walk: its item check and loop don't agree";
        return false;
    }
    out.planStopJump = call + 14;
    out.planContinue = cont;
    return true;
}

inline bool LocateColonizerIdle(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "BuildColonizer's idle ship count", kColonizerIdle, error);
    if (!p)
        return false;
    out.colonizerIdle = p + 30;
    out.colonizerShipCount = static_cast<size_t>(Disp32(p + 32));
    return true;
}

inline bool LocateStationRange(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "BuildStation", kBuildStation, error);
    if (!p)
        return false;
    uint8_t* outpost = FindOne(img, "FindPlanetForStarbase's distance penalty", kPenaltyLoadOutpost, error);
    if (!outpost)
        return false;
    uint8_t* station =
        FindOne(img, "FindDepositHolderForOrbitalStation's distance penalty", kPenaltyLoadStation, error);
    if (!station)
        return false;
    outpost += 27;
    station += 10;
    uint8_t* define = RelTarget(outpost + 3, outpost + 7);
    if (define != RelTarget(station + 3, station + 7)) {
        error = "the station searches read different distance penalties";
        return false;
    }
    // BuildStation calls both searches: a call that lands shortly before each load.
    bool callsOutpost = false, callsStation = false;
    for (uint8_t* c = p; c < p + kBuildStationSearch; ++c) {
        if (*c != 0xE8)
            continue;
        uint8_t* t = RelTarget(c + 1, c + 5);
        if (t < outpost && outpost - t < static_cast<ptrdiff_t>(kStationSearchSize))
            callsOutpost = true;
        if (t < station && station - t < static_cast<ptrdiff_t>(kStationSearchSize))
            callsStation = true;
    }
    if (!callsOutpost || !callsStation) {
        error = "BuildStation doesn't call the searches found";
        return false;
    }
    out.buildStation = p;
    out.penaltyLoadOutpost = outpost;
    out.penaltyLoadStation = station;
    out.stationPenalty = reinterpret_cast<int64_t*>(define);
    return true;
}

inline bool LocateSurveyBlacklist(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "MonthlyUpdate's survey blacklist clear", kSurveyBlacklistClear, error);
    if (!p)
        return false;
    out.blacklistJump = p + 34;
    return true;
}

inline bool LocateOpportunist(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "IsReadyForWarWith's opportunist pass", kOpportunistPass, error);
    if (!p)
        return false;
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
    if (!p)
        return false;
    out.warDistance = p + kWarDistanceAt;
    out.malusDistance = reinterpret_cast<int32_t*>(RelTarget(p + 15, p + 19));
    out.malusSkip = p + 21 + static_cast<int8_t>(p[20]);
    out.malus = reinterpret_cast<int64_t*>(RelTarget(p + 28, p + 32));
    return true;
}

inline bool LocateTruce(const Image& img, AiFixLocations& out, std::string& error) {
    uint8_t* p = FindOne(img, "PrepareWar's truce division", kTruceDivide, error);
    if (!p)
        return false;
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
    if (!p)
        return false;
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
    if (!c)
        return false;
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
    if (!init)
        return false;
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

} // namespace locate
