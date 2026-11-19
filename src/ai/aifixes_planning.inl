// Part of aifixes_runtime.inl: the fixes to what the AI builds and where its ships go (re/ai.md
// sections 2.1, 2.5, 2.15-2.18). The build plan, items the weekly walk can't order, the survey
// blacklist, idle colony and construction ships, and the expansion lists.

// ---- 2.1 The build plan --------------------------------------------------------------------
//
// The walk (BuildAndUpgradeDistrictsAndBuildings, weekly) orders items from the top of the plan
// until one can't be placed, then the game deletes the whole plan; it is made again only on the
// AI's month. With the fix:
// - before the walk, items whose colony is unusable or already builds a district or building go to
//   the end of the plan, in their order. The walk would stop at them; now it gets past them and
//   stops at them only at the end. They stay for later weeks.
// - after the walk, the items it ordered (moved out, so null) are dropped and the rest is kept,
//   instead of deleting it. The monthly CreateBuildPlan clears the plan before making a new one.
// The plan's weights are a month old at most, and every item is checked again before it is ordered.
// With the plan's debug log on, its explanation lines are numbered by the old order.

struct PlanItem {
    uint8_t* buildable;
    int64_t weight;
};
using PlanWalkFn = void(__fastcall*)(uint8_t* minister);
using PlanClearFn = void(__fastcall*)(uint8_t* array);
using ResolveColonyFn = uint8_t*(__fastcall*)(uint8_t* buildable);
using ColonyCheckFn = bool(__fastcall*)(uint8_t* sub);
using HasQueuedFn = bool(__fastcall*)(uint8_t* colony);

PlanWalkFn g_origPlanWalk = nullptr;
std::atomic<bool> g_planFailed{false}; // a fault: back to the game's behaviour from the next call
devstats::Counter g_planWalks{0}, g_planDeferred{0}, g_planOrdered{0}, g_planKept{0};

bool PlanItemBlocked(uint8_t* buildable) {
    if (buildable == *g_ai.nullBuildable)
        return true;
    uint8_t* colony = reinterpret_cast<ResolveColonyFn>(g_ai.resolveColony)(buildable);
    if (!colony)
        return true;
    uint8_t* sub = colony + g_ai.colonyCheckObject;
    auto check = *reinterpret_cast<ColonyCheckFn*>(*reinterpret_cast<uint8_t**>(sub) + g_ai.colonyCheckSlot);
    if (!check(sub))
        return true;
    return reinterpret_cast<HasQueuedFn>(g_ai.hasQueued)(colony);
}

// Moves the blocked items to the end, keeping the order of both groups. `spill` holds `count`.
// Returns how many moved, or -1 on a fault.
int DeferBlockedItems(PlanItem* items, int count, PlanItem* spill) {
    __try {
        int kept = 0, spilled = 0;
        for (int i = 0; i < count; ++i) {
            PlanItem item = items[i];
            if (item.buildable && PlanItemBlocked(item.buildable)) {
                spill[spilled++] = item;
            } else {
                items[kept++] = item;
            }
        }
        memcpy(items + kept, spill, sizeof(PlanItem) * spilled);
        return spilled;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The minister's plan and its size, or false on a fault.
bool ReadPlan(uint8_t* minister, uint8_t** array, int* count) {
    __try {
        *array = nullptr;
        *count = 0;
        uint8_t* strategy = *reinterpret_cast<uint8_t**>(minister + g_ai.ministerStrategy);
        if (strategy) {
            *array = strategy + g_ai.planArray;
            *count = *reinterpret_cast<int32_t*>(*array + g_ai.arrayCount);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall PlanWalkHook(uint8_t* minister) {
    thread_local std::vector<PlanItem> spill;
    uint8_t* array = nullptr;
    int count = 0;
    if (!g_planFailed && !ReadPlan(minister, &array, &count)) {
        g_planFailed = true;
        Log(LogLevel::Error, "ai_build_plan", "fault while reading a plan; the game's behaviour from now on");
    }
    if (!g_planFailed && array && count > 0) {
        if (spill.size() < static_cast<size_t>(count))
            spill.resize(count);
        auto items = *reinterpret_cast<PlanItem**>(array + g_ai.arrayData);
        int moved = DeferBlockedItems(items, count, spill.data());
        if (moved < 0) {
            g_planFailed = true;
            Log(LogLevel::Error, "ai_build_plan", "fault while sorting a plan; the game's behaviour from now on");
        } else {
            g_planWalks.fetch_add(1, std::memory_order_relaxed);
            g_planDeferred.fetch_add(moved, std::memory_order_relaxed);
        }
    }
    g_origPlanWalk(minister);
}

// Replaces the walk's Clear: drops what the walk ordered, keeps the rest.
bool KeepUnorderedItems(uint8_t* array) {
    __try {
        auto items = *reinterpret_cast<PlanItem**>(array + g_ai.arrayData);
        auto count = reinterpret_cast<int32_t*>(array + g_ai.arrayCount);
        uint8_t* null = *g_ai.nullBuildable;
        int kept = 0;
        for (int i = 0; i < *count; ++i) {
            if (items[i].buildable && items[i].buildable != null)
                items[kept++] = items[i];
        }
        g_planOrdered.fetch_add(*count - kept, std::memory_order_relaxed);
        g_planKept.fetch_add(kept, std::memory_order_relaxed);
        *count = kept;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall PlanClearHook(uint8_t* array) {
    if (!g_planFailed && KeepUnorderedItems(array))
        return;
    g_planFailed = true;
    reinterpret_cast<PlanClearFn>(g_ai.planClear)(array);
}

void InstallBuildPlanFix() {
    // The call first: until the walk is hooked too, it only keeps what the walk didn't order.
    Code jump;
    jump.JumpTo(reinterpret_cast<void*>(&PlanClearHook));
    uint8_t* stub = PlaceStub(jump, g_ai.planClearCall);
    int32_t rel;
    if (!stub || !Rel32(g_ai.planClearCall + 5, stub, &rel) || !WriteCode(g_ai.planClearCall + 1, &rel, 4)) {
        Log(LogLevel::Error, "ai_build_plan", "no memory near the game's code; off");
        return;
    }
    uint8_t* tramp =
        InstallJumpHook(g_ai.planWalk, locate::kPlanWalkStolenBytes, reinterpret_cast<void*>(&PlanWalkHook));
    if (!tramp) {
        Log(LogLevel::Warn, "ai_build_plan", "failed to hook the walk; only the unordered items are kept");
        return;
    }
    g_origPlanWalk = reinterpret_cast<PlanWalkFn>(tramp);
    Log(LogLevel::Info, "ai_build_plan", "on; the plan lasts until the AI's next month, blocked items wait at its end");
}

// ---- 2.16 Items the walk can't order ---------------------------------------------------------
//
// The walk checks each item (colony usable, nothing queued, affordable, ...) and stops at the
// first that fails; whatever is below it waits, for as long as the heaviest item can't be paid.
// The fix points the failed check's jump at the loop's continue, so the walk goes on to the next
// item. A failed item stays in the plan (build_plan_keep keeps it for the next week).

void InstallPlanSkipFix() {
    uint8_t* jump = g_ai.planStopJump;
    int32_t rel;
    if (jump[0] != 0x0F || jump[1] != 0x84 || !Rel32(jump + 6, g_ai.planContinue, &rel) ||
        !WriteCode(jump + 2, &rel, 4)) {
        Log(LogLevel::Error, "ai_build_plan_skip", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_build_plan_skip", "on; the weekly walk passes over items it can't order");
}

// ---- 2.15 The survey blacklist ---------------------------------------------------------------
//
// A system a science ship can't survey (hostile fleet seen, no clear path, ...) goes on a list
// that MonthlyUpdate empties only in the month country id % 12. The fix drops that test, so the
// list is emptied every AI month and each listed system is tested again once a month.

void InstallSurveyBlacklistFix() {
    const uint8_t nop2[2] = {0x66, 0x90};
    uint8_t* jump = g_ai.blacklistJump;
    if (jump[0] != 0x75 || jump[1] != 0x04 || !WriteCode(jump, nop2, sizeof(nop2))) {
        Log(LogLevel::Error, "ai_survey_blacklist", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_survey_blacklist", "on; emptied every AI month instead of once a year");
}

// ---- 2.17, 2.18 Ships without work -----------------------------------------------------------
//
// How long a fleet has been seen without work, by fleet id. AI updates run on worker threads.

struct IdleSpan {
    int64_t first, last; // days
};
struct IdleTracker {
    std::mutex lock;
    std::unordered_map<uint32_t, IdleSpan> spans;
    // Notes the fleet as idle today and returns the days it has been. A fleet not seen for more
    // than `gap` days (it had work, or its id belongs to a new fleet), or seen in the future (a
    // load), starts again.
    int64_t Seen(uint32_t id, int64_t day, int64_t gap) {
        std::lock_guard<std::mutex> guard(lock);
        if (spans.size() > 4096) {
            for (auto it = spans.begin(); it != spans.end();) {
                if (day < it->second.last || day - it->second.last > gap) {
                    it = spans.erase(it);
                } else {
                    ++it;
                }
            }
        }
        auto [it, added] = spans.try_emplace(id, IdleSpan{day, day});
        IdleSpan& span = it->second;
        if (day < span.last || day - span.last > gap)
            span.first = day;
        span.last = day;
        return day - span.first;
    }
    void Forget(uint32_t id) {
        std::lock_guard<std::mutex> guard(lock);
        spans.erase(id);
    }
};

// The fleet's id and today's day number; false on a fault or before a game is loaded.
bool ReadFleetAndDay(uint8_t* fleet, uint32_t* id, int64_t* day) {
    __try {
        uint8_t* state = g_bench.gameState ? *g_bench.gameState : nullptr;
        if (!state || !fleet)
            return false;
        *day = *reinterpret_cast<int32_t*>(state + g_bench.stateDate) / 24;
        *id = *reinterpret_cast<uint32_t*>(fleet + g_fleet.fleetIdOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 2.17 BuildColonizer builds a colony ship only when no colony ship is idle. A ship that can't
// reach any target stays idle for good and stops every later one, also one from a shipyard that
// could reach a target. With the fix an idle colony fleet counts as idle only for its first N
// days. The game's other limit stays: ships plus queued ships below the number of targets.

IdleTracker g_colonizerIdle;
int g_colonizerIdleDays = 0;
constexpr int64_t kColonizerGap = 45; // days between sightings that end an idle spell
devstats::Counter g_colonizerSeen{0}, g_colonizerPassed{0};

// The ships the idle count gets for this fleet: the game's number, or 0 once idle for long.
uint32_t __fastcall ColonizerIdleShips(uint8_t* fleet) {
    uint32_t ships = *reinterpret_cast<uint32_t*>(fleet + g_ai.colonizerShipCount);
    uint32_t id;
    int64_t day;
    if (!ReadFleetAndDay(fleet, &id, &day))
        return ships;
    g_colonizerSeen.fetch_add(1, std::memory_order_relaxed);
    if (g_colonizerIdle.Seen(id, day, kColonizerGap) < g_colonizerIdleDays)
        return ships;
    g_colonizerPassed.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void InstallColonizerFix(int days) {
    if (!g_bench.gameState || !g_fleet.fleetIdOffset) {
        Log(LogLevel::Error, "ai_colony_ships", "the game date or the fleet id wasn't found; off");
        return;
    }
    uint8_t* at = g_ai.colonizerIdle;
    Code c;
    c.Put({0x51, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});                          // push rcx, r9, r10, r11
    c.Put({0x48, 0x89, 0xF9});                                                  // mov rcx,rdi
    c.Put({0x48, 0x83, 0xEC, 0x20});                                            // sub rsp,20h
    c.Put({0x48, 0xB8}).Put64(reinterpret_cast<uint64_t>(&ColonizerIdleShips)); // mov rax,fn
    c.Put({0xFF, 0xD0});                                                        // call rax
    c.Put({0x48, 0x83, 0xC4, 0x20});                                            // add rsp,20h
    c.Put({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x59});                          // pop r11, r10, r9, rcx
    c.Put({0x44, 0x03, 0xF8});                                                  // add r15d,eax
    c.Put({0xE9}).Put32(0);                                                     // jmp back
    uint8_t* stub = AllocNear(at, c.n);
    uint8_t patch[locate::kColonizerIdleBytes] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    int32_t back, to;
    if (!stub || !Rel32(stub + c.n, at + sizeof(patch), &back) || !Rel32(at + 5, stub, &to)) {
        Log(LogLevel::Error, "ai_colony_ships", "no memory near the game's code; off");
        return;
    }
    memcpy(c.bytes + c.n - 4, &back, 4);
    memcpy(stub, c.bytes, c.n);
    FlushInstructionCache(GetCurrentProcess(), stub, c.n);
    memcpy(patch + 1, &to, 4);
    g_colonizerIdleDays = days;
    if (!WriteCode(at, patch, sizeof(patch))) {
        Log(LogLevel::Error, "ai_colony_ships", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_colony_ships", "on; a colony ship idle for %d days no longer stops another being built",
        days);
}

// 2.18 An idle construction ship asks BuildStation for work: a special project's station, a mining
// or research station, an outpost. Both searches take CONSTRUCTION_SHIP_DISTANCE_PENALTY per jump
// from the ship off a target's score, so far targets fall below zero and the ship stays idle next
// to them. With the fix, a ship BuildStation found nothing for during N days searches with half
// the penalty, and from 2N days with none, until it has work again. Only this ship's search
// changes: the factor is per thread, and the two loads of the define read it.

using BuildStationFn = uint64_t(__fastcall*)(uint8_t* minister, uint8_t* fleet, void* a3, uint64_t a4, void* a5);
BuildStationFn g_origBuildStation = nullptr;
IdleTracker g_constructorIdle;
int g_constructorIdleDays = 0;
constexpr int64_t kConstructorGap = 30;
thread_local int t_penaltyQuarters = 4;
devstats::Counter g_stationCalls{0}, g_stationWidened{0}, g_stationWidenedFound{0};

int64_t __fastcall StationPenalty() {
    int64_t penalty = *g_ai.stationPenalty;
    int quarters = t_penaltyQuarters;
    return quarters >= 4 ? penalty : penalty * quarters / 4;
}

uint64_t __fastcall BuildStationHook(uint8_t* minister, uint8_t* fleet, void* a3, uint64_t a4, void* a5) {
    uint32_t id;
    int64_t day;
    if (!ReadFleetAndDay(fleet, &id, &day))
        return g_origBuildStation(minister, fleet, a3, a4, a5);
    g_stationCalls.fetch_add(1, std::memory_order_relaxed);
    int64_t idle = g_constructorIdle.Seen(id, day, kConstructorGap);
    int quarters = idle >= 2 * int64_t(g_constructorIdleDays) ? 0 : idle >= g_constructorIdleDays ? 2 : 4;
    t_penaltyQuarters = quarters;
    uint64_t found = g_origBuildStation(minister, fleet, a3, a4, a5);
    t_penaltyQuarters = 4;
    if (quarters < 4) {
        g_stationWidened.fetch_add(1, std::memory_order_relaxed);
        if (found & 0xFF)
            g_stationWidenedFound.fetch_add(1, std::memory_order_relaxed);
    } else if (found & 0xFF) {
        g_constructorIdle.Forget(id); // it found work at the game's range
    }
    return found;
}

// Replaces a 7-byte load of the define with a call (and a 2-byte nop). The stub returns the scaled
// penalty in the register the game loaded (`slot`: where the stub's pushes keep that register)
// and leaves everything else as it was.
bool PatchPenaltyLoad(uint8_t* at, uint8_t rex, uint8_t modrm, uint8_t slot) {
    if (at[0] != rex || at[1] != 0x8B || at[2] != modrm)
        return false;
    Code c;
    c.Put(
        {0x9C, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53}); // pushfq; push rax, rcx, rdx, r8-r11
    c.Put({0x48, 0x83, 0xEC, 0x28});                                               // sub rsp,28h
    c.Put({0x48, 0xB8}).Put64(reinterpret_cast<uint64_t>(&StationPenalty));        // mov rax,fn
    c.Put({0xFF, 0xD0});                                                           // call rax
    c.Put({0x48, 0x83, 0xC4, 0x28});                                               // add rsp,28h
    c.Put({0x48, 0x89, 0x44, 0x24, slot});                                         // mov [rsp+slot],rax
    c.Put({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, 0x9D, 0xC3}); // pops; popfq; ret
    uint8_t* stub = PlaceStub(c, at);
    uint8_t patch[locate::kPenaltyLoadBytes] = {0xE8, 0, 0, 0, 0, 0x66, 0x90};
    int32_t rel;
    if (!stub || !Rel32(at + 5, stub, &rel))
        return false;
    memcpy(patch + 1, &rel, 4);
    return WriteCode(at, patch, sizeof(patch));
}

void InstallStationRangeFix(int days) {
    if (!g_bench.gameState || !g_fleet.fleetIdOffset) {
        Log(LogLevel::Error, "ai_construction_ships", "the game date or the fleet id wasn't found; off");
        return;
    }
    g_constructorIdleDays = days;
    // The loads first: until BuildStation is hooked they return the game's value.
    if (!PatchPenaltyLoad(g_ai.penaltyLoadOutpost, 0x4C, 0x15, 0x08) || // mov r10,[rip+define]
        !PatchPenaltyLoad(g_ai.penaltyLoadStation, 0x4C, 0x05, 0x18)) { // mov r8,[rip+define]
        Log(LogLevel::Error, "ai_construction_ships", "failed to patch the distance penalty; off");
        return;
    }
    uint8_t* tramp = InstallJumpHook(g_ai.buildStation, locate::kBuildStationStolenBytes,
                                     reinterpret_cast<void*>(&BuildStationHook));
    if (!tramp) {
        Log(LogLevel::Error, "ai_construction_ships", "failed to hook BuildStation; off");
        return;
    }
    g_origBuildStation = reinterpret_cast<BuildStationFn>(tramp);
    Log(LogLevel::Info, "ai_construction_ships",
        "on; without work for %d days a ship searches with half the distance penalty, "
        "from %d days with none",
        days, 2 * days);
}

// ---- 2.5 The expansion lists ---------------------------------------------------------------------
//
// The colonize data (colonize, explore, stations, outposts, expand borders, terraform) is rebuilt
// every month during the first AI_BUDGETING_START_YEARS years, then once a year per country. The
// year check's jl becomes a jmp: monthly for the whole game.

void InstallExpansionFix() {
    const uint8_t jmp = 0xEB;
    if (*g_ai.expansionYears != 0x7C || !WriteCode(g_ai.expansionYears, &jmp, 1)) {
        Log(LogLevel::Error, "ai_expansion_lists", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_expansion_lists", "on; rebuilt every month");
}
