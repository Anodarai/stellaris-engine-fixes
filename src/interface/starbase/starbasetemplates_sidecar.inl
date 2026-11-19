// Part of scriptstorage_runtime.inl: the starbase features' per-save state, which lives in the save
// store's sidecar (re/starbase_templates.md). The template library and staged jobs, defense platform
// auto-upgrade's schedule and the shipyards left out of reinforcement, with the keys they are stored
// under, and capturing them into the store before a save and restoring them after a load. Included
// here, not with the starbase runtime, because the store's save and load hooks call it.

// Template state shares the successful-save sidecar lifecycle. Keep store ->
// template as the lock order; UI/planner code must release template before flushing.
std::mutex g_starbaseTemplateLock;
starbasetemplates::Controller g_starbaseTemplates;
starbasetemplates::Controller g_starbaseGlobalLibrary, g_starbaseLocalLibrary;
uint64_t g_starbaseSaveGeneration = 0;
// Caller holds the template lock. Save-specific names override global names.
void RebuildStarbaseLibrary() {
    g_starbaseTemplates.library = g_starbaseGlobalLibrary.library;
    for (const auto& entry : g_starbaseLocalLibrary.library)
        g_starbaseTemplates.library[entry.first] = entry.second;
}
starbasetemplates::NativeReader g_starbaseTemplateReader;
std::atomic<bool> g_starbaseTemplatesOn{false};
// Defense platform auto-upgrade, per save; guarded by the template lock. Off when its command
// wasn't found or its saved schedule couldn't be read (the sidecar's value is then kept).
starbaseautoupgrade::Schedule g_autoUpgrade;
std::atomic<bool> g_autoUpgradeOn{false};
std::map<uint32_t, bool> g_autoUpgradeLast; // this session: whether the last run posted the command
// Shipyards left out of automatic fleet reinforcement, per save. Reinforcement is also worked out
// on AI threads, so the set has its own lock, taken after the template lock. Off when its hook
// wasn't installed or its saved set couldn't be read (the sidecar's value is then kept).
shipyardreinforce::Exclusions g_noReinforce;
std::mutex g_noReinforceLock;
std::atomic<size_t> g_noReinforceCount{0};
std::atomic<bool> g_reinforceOn{false};
bool StoreMultiplayer();
constexpr char kStarbaseLibraryKey[] = "sef.starbase.templates.v1";
constexpr char kStarbaseJobsKey[] = "sef.starbase.jobs.v1";
constexpr char kAutoUpgradeKey[] = "sef.starbase.autoupgrade.v1";
constexpr char kNoReinforceKey[] = "sef.starbase.noreinforce.v1";

// Called with g_storeLock held. A refused serialization leaves existing data intact.
bool CaptureStarbaseTemplates() {
    if (!g_starbaseTemplatesOn || StoreMultiplayer())
        return true;
    std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
    pdxstore::Store global = g_globalStore, save = g_saveStore;
    if (!global.Set(kStarbaseLibraryKey, g_starbaseGlobalLibrary.Serialize(true)))
        return false;
    if (g_starbaseLocalLibrary.library.empty())
        save.Remove(kStarbaseLibraryKey);
    else if (!save.Set(kStarbaseLibraryKey, g_starbaseLocalLibrary.Serialize(true)))
        return false;
    if (g_starbaseTemplates.jobs.empty())
        save.Remove(kStarbaseJobsKey);
    else if (!save.Set(kStarbaseJobsKey, g_starbaseTemplates.Serialize(false)))
        return false;
    if (g_autoUpgradeOn) {
        if (g_autoUpgrade.bases.empty())
            save.Remove(kAutoUpgradeKey);
        else if (!save.Set(kAutoUpgradeKey, g_autoUpgrade.Serialize()))
            return false;
    }
    if (g_reinforceOn) {
        std::lock_guard<std::mutex> reinforceLock(g_noReinforceLock);
        if (g_noReinforce.bases.empty())
            save.Remove(kNoReinforceKey);
        else if (!save.Set(kNoReinforceKey, g_noReinforce.Serialize()))
            return false;
    }
    g_globalStore = std::move(global);
    g_saveStore = std::move(save);
    return true;
}

// Called with g_storeLock held. Never carry jobs or outstanding posts across loads.
void RestoreStarbaseTemplateJobs() {
    std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
    ++g_starbaseSaveGeneration;
    g_starbaseLocalLibrary.library.clear();
    RebuildStarbaseLibrary();
    g_starbaseTemplates.jobs.clear();
    g_starbaseTemplates.inFlight.clear();
    g_autoUpgrade.bases.clear();
    g_autoUpgradeLast.clear();
    {
        std::lock_guard<std::mutex> reinforceLock(g_noReinforceLock);
        g_noReinforce.bases.clear();
        g_noReinforceCount = 0;
    }
    if (!g_starbaseTemplatesOn || StoreMultiplayer())
        return;
    const auto* schedule = g_autoUpgradeOn ? g_saveStore.Get(kAutoUpgradeKey) : nullptr;
    if (schedule && !g_autoUpgrade.Deserialize(*schedule)) {
        g_autoUpgradeOn = false;
        Log(LogLevel::Error, "starbase_autoupgrade", "invalid saved schedule; auto-upgrade off to preserve sidecar");
    }
    if (const auto* excluded = g_reinforceOn ? g_saveStore.Get(kNoReinforceKey) : nullptr) {
        std::lock_guard<std::mutex> reinforceLock(g_noReinforceLock);
        if (!g_noReinforce.Deserialize(*excluded)) {
            g_reinforceOn = false;
            Log(LogLevel::Error, "shipyard_reinforce", "invalid saved shipyard list; checkbox off to preserve sidecar");
        }
        g_noReinforceCount = g_noReinforce.bases.size();
    }
    const auto* library = g_saveStore.Get(kStarbaseLibraryKey);
    if (library && !g_starbaseLocalLibrary.Deserialize(*library, true)) {
        g_starbaseTemplatesOn = false;
        Log(LogLevel::Error, "starbase_templates", "invalid save-specific library; disabled to preserve sidecar");
        return;
    }
    RebuildStarbaseLibrary();
    const auto* payload = g_saveStore.Get(kStarbaseJobsKey);
    if (payload && !g_starbaseTemplates.Deserialize(*payload, false)) {
        g_starbaseTemplatesOn = false;
        Log(LogLevel::Error, "starbase_templates", "invalid saved plan data; disabled to preserve sidecar");
    }
}
