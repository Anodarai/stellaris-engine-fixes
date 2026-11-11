// Runs locate.h against stellaris.exe mapped as an image (no code runs) and checks the results
// against what Ghidra found for the build analysed in re/fleet_outliner_sort.md.
// Usage: locate_test.exe <path to stellaris.exe> [--expect-known]
//   --expect-known  also compare with the addresses of SHA-256 400DF27C...7A83
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../src/aibossthreat.h"
#include "../src/aifleettrace.h"
#include "../src/aiwarguard.h"
#include "../src/locate.h"

static bool TestBossThreatStub(aibossthreat::Locations found) {
    constexpr uint8_t setup[] = {
        0x53, 0x41, 0x56,  // push rbx; push r14
        0x48, 0x8B, 0xD9,  // mov rbx, rcx
        0x48, 0x8B, 0xC2,  // mov rax, rdx
        0x4D, 0x8B, 0xF0,  // mov r14, r8
    };
    constexpr uint8_t finish[] = {0x41, 0x5E, 0x5B, 0xC3};  // pop r14; pop rbx; ret
    auto buffer = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT,
                                                      PAGE_EXECUTE_READWRITE));
    if (!buffer) return false;
    auto end = buffer + 64 - sizeof(finish);
    found.resume = end;
    const auto code = aibossthreat::ThreatStub(found);
    memcpy(buffer, setup, sizeof(setup));
    memcpy(buffer + sizeof(setup), code.data(), code.size());
    memcpy(end, finish, sizeof(finish));
    FlushInstructionCache(GetCurrentProcess(), buffer, 64);

    using Estimate = void (*)(const int64_t*, int64_t, int64_t*);
    auto estimate = reinterpret_cast<Estimate>(buffer);
    std::vector<int64_t> fleet(found.cachedPower / sizeof(int64_t) + 1);
    constexpr int64_t floor = 40000LL << 15;
    constexpr int64_t avatar = 119032LL << 15;
    const struct { int64_t cached; int64_t minimum; int64_t expected; } cases[] = {
        {0, floor, floor},
        {-1, floor, floor},
        {floor, floor, floor},
        {avatar, floor, avatar},
        {avatar, INT64_MAX, INT64_MAX},
        {INT64_MAX, floor, INT64_MAX},
    };
    bool ok = true;
    for (const auto& test : cases) {
        fleet.back() = test.cached;
        int64_t result = -1;
        estimate(fleet.data(), test.minimum, &result);
        if (result != test.expected) ok = false;
    }
    VirtualFree(buffer, 0, MEM_RELEASE);
    return ok;
}

static bool TestWarRefreshStub(const aiwarguard::Locations& war) {
    // Match the native flag write's inputs: RDI points to the AI, EAX is the negated bool.
    constexpr uint8_t setup[] = {0x57, 0x48, 0x8B, 0xF9, 0x8B, 0xC2};  // push rdi; mov rdi, rcx; mov eax, edx
    constexpr uint8_t finish[] = {0x5F, 0xC3};                          // pop rdi; ret
    auto buffer = static_cast<uint8_t*>(VirtualAlloc(nullptr, 128, MEM_RESERVE | MEM_COMMIT,
                                                      PAGE_EXECUTE_READWRITE));
    if (!buffer) return false;
    auto end = buffer + 126;
    auto code = aiwarguard::RefreshStub(war, end);
    memcpy(buffer, setup, sizeof(setup));
    memcpy(buffer + sizeof(setup), code.data(), code.size());
    memcpy(end, finish, sizeof(finish));
    FlushInstructionCache(GetCurrentProcess(), buffer, 128);

    using SetPrepareWar = void (*)(uint32_t*, int);
    auto set = reinterpret_cast<SetPrepareWar>(buffer);
    std::vector<uint32_t> ai(war.aiFlags / sizeof(uint32_t) + 1);
    auto& flags = ai.back();
    constexpr uint32_t other = 0x400;
    constexpr uint32_t preparing = aiwarguard::kPreparing;
    constexpr uint32_t dirty = aiwarguard::kNeedStrategicWarData;
    const struct { uint32_t before; int prepare; uint32_t after; } cases[] = {
        {other, 0, other},
        {other, -1, other | preparing},
        {other | preparing, -1, other | preparing},
        {other | preparing, 0, other | dirty},
        {other | preparing | dirty, 0, other | dirty},
        {other | dirty, -1, other | preparing | dirty},
    };
    bool ok = true;
    for (const auto& test : cases) {
        flags = test.before;
        set(ai.data(), test.prepare);
        if (flags != test.after) ok = false;
    }
    VirtualFree(buffer, 0, MEM_RELEASE);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: locate_test <stellaris.exe> [--expect-known]\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep what was printed if a lookup crashes
    bool expectKnown = argc > 2 && strcmp(argv[2], "--expect-known") == 0;

    HMODULE mod = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod) {
        printf("FAIL: cannot map %s (error %lu)\n", argv[1], GetLastError());
        return 1;
    }
    auto base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod) & ~uintptr_t(3));
    locate::Image img = locate::Describe(base);
    locate::StringLocations str;
    locate::Locations loc;
    std::string error;
    ULONGLONG t0 = GetTickCount64();
    bool ok = locate::LocateStrings(img, str, error) && locate::Locate(img, loc, error);
    ULONGLONG ms = GetTickCount64() - t0;
    if (!ok) {
        printf("FAIL: %s\n", error.c_str());
        return 1;
    }
    printf("located everything in %llu ms\n", ms);

    auto rva = [&](const void* p) { return static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - base); };
    int failures = 0;
    auto expect = [&](const char* what, unsigned long long got, unsigned long long want) {
        bool good = !expectKnown || got == want;
        printf("  %-24s 0x%llX%s\n", what, got, good ? "" : "  MISMATCH");
        if (!good) {
            printf("    want 0x%llX\n", want);
            ++failures;
        }
    };
    expect("compare", rva(loc.compare), 0x14b3270);
    expect("fleetGroupUpdate", rva(loc.fleetGroupUpdate), 0x14b34c0);
    expect("fleetGroupVtblSlot", rva(loc.fleetGroupVtblSlot), 0x2455490);
    expect("fleetDb", rva(loc.fleetDb), 0x31129d8);
    expect("shipDb", rva(loc.shipDb), 0x3114c58);
    expect("buildString", rva(str.buildString), 0x33b960);
    expect("gameFree", rva(str.gameFree), 0x20218a4);
    expect("fleetIdOffset", loc.fleetIdOffset, 0x30);
    expect("shipIdOffset", loc.shipIdOffset, 0x18);
    expect("fleetShipsOffset", loc.fleetShipsOffset, 0x320);
    expect("fleetShipCountOffset", loc.fleetShipCountOffset, 0x32c);
    expect("fleetNameSubobject", loc.fleetNameSubobject, 0x50);
    expect("fleetNameVtblOffset", loc.fleetNameVtblOffset, 0x30);
    expect("shipDateOffset", loc.shipDateOffset, 0xf98);

    // The name function the fleet's vtable slot points to (CFleet::GetLocalizedName).
    if (expectKnown) {
        bool thunk = locate::IsFleetNameThunk(str, base + 0x1af370);
        printf("  fleet name thunk at 0x1AF370 %s\n", thunk ? "recognized" : "NOT recognized");
        if (!thunk) ++failures;
    }

    locate::MegaLocations mega;
    if (!locate::LocateMega(img, mega, error)) {
        printf("FAIL megastructures: %s\n", error.c_str());
        return 1;
    }
    printf("located megastructures\n");
    expect("mega groupUpdate", rva(mega.groupUpdate), 0x14bf270);
    expect("mega groupVtblSlot", rva(mega.groupVtblSlot), 0x2452930);
    expect("mega entryUpdate", rva(mega.entryUpdate), 0x14bea10);
    expect("mega typeGetName", rva(mega.typeGetName), 0x3b25e0);
    expect("megaDb", rva(mega.megaDb), 0x3114c68);
    expect("megaIdOffset", mega.megaIdOffset, 0x8);
    expect("megaTypeOffset", mega.megaTypeOffset, 0x178);
    expect("megaCustomNameFlag", mega.megaCustomNameFlag, 0x148);
    expect("megaCustomName", mega.megaCustomName, 0x120);
    expect("typeIdOffset", mega.typeIdOffset, 0x8);

    locate::TickLocations bench;
    if (!locate::LocateTicks(img, bench, error)) {
        printf("FAIL benchmark: %s\n", error.c_str());
        return 1;
    }
    printf("located the benchmark's functions\n");
    expect("console Execute", rva(bench.execute), 0x1b43650);
    expect("consoleLockedFlag", bench.consoleLockedFlag, 0xa9);
    expect("RunCommandNow", rva(bench.runCommand), 0x1b13120);
    expect("g_bAddToHistory", rva(bench.addToHistory), 0x2813d72);
    expect("CConsole::_pInstance", rva(bench.console), 0x3159310);
    expect("CheckTick", rva(bench.checkTick), 0x934e20);
    expect("idlerSpeed", bench.idlerSpeed, 0x590);
    expect("idlerPaused", bench.idlerPaused, 0x594);
    expect("gGameSpeedTurnTimes", rva(bench.speedTurnTimes), 0x23a8868);
    expect("HandleTurnTick", rva(bench.turnTick), 0x251800);
    expect("stateTick", bench.stateTick, 0x8);
    expect("stateDate", bench.stateDate, 0xc0);
    expect("RenderFrame", rva(bench.renderFrame), 0x24bdd0);
    expect("g_bFrameSmoothing", rva(bench.frameSmoothing), 0x2807830);
    expect("NUM_COMMANDS_PER_TURN", rva(bench.commandsPerTurn), 0x31157d8);
    expect("g_nTicksPerTurn", rva(bench.ticksPerTurn), 0x2807834);
    expect("g_CurrentGameState", rva(bench.gameState), 0x3114700);
    expect("idlerMultiplayer", bench.idlerMultiplayer, 0x180);

    locate::SaveLocations saves;
    if (!locate::LocateSaves(img, saves, error)) {
        printf("FAIL saves: %s\n", error.c_str());
        return 1;
    }
    printf("located saving and loading\n");
    expect("StartSaveToPath", rva(saves.startSave), 0xa890d0);
    expect("CGameState::Load", rva(saves.load), 0x264110);
    expect("OnNewGameStarted", rva(saves.newGame), 0x2710d0);
    expect("g_bIsReadingSaveFile", rva(saves.readingSave), 0x3114e21);
    locate::SaveWriteLocations saveWrite;
    if (!locate::LocateSaveWrite(img, saves, saveWrite, error)) {
        printf("FAIL save write: %s\n", error.c_str());
        return 1;
    }
    printf("located save completion\n");
    expect("ZipAndWrite", rva(saveWrite.zipAndWrite), 0xa8a1f0);
    expect("save parameters", saveWrite.paramsOffset, 0x70);
    expect("archive destructor", rva(saveWrite.destroyArchive), 0x1b3af20);
    expect("PHYSFS_close", rva(saveWrite.closeFile), 0x1b685e0);
    expect("cloud write", rva(saveWrite.cloudWrite), 0x1c9f270);
    expect("zip AddFile", rva(saveWrite.addFile), 0x1d03420);

    locate::VariableLocations vars;
    locate::LogScriptLocations logs;
    locate::MultiplayerLocations mp;
    uint8_t *effectCommand = nullptr, *readExecute = nullptr;
    if (!locate::LocateVariables(img, vars, error) ||
        !locate::LocateLogScript(img, vars.effectName, vars.triggerName, logs, error) ||
        !locate::LocateMultiplayer(img, mp, error) ||
        !locate::LocateEffectCommand(img, bench.gameState, &effectCommand, &readExecute, error)) {
        printf("FAIL script storage: %s\n", error.c_str());
        return 1;
    }
    printf("located script storage\n");
    expect("AccessVariablePointer", rva(vars.access), 0x9f7df0);
    expect("GetVariablePointer", rva(vars.get), 0x9f7ec0);
    expect("variables find", rva(vars.find), 0x227390);
    expect("CVariables::SetVariable", rva(vars.set), 0x7aae10);
    expect("variables map", vars.map, 0x8);
    expect("entry empty", vars.entryEmpty, 0x4);
    expect("entry value", vars.entryValue, 0x38);
    expect("variable effect name", vars.effectName, 0xc0);
    expect("variable trigger name", vars.triggerName, 0x220);
    expect("storage logEffect", rva(logs.logEffect), 0x1743450);
    expect("storage logTrigger", rva(logs.logTrigger), 0x19a7cf0);
    expect("log effect text", logs.logEffectText, 0xc0);
    expect("log trigger text", logs.logTriggerText, 0x220);
    expect("log text call", rva(logs.textCall), 0x17434f3);
    expect("ProcessString", rva(logs.processString), 0x9d3140);
    expect("multiplayer session", rva(mp.session), 0x3114e68);
    expect("multiplayer flag", mp.flag, 0x180);
    expect("storage effect command", rva(effectCommand), 0xec6390);
    expect("storage readExecute", rva(readExecute), 0xec58f0);
    locate::IntelLocations intel;
    if (!locate::LocateIntel(img, intel, error)) {
        printf("FAIL fleet intel: %s\n", error.c_str());
        return 1;
    }
    printf("located the fleet intel lookup\n");
    expect("intel lookup", rva(intel.lookup), 0x755bf0);
    expect("automation search", rva(intel.search), 0xe6e7d0);
    expect("selfObject", intel.selfObject, 0x2c48);
    expect("selfId", intel.selfId, 0x20);
    expect("selfRelation", rva(intel.selfRelation), 0x28522b0);
    expect("relData", intel.relData, 0x2c58);
    expect("relCount", intel.relCount, 0x2c64);
    expect("relObject", intel.relObject, 0x8);
    expect("defaultRelation", rva(intel.defaultRelation), 0x3114150);
    expect("relVirtual", intel.relVirtual, 0x38);
    expect("relFlags", intel.relFlags, 0x60);
    expect("relSharedBit", intel.relSharedBit, 25);
    expect("countryDb", rva(intel.countryDb), 0x3114c38);
    expect("nullCountry", rva(intel.nullCountry), 0x3112ea0);
    expect("countryId", intel.countryId, 0x20);
    expect("ownerIntel", intel.ownerIntel, 0x12e8);
    expect("hashFind", rva(intel.hashFind), 0xaf11a0);
    expect("noOwnIntel", intel.noOwnIntel, 0x314c);
    expect("ownIntelExpired", rva(intel.ownIntelExpired), 0x74ab60);
    expect("sysData", intel.sysData, 0x110);
    expect("sysCount", intel.sysCount, 0x11c);
    expect("sysKey", intel.sysKey, 0x8);
    expect("subData", intel.subData, 0x18);
    expect("subCount", intel.subCount, 0x24);
    expect("subValue", intel.subValue, 0xc);

    locate::CombatLocations combat;
    if (!locate::LocateCombat(img, combat, error)) {
        printf("FAIL combat map: %s\n", error.c_str());
        return 1;
    }
    printf("located the combat participants map\n");
    expect("combat insert", rva(combat.insert), 0x2c5e10);
    expect("combat map count", rva(combat.count), 0x34ee700);
    expect("countFirst", combat.countFirst, 0x14);
    expect("countLast", combat.countLast, 0xa4);
    expect("tail", combat.tail, 0xa8);

    locate::MegaAffordLocations afford;
    if (!locate::LocateMegaAfford(img, afford, error)) {
        printf("FAIL megastructure CanAfford: %s\n", error.c_str());
        return 1;
    }
    printf("located the AI's megastructure placement\n");
    expect("CanAfford", rva(afford.canAfford), 0xd50770);
    expect("FindBestPositionForMegaStructure", rva(afford.findBest), 0xbd7df0);

    locate::IncomeTargetLocations income;
    if (!locate::LocateIncomeTarget(img, income, error)) {
        printf("FAIL AI income targets: %s\n", error.c_str());
        return 1;
    }
    printf("located the AI's income target loop\n");
    expect("income target loop", rva(income.loop), 0xb52e70);
    expect("CalcScalingSubplans call", rva(income.scalingCall), 0xb52ed4);
    expect("CalcScalingSubplans", rva(income.calcScaling), 0xb53940);

    // The AI fixes: each part on its own, as stellaris-engine-fixes locates them.
    locate::AiFixLocations ai;
    struct {
        const char* what;
        bool (*locate)(const locate::Image&, locate::AiFixLocations&, std::string&);
    } aiParts[] = {
        {"the build plan", locate::LocateBuildPlan},
        {"the opportunist pass", locate::LocateOpportunist},
        {"the war distance malus", locate::LocateWarDistance},
        {"the truce division", locate::LocateTruce},
        {"the expansion data refresh", locate::LocateExpansionRefresh},
        {"the AI's day counters", locate::LocateAiSchedule},
        {"the AI's saved random", locate::LocateSavedRandom},
        {"the build plan's skip", locate::LocatePlanSkip},
        {"the idle colony ship count", locate::LocateColonizerIdle},
        {"the construction ship's range", locate::LocateStationRange},
        {"the survey blacklist's clear", locate::LocateSurveyBlacklist},
    };
    for (auto& part : aiParts) {
        if (!part.locate(img, ai, error)) {
            printf("FAIL AI fixes, %s: %s\n", part.what, error.c_str());
            return 1;
        }
    }
    printf("located the AI fixes\n");
    aibossthreat::Locations boss;
    if (!aibossthreat::Locate(img, boss, error)) {
        printf("FAIL AI boss threat floor: %s\n", error.c_str());
        return 1;
    }
    if (!TestBossThreatStub(boss)) {
        printf("FAIL AI boss threat floor stub\n");
        return 1;
    }
    printf("located and tested the AI boss threat floor\n");
    expect("boss estimate store", rva(boss.site), 0xba4abf);
    expect("cached fleet power", boss.cachedPower, 0x308);
    aiwarguard::Locations war;
    if (!aiwarguard::Locate(img, war, error)) {
        printf("FAIL AI war plan check: %s\n", error.c_str());
        return 1;
    }
    if (!TestWarRefreshStub(war)) {
        printf("FAIL AI war plan cancellation refresh\n");
        return 1;
    }
    printf("located the AI declaration check and tested the cancellation refresh\n");
    aifleettrace::Locations fleetTrace;
    if (!aifleettrace::Locate(img, fleetTrace)) {
        printf("FAIL AI fleet trace: signatures or call targets changed\n");
        ++failures;
    } else {
        printf("located the AI fleet trace\n");
    }
    expect("war score", rva(war.score), 0xc3cc20);
    expect("war declaration eligibility", rva(war.isPossible), 0x9ac4b0);
    expect("war cancellation flag write", rva(war.refresh), 0x6c347c);
    expect("plan walk", rva(ai.planWalk), 0xbc36b0);
    expect("plan stop jump", rva(ai.planStopJump), 0xbc378f);
    expect("plan continue", rva(ai.planContinue), 0xbc3c89);
    expect("colonizer idle count", rva(ai.colonizerIdle), 0xbb67ee);
    expect("colonizer ship count", ai.colonizerShipCount, 0x32c);
    expect("BuildStation", rva(ai.buildStation), 0xbbf870);
    expect("outpost penalty load", rva(ai.penaltyLoadOutpost), 0xbbe795);
    expect("station penalty load", rva(ai.penaltyLoadStation), 0xbbf728);
    expect("station penalty", rva(ai.stationPenalty), 0x3151db8);
    expect("survey blacklist jump", rva(ai.blacklistJump), 0xbb58b9);
    expect("minister strategy", ai.ministerStrategy, 0x88);
    expect("plan array", ai.planArray, 0x30);
    expect("plan Clear call", rva(ai.planClearCall), 0xbc3cc6);
    expect("plan Clear", rva(ai.planClear), 0xb551a0);
    expect("null buildable", rva(ai.nullBuildable), 0x3112f60);
    expect("ResolveColony", rva(ai.resolveColony), 0xbf5d70);
    expect("colony check object", ai.colonyCheckObject, 0x8);
    expect("colony check slot", ai.colonyCheckSlot, 0x8);
    expect("HasQueued...Construction", rva(ai.hasQueued), 0xdd2270);
    expect("opportunist pass", rva(ai.opportunist), 0xb79f0b);
    expect("opportunist join", rva(ai.opportunistJoin), 0xb7a06b);
    expect("target slot", ai.targetSlot, 0x88);
    expect("country wars", ai.countryWars, 0x2ba8);
    expect("country war count", ai.countryWarCount, 0x2bb4);
    expect("country personality", ai.countryPersonality, 0x33f8);
    expect("opportunist flag", ai.personalityOpportunist, 0x54);
    expect("country id", ai.countryId, 0x20);
    expect("country fleet power", ai.countryPower, 0x2f30);
    expect("war id", ai.warId, 0x20);
    expect("war attackers", ai.warAttackers, 0x80);
    expect("war defenders", ai.warDefenders, 0x98);
    expect("participant stride", ai.memberStride, 0x18);
    expect("participant id", ai.memberId, 0x8);
    expect("AI countryDb", rva(ai.countryDb), 0x3114c38);
    expect("AI nullCountry", rva(ai.nullCountry), 0x3112ea0);
    expect("warDb", rva(ai.warDb), 0x3114d10);
    expect("nullWar", rva(ai.nullWar), 0x3113758);
    expect("war distance malus", rva(ai.warDistance), 0xc3d27d);
    expect("MALUS_DISTANCE", rva(ai.malusDistance), 0x31159e8);
    expect("MALUS", rva(ai.malus), 0x3151e60);
    expect("malus skipped", rva(ai.malusSkip), 0xc3d2a4);
    expect("truce operand", rva(ai.truceOperand), 0xc3c762);
    expect("BOXED_IN_MULT", rva(ai.boxedInMult), 0x3115ba0);
    expect("expansion year check", rva(ai.expansionYears), 0xb6f8ee);
    expect("CAICore::Update", rva(ai.aiUpdate), 0x1afdd20);
    expect("AI core id", ai.coreId, 0x328);
    expect("AI random position", ai.randomPosition, 0x32c);
    expect("AI random seed", ai.randomSeed, 0x330);
    expect("AI week counter", ai.weekCounter, 0x334);
    expect("AI month counter", ai.monthCounter, 0x338);
    expect("AI year counter", ai.yearCounter, 0x33c);
    expect("saved random_count jump", rva(ai.savedRandomJumps[0]), 0xb77b02);
    expect("saved random_seed jump", rva(ai.savedRandomJumps[1]), 0xb77b41);
    expect("g_bUseSavedRandom", rva(ai.useSavedRandom), 0x3114e38);

    locate::LexerLocations lexer;
    if (!locate::LocateLexer(img, lexer, error)) {
        printf("FAIL token strings: %s\n", error.c_str());
        return 1;
    }
    printf("located the token strings' table\n");
    expect("RebuildLookup", rva(lexer.rebuild), 0x1bb5b20);
    expect("lexer instance", rva(lexer.instance), 0x1bb5ab0);
    expect("SetSizeAndEmplace", rva(lexer.setSize), 0x38d290);
    expect("CString assign", rva(lexer.assign), 0x15bcf0);
    expect("table", lexer.array, 0x68);
    expect("table data", lexer.data, 0x70);
    expect("table size", lexer.count, 0x7c);
    expect("wanted", lexer.wanted, 0x80);
    expect("dynamic data", lexer.dynData, 0x58);
    expect("dynamic count", lexer.dynCount, 0x64);
    expect("token name", lexer.tokenName, 0x10);

    locate::DeferredRefLocations deferred;
    if (!locate::LocateDeferredRefs(img, deferred, error)) {
        printf("FAIL deferred references: %s\n", error.c_str());
        return 1;
    }
    printf("located the deferred references: list +0x%zx/+0x%zx, entries 0x%zx bytes, owner +0x%zx\n", deferred.data,
           deferred.count, deferred.entrySize, deferred.owner);
    expect("deferred Register_Internal", rva(deferred.registerRef), 0x1bb8b90);
    expect("deferred Unregister", rva(deferred.unregister), 0x1bb8e60);
    if (deferred.data != 8 || deferred.count != 0x14 || deferred.entrySize != 0xa8 || deferred.owner != 0x60) {
        printf("FAIL deferred references: unexpected layout\n");
        return 1;
    }

    locate::ModifierDocsLocations docs;
    if (!locate::LocateModifierDocs(img, docs, error)) {
        printf("FAIL modifier documentation: %s\n", error.c_str());
        return 1;
    }
    printf("located the modifier documentation\n");
    expect("LogDefinitions", rva(docs.logDefinitions), 0x36d540);

    locate::CrtHeapLocations crt;
    if (!locate::LocateCrtHeap(img, crt, error)) {
        printf("FAIL CRT heap: %s\n", error.c_str());
        return 1;
    }
    printf("located the CRT heap\n");
    expect("_malloc_base", rva(crt.malloc), 0x2055ca8);
    expect("_calloc_base", rva(crt.calloc), 0x2055bc8);
    expect("_realloc_base", rva(crt.realloc), 0x2058250);
    expect("_free_base", rva(crt.free), 0x205579c);
    expect("_msize_base", rva(crt.msize), 0x204bff0);
    expect("__acrt_heap", rva(crt.heap), 0x3111180);

    printf(failures ? "%d FAILURES\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
