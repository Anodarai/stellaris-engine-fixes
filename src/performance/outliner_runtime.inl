// The outliner's sorting: fleets by name (a hook on the comparator the game's stable sort calls)
// and megastructures by name (the group has no sort of its own; the hook replays its entries in
// order). Names are compared as natsort.h does. Included by dllmain.cpp inside its anonymous
// namespace, after the hooking helpers.

// ===========================================================================
// Fleets

locate::Locations g_fleet;
game::CompareFn g_origCompare = nullptr;
game::FleetGroupUpdateFn g_origFleetUpdate = nullptr;

// Copies the fleet's display name into `out`. Returns the length, -1 on a fault, or -2 if the
// fleet's name function is not the one the outliner's code calls (unexpected game version).
// Kept free of C++ objects with destructors so it can use SEH.
int FleetNameRaw(uint8_t* fleet, char* out, size_t outSize) {
    __try {
        void* sub = fleet + g_fleet.fleetNameSubobject;
        auto vtbl = *reinterpret_cast<uint8_t***>(sub);
        uint8_t* fn = vtbl[g_fleet.fleetNameVtblOffset / sizeof(void*)];
        if (!locate::IsFleetNameThunk(g_str, fn))
            return -2;
        game::String s{};
        s.cap = 0xF;
        reinterpret_cast<game::GetNameFn>(fn)(sub, &s);
        return TakeString(s, out, outSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Oldest construction date among the fleet's ships (INT_MAX if it has none), or -1 on a fault.
int64_t FleetAgeRaw(uint8_t* fleet) {
    __try {
        auto ships = *reinterpret_cast<uint32_t**>(fleet + g_fleet.fleetShipsOffset);
        int count = *reinterpret_cast<int*>(fleet + g_fleet.fleetShipCountOffset);
        int32_t oldest = INT_MAX;
        for (int i = 0; i < count; ++i) {
            uint8_t* ship = ResolveRef(*g_fleet.shipDb, ships[i], g_fleet.shipIdOffset);
            if (!ship)
                continue;
            int32_t date = *reinterpret_cast<int32_t*>(ship + g_fleet.shipDateOffset);
            if (date < oldest)
                oldest = date;
        }
        return oldest;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

struct FleetKey {
    std::string name; // markup stripped
    int32_t oldestShip = INT_MAX;
};

// A game read faulted: fall back to the original order for good. Takes effect at the next
// update, since switching comparators mid-sort would break the sort's ordering guarantees
// (the game's inlined insertion sort relies on them to stay inside the array).
bool g_fleetFailed = false;
std::unordered_map<uint32_t, FleetKey> g_fleetKeys; // for one outliner update
bool g_inFleetPass = false;                         // inside a fleet group update that uses FleetLess
devstats::MainCounter g_fleetKeyBuilds = 0;
uint64_t g_fleetCompares = 0; // also tells whether a pass sorted anything
std::vector<uint32_t> g_fleetPassIds;
int g_fleetOrdersLogged = 0;
devstats::Timer g_fleetTiming;

FleetKey BuildFleetKey(uint32_t id) {
    FleetKey key;
    uint8_t* fleet = ResolveRef(*g_fleet.fleetDb, id, g_fleet.fleetIdOffset);
    if (!fleet)
        return key; // vanished fleets sort first, consistently
    char buf[512];
    int n = FleetNameRaw(fleet, buf, sizeof(buf));
    int64_t age = FleetAgeRaw(fleet);
    if (n < 0 || age < 0) {
        Log(LogLevel::Error, "fleets", "%s reading fleet %08X (name=%d age=%lld); fleets fall back to the game's order",
            n == -2 ? "unexpected name function" : "fault", id, n, age);
        g_fleetFailed = true;
        return key;
    }
    key.name = natsort::StripMarkup(std::string_view(buf, n));
    key.oldestShip = static_cast<int32_t>(age);
    ++g_fleetKeyBuilds;
    return key;
}

const FleetKey& FleetKeyFor(uint32_t id) {
    auto it = g_fleetKeys.find(id);
    if (it == g_fleetKeys.end())
        it = g_fleetKeys.emplace(id, BuildFleetKey(id)).first;
    return it->second;
}

bool FleetLess(uint32_t a, uint32_t b) {
    const FleetKey& ka = FleetKeyFor(a);
    const FleetKey& kb = FleetKeyFor(b);
    int c = natsort::Compare(ka.name, kb.name);
    if (c != 0)
        return c < 0;
    return ka.oldestShip < kb.oldestShip; // equal: false, so the stable sort keeps ID order
}

bool __fastcall CompareHook(void* cmp, uint32_t a, uint32_t b) {
    if (!g_inFleetPass)
        return g_origCompare(cmp, a, b);
    ++g_fleetCompares;
    if (g_fleetOrdersLogged < 1) {
        g_fleetPassIds.push_back(a);
        g_fleetPassIds.push_back(b);
    }
    return FleetLess(a, b);
}

void LogFleetOrder(void* group) {
    std::sort(g_fleetPassIds.begin(), g_fleetPassIds.end());
    g_fleetPassIds.erase(std::unique(g_fleetPassIds.begin(), g_fleetPassIds.end()), g_fleetPassIds.end());
    std::stable_sort(g_fleetPassIds.begin(), g_fleetPassIds.end(), FleetLess);
    Log(LogLevel::Debug, "fleets", "fleet order for group %p (%zu fleets):", group, g_fleetPassIds.size());
    for (uint32_t id : g_fleetPassIds) {
        const FleetKey& k = FleetKeyFor(id);
        Log(LogLevel::Debug, "fleets", "  %08X  %-40s oldest ship %s", id, k.name.c_str(),
            FormatDate(k.oldestShip).c_str());
    }
}

int __fastcall FleetGroupUpdateHook(void* group, bool countOnly) {
    g_fleetKeys.clear();
    g_fleetPassIds.clear();
    uint64_t comparesBefore = g_fleetCompares;
    LONGLONG t0 = devstats::Ticks();
    g_inFleetPass = !g_fleetFailed;
    int result = g_origFleetUpdate(group, countOnly);
    g_inFleetPass = false;
    LONGLONG t1 = devstats::Ticks();

    if (g_fleetCompares > comparesBefore) {
        g_fleetTiming.Add(t1 - t0);
        if (g_fleetOrdersLogged < 1 && !g_fleetFailed) {
            ++g_fleetOrdersLogged;
            LogFleetOrder(group);
        }
    }
    MaybeLogSummary();
    return result;
}

void InstallFleets(const locate::Image& img) {
    std::string error;
    if (!locate::Locate(img, g_fleet, error)) {
        Log(LogLevel::Error, "fleets", "%s; fleet sorting is off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "fleets",
        "fleet db +%llX (id at +0x%zx), ship db +%llX (id at +0x%zx), ships at +0x%zx/+0x%zx, "
        "name via +0x%zx vtable +0x%zx, ship construction date at +0x%zx",
        Rva(g_fleet.fleetDb), g_fleet.fleetIdOffset, Rva(g_fleet.shipDb), g_fleet.shipIdOffset,
        g_fleet.fleetShipsOffset, g_fleet.fleetShipCountOffset, g_fleet.fleetNameSubobject, g_fleet.fleetNameVtblOffset,
        g_fleet.shipDateOffset);
    g_origFleetUpdate = reinterpret_cast<game::FleetGroupUpdateFn>(g_fleet.fleetGroupUpdate);
    auto tramp = InstallJumpHook(g_fleet.compare, locate::kCompareStolenBytes, reinterpret_cast<void*>(&CompareHook));
    if (!tramp) {
        Log(LogLevel::Error, "fleets", "failed to hook SSortFleets::Compare; fleet sorting is off");
        return;
    }
    g_origCompare = reinterpret_cast<game::CompareFn>(tramp);
    if (!SwapVtableSlot(g_fleet.fleetGroupVtblSlot, g_fleet.fleetGroupUpdate,
                        reinterpret_cast<void*>(&FleetGroupUpdateHook))) {
        // The comparator hook alone passes everything through to the original.
        Log(LogLevel::Error, "fleets", "failed to hook COutlinerGroupFleet::UpdateInternal; fleet sorting is off");
        return;
    }
    Log(LogLevel::Info, "fleets", "hooks installed");
}

// ===========================================================================
// Megastructures
//
// The group has no sort: UpdateInternal walks the country's megastructure list and calls
// UpdateEntry(entry i, mega) for each one it shows. The hook runs UpdateInternal twice: first
// recording the megastructures it shows (UpdateEntry skipped), then again handing UpdateEntry
// the same megastructures sorted by name. The game decides what is shown both times and
// nothing in the game state is changed.

using MegaEntryUpdateFn = void(__fastcall*)(void* entry, uint8_t* mega);
using TypeGetNameFn = game::String*(__fastcall*)(uint8_t* type, game::String* out);
using BuildStringFn = game::String*(__fastcall*)(uint8_t* name, game::String* out);

locate::MegaLocations g_mega;
game::FleetGroupUpdateFn g_origMegaUpdate = nullptr;
MegaEntryUpdateFn g_origMegaEntry = nullptr;

enum class MegaMode { Off, Record, Replay };
MegaMode g_megaMode = MegaMode::Off;
std::vector<uint8_t*> g_megaShown;  // Record: what the game shows, in its order
std::vector<uint8_t*> g_megaSorted; // Replay: the same, sorted
size_t g_megaReplayIndex = 0;
bool g_megaFailed = false;
devstats::MainCounter g_megaNameBuilds = 0;
uint64_t g_megaMismatches = 0;
int g_megaOrdersLogged = 0;
devstats::Timer g_megaTiming;

struct MegaName {
    std::string name; // markup stripped
    uint32_t typeId = 0;
    ULONGLONG built = 0;
};
std::unordered_map<uint32_t, MegaName> g_megaNames; // by megastructure id, refreshed every second
constexpr ULONGLONG kMegaNameTtlMs = 1000;

// The megastructure's name as the outliner shows it (its own name, or its type's), or -1 on a fault.
int MegaNameRaw(uint8_t* mega, char* out, size_t outSize) {
    __try {
        game::String s{};
        s.cap = 0xF;
        if (*reinterpret_cast<void**>(mega + g_mega.megaCustomNameFlag) != nullptr) {
            reinterpret_cast<BuildStringFn>(g_str.buildString)(mega + g_mega.megaCustomName, &s);
        } else {
            uint8_t* type = *reinterpret_cast<uint8_t**>(mega + g_mega.megaTypeOffset);
            reinterpret_cast<TypeGetNameFn>(g_mega.typeGetName)(type, &s);
        }
        return TakeString(s, out, outSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const std::string* MegaNameFor(uint8_t* mega, ULONGLONG now) {
    uint32_t id = *reinterpret_cast<uint32_t*>(mega + g_mega.megaIdOffset);
    uint8_t* type = *reinterpret_cast<uint8_t**>(mega + g_mega.megaTypeOffset);
    uint32_t typeId = type ? *reinterpret_cast<uint32_t*>(type + g_mega.typeIdOffset) : 0;
    MegaName& cached = g_megaNames[id];
    if (cached.built == 0 || cached.typeId != typeId || now - cached.built >= kMegaNameTtlMs) {
        char buf[512];
        int n = MegaNameRaw(mega, buf, sizeof(buf));
        if (n < 0)
            return nullptr;
        cached.name = natsort::StripMarkup(std::string_view(buf, n));
        cached.typeId = typeId;
        cached.built = now;
        ++g_megaNameBuilds;
    }
    return &cached.name;
}

void __fastcall MegaEntryHook(void* entry, uint8_t* mega) {
    switch (g_megaMode) {
    case MegaMode::Record:
        g_megaShown.push_back(mega);
        return; // the replay updates every entry
    case MegaMode::Replay:
        if (g_megaReplayIndex < g_megaSorted.size())
            mega = g_megaSorted[g_megaReplayIndex];
        ++g_megaReplayIndex;
        break;
    case MegaMode::Off:
        break;
    }
    g_origMegaEntry(entry, mega);
}

// False if a name could not be read.
bool SortMegastructures(const std::vector<uint8_t*>& shown, std::vector<uint8_t*>& sorted) {
    ULONGLONG now = GetTickCount64();
    std::vector<std::pair<const std::string*, uint8_t*>> keyed;
    keyed.reserve(shown.size());
    for (uint8_t* mega : shown) {
        const std::string* name = MegaNameFor(mega, now);
        if (!name)
            return false;
        keyed.emplace_back(name, mega);
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        return natsort::Compare(*a.first, *b.first) < 0; // equal names keep the game's order
    });
    sorted.clear();
    sorted.reserve(keyed.size());
    for (const auto& k : keyed)
        sorted.push_back(k.second);
    return true;
}

int __fastcall MegaGroupUpdateHook(void* group, bool countOnly) {
    if (countOnly || g_megaFailed)
        return g_origMegaUpdate(group, countOnly);

    LONGLONG t0 = devstats::Ticks();
    g_megaShown.clear();
    g_megaMode = MegaMode::Record;
    int shown = g_origMegaUpdate(group, false);
    g_megaMode = MegaMode::Off;

    if (!SortMegastructures(g_megaShown, g_megaSorted)) {
        Log(LogLevel::Error, "megastructures",
            "fault reading a megastructure name; megastructures fall back to the game's order");
        g_megaFailed = true;
        return g_origMegaUpdate(group, false);
    }

    g_megaReplayIndex = 0;
    g_megaMode = MegaMode::Replay;
    int replayed = g_origMegaUpdate(group, false);
    g_megaMode = MegaMode::Off;

    if (replayed != shown || g_megaReplayIndex != g_megaSorted.size()) {
        // Should not happen within one frame; the entries were still all updated (extra ones
        // with the game's own megastructure), so just count it.
        if (g_megaMismatches++ < 3) {
            Log(LogLevel::Error, "megastructures", "replay showed %d (%zu entries) but the first run showed %d",
                replayed, g_megaReplayIndex, shown);
        }
    }
    g_megaTiming.AddSince(t0);

    if (g_megaOrdersLogged < 1 && !g_megaSorted.empty()) {
        ++g_megaOrdersLogged;
        Log(LogLevel::Debug, "megastructures", "megastructure order for group %p (%zu shown):", group,
            g_megaSorted.size());
        ULONGLONG now = GetTickCount64();
        for (uint8_t* mega : g_megaSorted) {
            const std::string* name = MegaNameFor(mega, now);
            Log(LogLevel::Debug, "megastructures", "  %08X  %s",
                *reinterpret_cast<uint32_t*>(mega + g_mega.megaIdOffset), name ? name->c_str() : "?");
        }
    }
    MaybeLogSummary();
    return replayed;
}

void InstallMegastructures(const locate::Image& img) {
    std::string error;
    if (!locate::LocateMega(img, g_mega, error)) {
        Log(LogLevel::Error, "megastructures", "%s; megastructure sorting is off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "megastructures",
        "db +%llX (id at +0x%zx), type at +0x%zx (id at +0x%zx), own name at +0x%zx if +0x%zx is set",
        Rva(g_mega.megaDb), g_mega.megaIdOffset, g_mega.megaTypeOffset, g_mega.typeIdOffset, g_mega.megaCustomName,
        g_mega.megaCustomNameFlag);
    g_origMegaUpdate = reinterpret_cast<game::FleetGroupUpdateFn>(g_mega.groupUpdate);
    auto tramp =
        InstallJumpHook(g_mega.entryUpdate, locate::kMegaEntryStolenBytes, reinterpret_cast<void*>(&MegaEntryHook));
    if (!tramp) {
        Log(LogLevel::Error, "megastructures", "failed to hook UpdateEntry; megastructure sorting is off");
        return;
    }
    g_origMegaEntry = reinterpret_cast<MegaEntryUpdateFn>(tramp); // with mode Off the hook passes through
    if (!SwapVtableSlot(g_mega.groupVtblSlot, g_mega.groupUpdate, reinterpret_cast<void*>(&MegaGroupUpdateHook))) {
        Log(LogLevel::Error, "megastructures",
            "failed to hook COutlinerGroupMegastructure::UpdateInternal; megastructure sorting is off");
        return;
    }
    Log(LogLevel::Info, "megastructures", "hooks installed");
}

#ifdef SEF_DEV_STATS
// Totals since the start, like the counters next to them.
void LogOutlinerStats() {
    const auto fleets = g_fleetTiming.Total(), megas = g_megaTiming.Total();
    Log(LogLevel::Stats, "fleets", "sorts=%llu compares=%llu keys=%llu avg_us=%.1f max_us=%.1f faulted=%d",
        fleets.calls, g_fleetCompares, g_fleetKeyBuilds, fleets.AvgUs(), fleets.maxUs, g_fleetFailed);
    Log(LogLevel::Stats, "megastructures", "sorts=%llu names=%llu avg_us=%.1f max_us=%.1f faulted=%d", megas.calls,
        g_megaNameBuilds, megas.AvgUs(), megas.maxUs, g_megaFailed);
}
#endif
