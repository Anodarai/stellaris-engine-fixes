// Part of aifixes_runtime.inl: the AI fleet trace ([diagnostics] ai_fleet_country, ai_fleet_system;
// aifleettrace.h). Logs, for one country or system, which fleets the AI's military planning scans,
// what it wants, and why adding a fleet to a group fails.

constexpr uint32_t kNoAiTraceId = UINT32_MAX;
uint32_t g_aiTraceCountry = kNoAiTraceId;
uint32_t g_aiTraceSystem = kNoAiTraceId;
aifleettrace::Locations g_aiTraceAt{}; // the functions, and the field offsets read from them

using AiTraceScanFn = void (*)(const uint8_t*, const uint8_t*, bool);
using AiTraceCollectFn = void (*)(const uint8_t*, uint8_t*);
using AiTraceCountryFn = const uint8_t* (*)(const uint8_t*);
using AiTraceArmedConflictFn = bool (*)(const uint8_t*);
using AiTraceWantedFn = int64_t* (*)(const uint8_t*, int64_t*, int, const uint8_t*, void*);
using AiTraceAssignFn = int64_t* (*)(const uint8_t*, int64_t*, const uint8_t*, const uint8_t*, int, bool);
using AiTraceAddFn = int (*)(const uint8_t*, const void*, uint32_t, int, void*, void*, bool);
AiTraceScanFn g_aiTraceScan = nullptr;
AiTraceCollectFn g_aiTraceCollect = nullptr;
AiTraceCountryFn g_aiTraceCountryFor = nullptr;
AiTraceArmedConflictFn g_aiTraceArmedConflict = nullptr;
AiTraceWantedFn g_aiTraceWanted = nullptr;
AiTraceAssignFn g_aiTraceAssign = nullptr;
AiTraceAddFn g_aiTraceAdd = nullptr;

template <typename T> T AiTraceRead(const uint8_t* object, size_t offset, T fallback) {
    if (!object)
        return fallback;
    __try {
        return *reinterpret_cast<const T*>(object + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

bool AiTraceMatchingGroup(const uint8_t* group) {
    return AiTraceRead(group, g_aiTraceAt.groupSystem, kNoAiTraceId) == g_aiTraceSystem &&
           AiTraceRead(group, g_aiTraceAt.groupCountry, kNoAiTraceId) == g_aiTraceCountry;
}

void AiTraceLogScan(const uint8_t* minister, const uint8_t* system, bool local) {
    const auto& at = g_aiTraceAt;
    __try {
        const auto country = g_aiTraceCountryFor(minister);
        if (!country || *reinterpret_cast<const uint32_t*>(country + at.countryId) != g_aiTraceCountry)
            return;
        const auto aiModule = *reinterpret_cast<const uint8_t* const*>(minister + at.ministerModule);
        const auto moduleVtable = *reinterpret_cast<const uintptr_t* const*>(aiModule);
        using GetAi = const uint8_t* (*)(const uint8_t*);
        const auto ai = reinterpret_cast<GetAi>(moduleVtable[at.moduleAiSlot / 8])(aiModule);
        const auto aiVtable = *reinterpret_cast<const uintptr_t* const*>(ai);
        using GetStrategies = const uint8_t* (*)(const uint8_t*, int);
        const auto strategies =
            reinterpret_cast<GetStrategies>(aiVtable[at.aiStrategiesSlot / 8])(ai, static_cast<int>(at.hostileList));
        const auto entries = *reinterpret_cast<const uint8_t* const* const*>(strategies + at.strategiesEntries);
        const int count = *reinterpret_cast<const int*>(strategies + at.strategiesCount);
        const auto ids = *reinterpret_cast<const uint32_t* const*>(system + at.systemFleetArray);
        const int fleetCount = *reinterpret_cast<const int*>(system + at.systemFleetArray + 12);
        const bool preparingWar = (*reinterpret_cast<const uint32_t*>(ai + at.aiFlags) & 4) != 0;
        Log(LogLevel::Debug, "ai_fleet_trace",
            "scan local=%d prepare_war=%d armed_conflict_with_allies=%d fleets_present=%d", local, preparingWar,
            g_aiTraceArmedConflict(country), fleetCount);
        for (int i = 0; i < count; ++i) {
            const auto id = *reinterpret_cast<const uint32_t*>(entries[i] + at.entryTarget);
            for (int j = 0; j < fleetCount; ++j) {
                if (ids[j] != id)
                    continue;
                Log(LogLevel::Debug, "ai_fleet_trace", "hostile target=%u in strategy list", id);
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(LogLevel::Warn, "ai_fleet_trace", "could not read the hostile strategy");
    }
}

void AiTraceScanHook(const uint8_t* minister, const uint8_t* system, bool local) {
    g_aiTraceScan(minister, system, local);
    if (AiTraceRead(system, g_aiTraceAt.systemId, kNoAiTraceId) == g_aiTraceSystem)
        AiTraceLogScan(minister, system, local);
}

void AiTraceCollectHook(const uint8_t* minister, uint8_t* available) {
    g_aiTraceCollect(minister, available);
    if (AiTraceRead(g_aiTraceCountryFor(minister), g_aiTraceAt.countryId, kNoAiTraceId) == g_aiTraceCountry) {
        Log(LogLevel::Debug, "ai_fleet_trace", "collected %d available fleets",
            AiTraceRead(available, g_aiTraceAt.availableCount, -1));
    }
}

// The target is a military group's leading part, so it has the group's target and system fields.
int64_t* AiTraceWantedHook(const uint8_t* minister, int64_t* power, int mission, const uint8_t* target,
                           void* explanation) {
    auto result = g_aiTraceWanted(minister, power, mission, target, explanation);
    if (AiTraceRead(target, g_aiTraceAt.groupSystem, kNoAiTraceId) != g_aiTraceSystem ||
        AiTraceRead(g_aiTraceCountryFor(minister), g_aiTraceAt.countryId, kNoAiTraceId) != g_aiTraceCountry)
        return result;
    Log(LogLevel::Debug, "ai_fleet_trace", "target=%u mission=%d required_power=%.2f",
        AiTraceRead(target, g_aiTraceAt.groupTarget, kNoAiTraceId), mission, *power / 32768.0);
    return result;
}

int64_t* AiTraceAssignHook(const uint8_t* minister, int64_t* power, const uint8_t* available, const uint8_t* group,
                           int maxDistance, bool armedConflict) {
    const bool trace = AiTraceMatchingGroup(group);
    if (trace) {
        Log(LogLevel::Debug, "ai_fleet_trace", "assign target=%u mission=%u available_fleets=%d max_distance=%d",
            AiTraceRead(group, g_aiTraceAt.groupTarget, kNoAiTraceId),
            AiTraceRead(group, g_aiTraceAt.groupMission, kNoAiTraceId),
            AiTraceRead(available, g_aiTraceAt.availableCount, -1), maxDistance);
    }
    auto result = g_aiTraceAssign(minister, power, available, group, maxDistance, armedConflict);
    if (trace) {
        Log(LogLevel::Debug, "ai_fleet_trace", "assigned_power=%.2f group_fleets=%d", *power / 32768.0,
            AiTraceRead(group, g_aiTraceAt.groupFleetCount, -1));
    }
    return result;
}

int AiTraceAddHook(const uint8_t* group, const void* policy, uint32_t fleet, int maxDistance, void* cache,
                   void* powerCache, bool armedConflict) {
    const bool trace = AiTraceMatchingGroup(group);
    const int result = g_aiTraceAdd(group, policy, fleet, maxDistance, cache, powerCache, armedConflict);
    if (trace) {
        static constexpr const char* reasons[] = {
            "accepted",           "invalid fleet",    "invalid target system",       "failed combat readiness policy",
            "incompatible fleet", "already in group", "outside assignment distance", "route or grouping failed"};
        const char* reason = result >= 0 && result < static_cast<int>(std::size(reasons)) ? reasons[result] : "unknown";
        Log(LogLevel::Debug, "ai_fleet_trace", "fleet=%u target=%u add_result=%d (%s)", fleet,
            AiTraceRead(group, g_aiTraceAt.groupTarget, kNoAiTraceId), result, reason);
    }
    return result;
}

void InstallAiFleetTrace(HMODULE self, const locate::Image& image) {
    g_aiTraceCountry = static_cast<uint32_t>(IniInt(self, "diagnostics", "ai_fleet_country", -1));
    g_aiTraceSystem = static_cast<uint32_t>(IniInt(self, "diagnostics", "ai_fleet_system", -1));
    if (g_aiTraceCountry == kNoAiTraceId || g_aiTraceSystem == kNoAiTraceId)
        return;

    auto& found = g_aiTraceAt; // filled before any hook can run
    if (!aifleettrace::Locate(image, found)) {
        Log(LogLevel::Error, "ai_fleet_trace", "could not locate the military planner");
        return;
    }
    g_aiTraceCountryFor = reinterpret_cast<AiTraceCountryFn>(found.country);
    g_aiTraceArmedConflict = reinterpret_cast<AiTraceArmedConflictFn>(found.armedConflict);
    g_aiTraceScan = reinterpret_cast<AiTraceScanFn>(
        InstallJumpHook(found.scan, aifleettrace::kScanStolen, reinterpret_cast<void*>(&AiTraceScanHook)));
    g_aiTraceCollect = reinterpret_cast<AiTraceCollectFn>(
        InstallJumpHook(found.collect, aifleettrace::kCollectStolen, reinterpret_cast<void*>(&AiTraceCollectHook)));
    g_aiTraceWanted = reinterpret_cast<AiTraceWantedFn>(
        InstallJumpHook(found.wanted, aifleettrace::kWantedStolen, reinterpret_cast<void*>(&AiTraceWantedHook)));
    g_aiTraceAssign = reinterpret_cast<AiTraceAssignFn>(
        InstallJumpHook(found.assign, aifleettrace::kAssignStolen, reinterpret_cast<void*>(&AiTraceAssignHook)));
    g_aiTraceAdd = reinterpret_cast<AiTraceAddFn>(
        InstallJumpHook(found.add, aifleettrace::kAddStolen, reinterpret_cast<void*>(&AiTraceAddHook)));
    Log(LogLevel::Debug, "ai_fleet_trace", "country=%u system=%u hooks scan=%d collect=%d wanted=%d assign=%d add=%d",
        g_aiTraceCountry, g_aiTraceSystem, g_aiTraceScan != nullptr, g_aiTraceCollect != nullptr,
        g_aiTraceWanted != nullptr, g_aiTraceAssign != nullptr, g_aiTraceAdd != nullptr);
}
