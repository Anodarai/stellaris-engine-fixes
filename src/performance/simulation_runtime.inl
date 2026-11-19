// [speed] savings inside the simulation: fleet automation's intel lookup (fast_intel_lookup), the
// empty combat participant entries (skip_empty_combat_entries) and the AI's megastructure placement
// (mega_afford_cache). None changes what the game computes; each has a *_verify setting or an A/B
// switch in sef_bench. Included by dllmain.cpp inside its anonymous namespace, after the hooking
// helpers and the ini settings. See re/performance_candidates.md.

// ===========================================================================
// Fleet automation's intel lookup
//
// Fleet automation finds its targets with a shortest-path search over every system. For each
// system it expands, the game walks the fleets there and asks, for each, the searching country's
// intel on the fleet's owner in that system (`lookup`). That function scans the country's
// relations for the owner and then, without shared intel, the country's per-system list for the
// system: two linear scans per call, a large part of automation's time.
//
// While a search runs nothing changes the game state (it only reads), so during one search
// call, on its thread, the answers are computed once:
//   - per (country, owner): the relation's verdict on shared intel (the same virtual check and
//     flag the game reads);
//   - per country: whether it has intel of its own (the same field and function);
//   - per (country, system): the largest value of the system's entry, from one pass over the list
//     (the first entry for a system wins, as in the game's scan).
// Shared intel is still read through the game's own hash lookup. The caches are dropped when the
// search returns. Outside a search the game's function runs unchanged.
//
// [speed] fast_intel_lookup=0 turns this off; fast_intel_verify=1 also runs the game's function
// for every lookup, returns the game's answer, and logs any difference.

locate::IntelLocations g_intel;
game::IntelFn g_origIntel = nullptr;
game::SearchFn g_origSearch = nullptr;
std::atomic<bool> g_intelOn{false};
bool g_intelVerify = false;
devstats::Counter g_intelSearches{0}, g_intelFast{0}, g_intelCompared{0};
std::atomic<uint64_t> g_intelMismatches{0}; // limits the verify log
std::atomic<bool> g_intelFailed{false};

struct IntelCache {
    uint8_t* country = nullptr;
    std::unordered_map<uint32_t, bool> shared; // owner id -> shared intel
    int ownIntel = -1;                         // -1 not known yet; 0 none (the game returns 0); 1 has some
    bool systemsBuilt = false;
    std::unordered_map<int32_t, int32_t> systems; // system id -> largest sub-entry value (at least 0)
};
thread_local int t_searchDepth = 0;
thread_local std::vector<IntelCache> t_intelCaches; // one per country seen in the current search
thread_local size_t t_intelUsed = 0;

IntelCache& IntelCacheFor(uint8_t* country) {
    for (size_t i = 0; i < t_intelUsed; ++i) {
        if (t_intelCaches[i].country == country)
            return t_intelCaches[i];
    }
    if (t_intelUsed == t_intelCaches.size())
        t_intelCaches.emplace_back();
    IntelCache& c = t_intelCaches[t_intelUsed++];
    c.country = country;
    c.shared.clear();
    c.ownIntel = -1;
    c.systemsBuilt = false;
    c.systems.clear();
    return c;
}

// The game's lookup, from the caches. Reads only what the game's function reads.
int32_t FastIntel(uint8_t* country, int32_t system, uint32_t owner) {
    const locate::IntelLocations& L = g_intel;
    IntelCache& c = IntelCacheFor(country);
    bool shared;
    auto known = c.shared.find(owner);
    if (known != c.shared.end()) {
        shared = known->second;
    } else {
        uint8_t* relation;
        if (owner == At<uint32_t>(At<uint8_t*>(country, L.selfObject), L.selfId)) {
            relation = L.selfRelation;
        } else {
            relation = *L.defaultRelation;
            uint8_t* entry = At<uint8_t*>(country, L.relData);
            int count = At<int32_t>(country, L.relCount);
            for (int i = 0; i < count; ++i, entry += 0x70) {
                if (At<uint32_t>(entry, 0) == owner) {
                    relation = entry + L.relObject;
                    break;
                }
            }
        }
        auto check = At<game::RelationCheckFn>(At<uint8_t*>(relation, 0), L.relVirtual);
        shared = check(relation) && ((At<uint32_t>(relation, L.relFlags) >> L.relSharedBit) & 1) != 0;
        c.shared.emplace(owner, shared);
    }
    if (shared) {
        uint8_t* ownerCountry = ResolveRef(*L.countryDb, owner, L.countryId);
        if (!ownerCountry)
            ownerCountry = *L.nullCountry;
        return static_cast<int32_t>(
            reinterpret_cast<game::HashFindFn>(L.hashFind)(ownerCountry + L.ownerIntel, system));
    }
    if (c.ownIntel < 0) {
        c.ownIntel = At<int32_t>(country, L.noOwnIntel) == 0 &&
                             !reinterpret_cast<game::CountryCheckFn>(L.ownIntelExpired)(country)
                         ? 1
                         : 0;
    }
    if (c.ownIntel == 0)
        return 0;
    if (!c.systemsBuilt) {
        uint8_t* entry = At<uint8_t*>(country, L.sysData);
        int count = At<int32_t>(country, L.sysCount);
        c.systems.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i, entry += 0x28) {
            int32_t key = At<int32_t>(entry, L.sysKey);
            if (c.systems.count(key))
                continue; // the game's scan stops at the first
            int32_t best = 0;
            uint8_t* sub = At<uint8_t*>(entry, L.subData);
            int subs = At<int32_t>(entry, L.subCount);
            for (int j = 0; j < subs; ++j, sub += 0x18)
                best = (std::max)(best, At<int32_t>(sub, L.subValue));
            c.systems.emplace(key, best);
        }
        c.systemsBuilt = true;
    }
    auto found = c.systems.find(system);
    return found == c.systems.end() ? 0 : found->second;
}

// FastIntel behind SEH: false on a fault.
bool FastIntelGuarded(uint8_t* country, int32_t system, uint32_t owner, int32_t* out) {
    __try {
        *out = FastIntel(country, system, owner);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall IntelHook(uint8_t* country, int32_t system, uint32_t owner) {
    if (t_searchDepth == 0 || !g_intelOn)
        return g_origIntel(country, system, owner);
    int32_t fast = 0;
    if (!FastIntelGuarded(country, system, owner, &fast)) {
        if (!g_intelFailed.exchange(true))
            Log(LogLevel::Error, "fleet_intel", "fault in the fast lookup; it is off from now on");
        g_intelOn = false;
        return g_origIntel(country, system, owner);
    }
    g_intelFast.fetch_add(1, std::memory_order_relaxed);
    if (!g_intelVerify)
        return static_cast<uint32_t>(fast);
    uint64_t game = g_origIntel(country, system, owner);
    g_intelCompared.fetch_add(1, std::memory_order_relaxed);
    if (static_cast<int32_t>(game) != fast && g_intelMismatches.fetch_add(1) < 20) {
        Log(LogLevel::Error, "fleet_intel", "MISMATCH: country %p system %d owner %08X: game %d, fast %d", country,
            system, owner, static_cast<int32_t>(game), fast);
    }
    return game;
}

// Scope for the caches: one search call on this thread.
struct SearchScope {
    SearchScope() { ++t_searchDepth; }
    ~SearchScope() {
        if (--t_searchDepth == 0)
            t_intelUsed = 0;
    }
};

uint64_t __fastcall SearchHook(void* a, void* b, void* c, void* d, void* e, void* f) {
    if (!g_intelOn)
        return g_origSearch(a, b, c, d, e, f);
    g_intelSearches.fetch_add(1, std::memory_order_relaxed);
    SearchScope scope;
    return g_origSearch(a, b, c, d, e, f);
}

void InstallIntel(HMODULE self, const locate::Image& img) {
    bool enabled = IniOn(self, "speed", "fast_intel_lookup", true);
    g_intelVerify = IniOn(self, "speed", "fast_intel_verify", false);
    if (!enabled) {
        Log(LogLevel::Info, "fleet_intel", "off (fast_intel_lookup=0)");
        return;
    }
    std::string error;
    if (!locate::LocateIntel(img, g_intel, error)) {
        Log(LogLevel::Error, "fleet_intel", "%s; the fast lookup is off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "fleet_intel",
        "lookup +%llX, search +%llX; relations +0x%zx/+0x%zx, systems +0x%zx/+0x%zx, owner intel +0x%zx",
        Rva(g_intel.lookup), Rva(g_intel.search), g_intel.relData, g_intel.relCount, g_intel.sysData, g_intel.sysCount,
        g_intel.ownerIntel);
    auto lookup = InstallJumpHook(g_intel.lookup, locate::kIntelLookupStolenBytes, reinterpret_cast<void*>(&IntelHook));
    if (!lookup) {
        Log(LogLevel::Error, "fleet_intel", "failed to hook the lookup; the fast lookup is off");
        return;
    }
    g_origIntel = reinterpret_cast<game::IntelFn>(lookup); // passes through until g_intelOn
    auto search = InstallJumpHook(g_intel.search, locate::kSearchStolenBytes, reinterpret_cast<void*>(&SearchHook));
    if (!search) {
        Log(LogLevel::Error, "fleet_intel", "failed to hook the search; the fast lookup is off");
        return;
    }
    g_origSearch = reinterpret_cast<game::SearchFn>(search);
    g_intelOn = true;
    Log(LogLevel::Info, "fleet_intel", "hooks installed; fast lookup on%s",
        g_intelVerify ? ", verifying every lookup against the game's" : "");
}

#ifdef SEF_DEV_STATS
void LogIntelStats() {
    if (!g_origSearch)
        return;
    Log(LogLevel::Stats, "fleet_intel",
        "searches=%llu fast_lookups=%llu faulted=%d verify=%d compared=%llu mismatches=%llu", g_intelSearches.load(),
        g_intelFast.load(), g_intelFailed.load(), g_intelVerify, g_intelCompared.load(), g_intelMismatches.load());
}
#endif

// ---- Combat participants map ----
//
// Every tick, CGameState::MicroUpdate empties a static map of fleet id -> SCombatParticipants and
// inserts an entry for every fleet, including the many not in combat, whose entry is empty (seven
// empty arrays and a zero 16-bit field). Both readers, MicroUpdate's second fleet loop and the
// parallel ship update, look a fleet up and use an empty SCombatParticipants when it is missing.
// So an empty entry and no entry give the same result, and the hook skips inserting empty ones:
// fewer entries to insert, destroy, copy and probe, every tick.
//
// The caller only reads the result's `inserted` flag (if false it moves the value into the entry
// found), so a skipped insert reports inserted = true. An id already inserted since the map was
// last empty goes to the game's insert, so a repeated id behaves exactly as before.
//
// [speed] skip_empty_combat_entries=0 turns this off. With it on, `sef_bench combat off|on` switches
// the skipping at run time, for A/B runs of one save; the switch takes effect when the map is next
// empty (the next tick).

locate::CombatLocations g_combat;
game::CombatInsertFn g_origCombatInsert = nullptr;
devstats::MainCounter g_combatInserts = 0, g_combatSkipped = 0;
std::vector<uint32_t> g_combatIds; // ids passed to the game's insert since the map was last empty
bool g_combatWanted = true;        // set by `sef_bench combat`
bool g_combatSkipping = true;      // g_combatWanted, taken when the map is empty

bool IsEmptyParticipants(const uint8_t* value) {
    for (size_t at = g_combat.countFirst; at <= g_combat.countLast; at += 0x18) {
        if (At<int32_t>(value, at) != 0)
            return false;
    }
    return At<uint16_t>(value, g_combat.tail) == 0;
}

game::InsertResult* __fastcall CombatInsertHook(void* unused, game::InsertResult* out, uint32_t hash,
                                                const uint32_t* id, uint8_t* value) {
    ++g_combatInserts; // MicroUpdate's serial part: main thread only
    if (*g_combat.count == 0) {
        g_combatIds.clear();
        g_combatSkipping = g_combatWanted;
    }
    if (!g_combatSkipping)
        return g_origCombatInsert(unused, out, hash, id, value);
    if (IsEmptyParticipants(value) && std::find(g_combatIds.begin(), g_combatIds.end(), *id) == g_combatIds.end()) {
        ++g_combatSkipped;
        out->entry = nullptr;
        out->inserted = true;
        return out;
    }
    g_combatIds.push_back(*id);
    return g_origCombatInsert(unused, out, hash, id, value);
}

void InstallCombat(HMODULE self, const locate::Image& img) {
    if (!IniOn(self, "speed", "skip_empty_combat_entries", true)) {
        Log(LogLevel::Info, "combat_map", "off (skip_empty_combat_entries=0)");
        return;
    }
    std::string error;
    if (!locate::LocateCombat(img, g_combat, error)) {
        Log(LogLevel::Error, "combat_map", "%s; off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "combat_map", "insert +%llX, count +%llX; entry counts +0x%zx..+0x%zx, field +0x%zx",
        Rva(g_combat.insert), Rva(g_combat.count), g_combat.countFirst, g_combat.countLast, g_combat.tail);
    g_combatIds.reserve(256);
    auto tramp =
        InstallJumpHook(g_combat.insert, locate::kCombatInsertStolenBytes, reinterpret_cast<void*>(&CombatInsertHook));
    if (!tramp) {
        Log(LogLevel::Error, "combat_map", "failed to hook the insert; off");
        return;
    }
    g_origCombatInsert = reinterpret_cast<game::CombatInsertFn>(tramp);
    Log(LogLevel::Info, "combat_map", "hook installed; empty entries are not inserted");
}

#ifdef SEF_DEV_STATS
void LogCombatStats() {
    if (!g_origCombatInsert)
        return;
    Log(LogLevel::Stats, "combat_map", "inserts=%llu empty_skipped=%llu skipped_pct=%.1f", g_combatInserts,
        g_combatSkipped, g_combatInserts ? 100.0 * g_combatSkipped / g_combatInserts : 0.0);
}
#endif

// ---- AI megastructure placement ----
//
// In a country's weekly AI update, CAIInteriorMinister::BuildMegaStructure calls
// FindBestPositionForMegaStructure for a megastructure type and a construction ship. That scores
// every system the ship can reach, and each score calls CMegaStructureType::CanQueue, which ends in
// CanAfford(type, country): the type's cost for the country, from its modifiers, checked against
// the country's resources. The system plays no part in it, and nothing in the scoring loop changes
// the country (the AI only posts commands, and runs with the game state read-only). So within one
// FindBestPosition call, on its thread, the answer for a (type, country) is computed once.
// Calls that ask for the reason text (a CString*) always run the game's function.
//
// The weekly update of the largest AI empire is what the main thread waits for at the end of the
// parallel AI update; this placement search is a large part of it (re/simulation_speed.md).
//
// [speed] mega_afford_cache=0 turns this off; mega_afford_verify=1 also runs the game's function
// on every cached answer and logs any difference.

locate::MegaAffordLocations g_afford;
game::CanAffordFn g_origCanAfford = nullptr;
game::FindBestPositionFn g_origFindBest = nullptr;
bool g_affordVerify = false;
devstats::Counter g_affordScopes{0}, g_affordCalls{0}, g_affordCached{0};
std::atomic<uint64_t> g_affordMismatches{0}; // limits the verify log

struct AffordEntry {
    const uint8_t* type;
    const uint8_t* country;
    bool result;
};
thread_local int t_affordDepth = 0;
thread_local std::vector<AffordEntry> t_affordCache;

bool __fastcall CanAffordHook(const uint8_t* type, const uint8_t* country, void* why) {
    if (t_affordDepth == 0 || why != nullptr)
        return g_origCanAfford(type, country, why);
    g_affordCalls.fetch_add(1, std::memory_order_relaxed);
    for (const AffordEntry& e : t_affordCache) {
        if (e.type != type || e.country != country)
            continue;
        g_affordCached.fetch_add(1, std::memory_order_relaxed);
        if (g_affordVerify) {
            bool game = g_origCanAfford(type, country, why);
            if (game != e.result && g_affordMismatches.fetch_add(1) < 20) {
                Log(LogLevel::Error, "mega_afford", "CanAfford MISMATCH: type %p country %p: game %d, cached %d", type,
                    country, game, e.result);
            }
            return game;
        }
        return e.result;
    }
    bool result = g_origCanAfford(type, country, why);
    t_affordCache.push_back({type, country, result});
    return result;
}

// Scope for the cache: one FindBestPosition call on this thread.
struct AffordScope {
    AffordScope() { ++t_affordDepth; }
    ~AffordScope() {
        if (--t_affordDepth == 0)
            t_affordCache.clear();
    }
};

void* __fastcall FindBestHook(void* a, void* b, void* c, void* d, void* e, void* f, void* g) {
    g_affordScopes.fetch_add(1, std::memory_order_relaxed);
    AffordScope scope;
    return g_origFindBest(a, b, c, d, e, f, g);
}

void InstallMegaAfford(HMODULE self, const locate::Image& img) {
    if (!IniOn(self, "speed", "mega_afford_cache", true)) {
        Log(LogLevel::Info, "mega_afford", "off (mega_afford_cache=0)");
        return;
    }
    g_affordVerify = IniOn(self, "speed", "mega_afford_verify", false);
    std::string error;
    if (!locate::LocateMegaAfford(img, g_afford, error)) {
        Log(LogLevel::Error, "mega_afford", "%s; off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "mega_afford", "CanAfford +%llX, FindBestPositionForMegaStructure +%llX",
        Rva(g_afford.canAfford), Rva(g_afford.findBest));
    // CanAfford first: until FindBestPosition is hooked, t_affordDepth stays 0 and it passes through.
    auto afford =
        InstallJumpHook(g_afford.canAfford, locate::kCanAffordStolenBytes, reinterpret_cast<void*>(&CanAffordHook));
    if (!afford) {
        Log(LogLevel::Error, "mega_afford", "failed to hook CanAfford; off");
        return;
    }
    g_origCanAfford = reinterpret_cast<game::CanAffordFn>(afford);
    auto find = InstallJumpHook(g_afford.findBest, locate::kFindBestPositionStolenBytes,
                                reinterpret_cast<void*>(&FindBestHook));
    if (!find) {
        Log(LogLevel::Error, "mega_afford", "failed to hook FindBestPositionForMegaStructure; off");
        return;
    }
    g_origFindBest = reinterpret_cast<game::FindBestPositionFn>(find);
    Log(LogLevel::Info, "mega_afford", "hooks installed; CanAfford cached per search%s",
        g_affordVerify ? ", verifying every cached answer against the game's" : "");
}

#ifdef SEF_DEV_STATS
void LogAffordStats() {
    if (!g_origFindBest)
        return;
    uint64_t calls = g_affordCalls.load(), cached = g_affordCached.load();
    Log(LogLevel::Stats, "mega_afford",
        "searches=%llu calls=%llu cached=%llu cached_pct=%.1f verify=%d mismatches=%llu", g_affordScopes.load(), calls,
        cached, calls ? 100.0 * cached / calls : 0.0, g_affordVerify, g_affordMismatches.load());
}
#endif
