// Part of numericfixes_runtime.inl: the fleets' [numeric_fixes] settings, fleet_power_arithmetic and
// fleet_stat_arithmetic. The fleet's power sums (fleetoverflow.h) and hull, armor and shield totals
// (fleetstats.h), the ship design's cached numbers (designcache.h), ship and component damage
// (shipdamage.h, componentmath.h), a ship's maximum stats (shipstats.h) and its power formula
// (shipformula.h).

// ---- fleet_power_arithmetic: the fleet's power sums ----

fleetoverflow::Locations g_fleetPower;
fleetoverflow::PowerFn g_originalFleetPower = nullptr;
fleetoverflow::HealthFn g_originalHealthPower = nullptr;
std::atomic<bool> g_fleetPowerFailed{false};

int64_t* __fastcall FleetPowerHook(uint8_t* fleet, int64_t* out, int mode, bool cached) {
    if (!g_fleetPowerFailed.load(std::memory_order_relaxed)) {
        __try {
            int64_t result;
            if (fleetoverflow::Compute(g_fleetPower, fleet, mode, cached, false, result)) {
                *out = result;
                return out;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!g_fleetPowerFailed.exchange(true))
            Log(LogLevel::Error, "fleet_power", "invalid state or memory fault; off from next call");
    }
    return g_originalFleetPower(fleet, out, mode, cached);
}

int64_t* __fastcall HealthPowerHook(uint8_t* fleet, int64_t* out) {
    if (!g_fleetPowerFailed.load(std::memory_order_relaxed)) {
        __try {
            int64_t result;
            if (fleetoverflow::Compute(g_fleetPower, fleet, 0, true, true, result)) {
                *out = result;
                return out;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!g_fleetPowerFailed.exchange(true))
            Log(LogLevel::Error, "fleet_power", "invalid state or memory fault; off from next call");
    }
    return g_originalHealthPower(fleet, out);
}

void InstallFleetPowerArithmetic(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "fleet_power_arithmetic")) {
        Log(LogLevel::Info, "fleet_power", "off (fleet_power_arithmetic=0)");
        return;
    }
    std::string error;
    if (!fleetoverflow::Locate(img, g_fleetPower, error)) {
        Log(LogLevel::Error, "fleet_power", "%s; off", error.c_str());
        return;
    }
    auto power =
        InstallJumpHook(g_fleetPower.power, fleetoverflow::kPowerStolen, reinterpret_cast<void*>(&FleetPowerHook));
    if (!power) {
        Log(LogLevel::Error, "fleet_power", "cannot install aggregate hook; off");
        return;
    }
    g_originalFleetPower = reinterpret_cast<fleetoverflow::PowerFn>(power);
    auto health = InstallJumpHook(g_fleetPower.healthPower, fleetoverflow::kHealthStolen,
                                  reinterpret_cast<void*>(&HealthPowerHook));
    if (!health) {
        // The existing aggregate trampoline remains valid even if restoring code fails.
        g_fleetPowerFailed.store(true);
        bool restored = WriteCode(g_fleetPower.power, power, fleetoverflow::kPowerStolen);
        Log(LogLevel::Error, "fleet_power", "cannot install health hook; off (aggregate restored=%d)",
            restored ? 1 : 0);
        return;
    }
    g_originalHealthPower = reinterpret_cast<fleetoverflow::HealthFn>(health);
    Log(LogLevel::Info, "fleet_power",
        "wide sums, ratios and products at +%llX / +%llX; ship power production unchanged", Rva(g_fleetPower.power),
        Rva(g_fleetPower.healthPower));
}

// ---- fleet_stat_arithmetic: the fleet's hull, armor and shield totals ----

fleetstats::Locations g_fleetStats;
fleetstats::HullFn g_originalHull[2]{};
fleetstats::PointsFn g_originalPoints[2]{};
std::atomic<bool> g_fleetStatsFailed{false};

void FleetStatsFault() {
    if (!g_fleetStatsFailed.exchange(true))
        Log(LogLevel::Error, "fleet_stats", "invalid state or memory fault; off from next call");
}

int64_t* FleetHullValue(uint8_t* fleet, int64_t* out, bool maximum) {
    if (!g_fleetStatsFailed.load()) {
        __try {
            int64_t a, b;
            if (fleetstats::Hull(g_fleetStats, fleet, a, b)) {
                *out = maximum ? b : a;
                return out;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        FleetStatsFault();
    }
    return g_originalHull[maximum ? 1 : 0](fleet, out);
}

int64_t* __fastcall FleetHull(uint8_t* fleet, int64_t* out) {
    return FleetHullValue(fleet, out, false);
}
int64_t* __fastcall FleetMaxHull(uint8_t* fleet, int64_t* out) {
    return FleetHullValue(fleet, out, true);
}

void FleetPointsValue(uint8_t* fleet, int64_t* current, int64_t* maximum, int type) {
    bool writing = false;
    if (!g_fleetStatsFailed.load()) {
        __try {
            int64_t a = *current, b = *maximum;
            if (fleetstats::Points(g_fleetStats, fleet, type, a, b, current == maximum)) {
                writing = true;
                *current = a;
                *maximum = b;
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        FleetStatsFault();
    }
    if (!writing)
        g_originalPoints[type](fleet, current, maximum);
}

void __fastcall FleetArmor(uint8_t* fleet, int64_t* current, int64_t* maximum) {
    FleetPointsValue(fleet, current, maximum, 0);
}
void __fastcall FleetShield(uint8_t* fleet, int64_t* current, int64_t* maximum) {
    FleetPointsValue(fleet, current, maximum, 1);
}

void InstallFleetStats(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "fleet_stat_arithmetic")) {
        Log(LogLevel::Info, "fleet_stats", "off (fleet_stat_arithmetic=0)");
        return;
    }
    std::string error;
    if (!fleetstats::Locate(img, g_fleetStats, error)) {
        Log(LogLevel::Error, "fleet_stats", "%s; off", error.c_str());
        return;
    }
    void* const functions[] = {reinterpret_cast<void*>(&FleetHull), reinterpret_cast<void*>(&FleetMaxHull),
                               reinterpret_cast<void*>(&FleetArmor), reinterpret_cast<void*>(&FleetShield)};
    JumpHook hooks[4];
    for (int i = 0; i < 4; ++i) {
        hooks[i] = {g_fleetStats.functions[i], fleetstats::kStolen[i], functions[i]};
    }
    uint8_t* original[4]{};
    if (!InstallJumpHooks(hooks, 4, original)) {
        g_fleetStatsFailed = true;
        Log(LogLevel::Error, "fleet_stats", "installation failed; off");
        return;
    }
    for (int i = 0; i < 2; ++i) {
        g_originalHull[i] = reinterpret_cast<fleetstats::HullFn>(original[i]);
        g_originalPoints[i] = reinterpret_cast<fleetstats::PointsFn>(original[i + 2]);
    }
    Log(LogLevel::Info, "fleet_stats",
        "wide hull/armor/shield totals; common scaling preserves extreme current/maximum ratios");
}

// ---- fleet_stat_arithmetic and fleet_power_arithmetic: the ship design's cached numbers ----

diploscores::Locations g_diploScores;
shipformula::Locations g_shipFormula;
shipstats::Locations g_shipStats;
componentmath::Locations g_componentMath;
designcache::Locations g_designCache;
std::atomic<bool> g_designCacheFailed{false};

// Makes the hooks and blocks pass everything to the game's own code; true the first time.
bool SwitchDesignCacheOff() {
    for (auto& block : g_designCache.blocks)
        block.failed = true;
    return !g_designCacheFailed.exchange(true);
}

void DisableDesignCache() {
    if (SwitchDesignCacheOff())
        Log(LogLevel::Error, "design_cache", "calculation fault; off from next call");
}

using DesignCalcFn = void(__fastcall*)(uint8_t*, uint8_t*);
using DesignAddFn = void(__fastcall*)(uint8_t*, int64_t*, uint32_t, void*, int);
using DesignComponentFn = void(__fastcall*)(uint8_t*, uint8_t*, int);
using DesignRecalcFn = void(__fastcall*)(uint8_t*);
using DesignModifierFn = void(__fastcall*)(uint8_t*, uint8_t*, uint32_t, void*, int);

bool CalculateDesignModifiers(uint8_t* base, uint8_t* mods, uint32_t kind, void* name, int count, bool& writing) {
    try {
        designcache::AddModifiers(g_designCache, base, mods, kind, count, [&](uint8_t* stat, int64_t value, int mode) {
            writing = true;
            reinterpret_cast<DesignAddFn>(g_designCache.originals[1])(stat, &value, kind, name, mode);
        });
        return true;
    } catch (...) {
        return false;
    }
}

void __fastcall DesignAddModifiers(uint8_t* base, uint8_t* mods, uint32_t kind, void* name, int count) {
    bool writing = false;
    if (!g_designCacheFailed.load()) {
        __try {
            if (CalculateDesignModifiers(base, mods, kind, name, count, writing))
                return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        DisableDesignCache();
    }
    if (!writing)
        reinterpret_cast<DesignModifierFn>(g_designCache.originals[5])(base, mods, kind, name, count);
}

void CalculateDesignStats(uint8_t* stats, uint8_t* record, bool& started) {
    designcache::Pass pass(stats, record);
    designcache::Scope scope(pass);
    started = true;
    reinterpret_cast<DesignCalcFn>(g_designCache.originals[0])(stats, record);
}

bool TryCalculateDesignStats(uint8_t* stats, uint8_t* record, bool& started) {
    try {
        CalculateDesignStats(stats, record, started);
        return true;
    } catch (...) {
        return false;
    }
}

void __fastcall DesignCalcStats(uint8_t* stats, uint8_t* record) {
    bool started = false;
    auto previous = designcache::current;
    auto previousComponent = designcache::component;
    auto previousSpecial = designcache::special;
    if (!g_designCacheFailed.load()) {
        __try {
            if (TryCalculateDesignStats(stats, record, started))
                return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        designcache::current = previous;
        designcache::component = previousComponent;
        designcache::special = previousSpecial;
        DisableDesignCache();
    }
    if (!started)
        reinterpret_cast<DesignCalcFn>(g_designCache.originals[0])(stats, record);
}

bool CaptureDesignValue(uint8_t* stat, int64_t value, int mode) {
    try {
        designcache::Add(g_designCache, stat, value, mode);
        return true;
    } catch (...) {
        return false;
    }
}

void __fastcall DesignAddValue(uint8_t* stat, int64_t* amount, uint32_t kind, void* name, int mode) {
    if (!g_designCacheFailed.load()) {
        bool ok = false;
        __try {
            ok = CaptureDesignValue(stat, *amount, mode);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!ok)
            DisableDesignCache();
    }
    reinterpret_cast<DesignAddFn>(g_designCache.originals[1])(stat, amount, kind, name, mode);
}

void __fastcall DesignAddComponent(uint8_t* stats, uint8_t* component, int count) {
    // A single-use override replaces the first damage contribution with its exact
    // component result. Later flat-damage modifiers remain separate contributions.
    designcache::Component value{(component[0x1e1] & 0xfd) == 0 ? stats + 0xc0 : nullptr, component};
    designcache::ComponentScope scope(value);
    reinterpret_cast<DesignComponentFn>(g_designCache.originals[2])(stats, component, count);
}

void CalculateDesignSpecial(uint8_t* design, bool& started) {
    designcache::SpecialPass pass{design};
    designcache::SpecialScope scope(pass);
    started = true;
    reinterpret_cast<DesignRecalcFn>(g_designCache.originals[3])(design);
    if (!g_designCacheFailed.load())
        designcache::PublishSpecial(pass);
}

bool TryCalculateDesignSpecial(uint8_t* design, bool& started) {
    try {
        CalculateDesignSpecial(design, started);
        return true;
    } catch (...) {
        return false;
    }
}

void __fastcall DesignRecalc(uint8_t* design) {
    bool started = false;
    auto previous = designcache::current;
    auto previousComponent = designcache::component;
    auto previousSpecial = designcache::special;
    if (!g_designCacheFailed.load()) {
        __try {
            if (TryCalculateDesignSpecial(design, started))
                return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        designcache::current = previous;
        designcache::component = previousComponent;
        designcache::special = previousSpecial;
        DisableDesignCache();
    }
    if (!started)
        reinterpret_cast<DesignRecalcFn>(g_designCache.originals[3])(design);
}

bool CalculateGroupedSources(uint8_t* stat, uint8_t* output, bool& started) {
    try {
        designcache::GroupPass pass{output};
        designcache::GroupScope scope(pass);
        started = true;
        reinterpret_cast<DesignCalcFn>(g_designCache.originals[6])(stat, output);
        return true;
    } catch (...) {
        return false;
    }
}

void __fastcall DesignGroupedSources(uint8_t* stat, uint8_t* output) {
    bool started = false;
    auto previous = designcache::group;
    if (!g_designCacheFailed.load()) {
        __try {
            if (CalculateGroupedSources(stat, output, started))
                return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        designcache::group = previous;
        DisableDesignCache();
    }
    if (!started)
        reinterpret_cast<DesignCalcFn>(g_designCache.originals[6])(stat, output);
}

void InstallDesignCache(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "fleet_stat_arithmetic") && !NumericFix(self, "fleet_power_arithmetic")) {
        Log(LogLevel::Info, "design_cache", "off (fleet_stat_arithmetic=0 and fleet_power_arithmetic=0)");
        return;
    }
    std::string error;
    if (!designcache::Locate(img, g_designCache, error)) {
        Log(LogLevel::Error, "design_cache", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.AddAll(g_designCache.blocks);
    if (!blocks.Prepare()) {
        Log(LogLevel::Error, "design_cache", "cannot prepare stubs; off");
        return;
    }
    void* const functions[] = {
        reinterpret_cast<void*>(&DesignCalcStats),    reinterpret_cast<void*>(&DesignAddValue),
        reinterpret_cast<void*>(&DesignAddComponent), reinterpret_cast<void*>(&DesignRecalc),
        reinterpret_cast<void*>(&DesignAddModifiers), reinterpret_cast<void*>(&DesignGroupedSources)};
    const int entries[] = {0, 1, 2, 3, 5, 6}; // which of designcache's functions each hook replaces
    JumpHook hooks[6];
    for (int i = 0; i < 6; ++i) {
        hooks[i] = {g_designCache.functions[entries[i]], designcache::kStolen[entries[i]], functions[i]};
    }
    uint8_t* originals[6]{};
    bool hooked = InstallJumpHooks(hooks, 6, originals);
    for (int i = 0; i < 6; ++i) {
        g_designCache.originals[entries[i]] = originals[i];
    }
    if (hooked && !blocks.Write()) {
        RemoveJumpHooks(hooks, 6, originals);
        hooked = false;
    }
    if (!hooked) {
        SwitchDesignCacheOff();
        Log(LogLevel::Error, "design_cache", "cannot install; off");
        return;
    }
    Log(LogLevel::Info, "design_cache", "wide rebuild-scoped stat terms, component totals and excess-power ratios");
}

// ---- fleet_stat_arithmetic and fleet_power_arithmetic: ship damage ----

shipdamage::Locations g_shipDamage;
std::atomic<bool> g_shipDamageFailed{false};

// SEH filter of the ship damage and ship stat hooks: logs where the fault was.
int ShipNumericFault(const char* feature, int kind, EXCEPTION_POINTERS* fault, void* subject, void* tooltip) {
    const auto& record = *fault->ExceptionRecord;
    Log(LogLevel::Error, feature,
        "fault detail: kind=%d code=%08lX instruction=%p subject=%p tooltip=%p operation=%llu address=%p", kind,
        record.ExceptionCode, record.ExceptionAddress, subject, tooltip,
        record.NumberParameters > 0 ? static_cast<unsigned long long>(record.ExceptionInformation[0]) : 0,
        record.NumberParameters > 1 ? reinterpret_cast<void*>(record.ExceptionInformation[1]) : nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// kind 0: damage; 1: component adjustments; 2: the damaged-hull penalty.
bool CalculateShipDamage(int kind, int64_t* out, uint8_t* design, uint8_t* modifiers, uint8_t* ship, bool weighted,
                         void* tooltip, int id, bool& writing) {
    try {
        auto value = (kind == 0   ? shipdamage::Damage(g_shipDamage, design, modifiers, ship, weighted)
                      : kind == 1 ? shipdamage::Adjustments(g_shipDamage, design, modifiers, weighted)
                                  : shipdamage::Penalty(g_shipDamage, ship, id))
                         .Clamp();
        // The result first: a fault in the native tooltip formatter must not leave *out unwritten.
        writing = true;
        *out = value;
        if (tooltip) {
            int64_t ignored;
            if (kind == 0) {
                reinterpret_cast<shipdamage::MainFn>(g_shipDamage.originals[0])(&ignored, design, modifiers, ship,
                                                                                weighted, tooltip);
            } else {
                reinterpret_cast<shipdamage::AdjustFn>(g_shipDamage.originals[1])(design, &ignored, modifiers, weighted,
                                                                                  tooltip);
            }
        }
        return true;
    } catch (const std::exception& e) {
        Log(LogLevel::Error, "ship_damage", "calculation failed: kind=%d %s", kind, e.what());
        return false;
    } catch (...) {
        return false;
    }
}

int64_t* ShipDamageValue(int kind, int64_t* out, uint8_t* design, uint8_t* modifiers, uint8_t* ship, bool weighted,
                         void* tooltip, int id) {
    bool writing = false;
    if (!g_shipDamageFailed.load()) {
        __try {
            if (CalculateShipDamage(kind, out, design, modifiers, ship, weighted, tooltip, id, writing))
                return out;
        } __except (ShipNumericFault("ship_damage", kind, GetExceptionInformation(), design ? design : ship, tooltip)) {
        }
        if (!g_shipDamageFailed.exchange(true))
            Log(LogLevel::Error, "ship_damage", "memory/formatting fault; off from next call");
    }
    if (writing)
        return out;
    if (kind == 0)
        return reinterpret_cast<shipdamage::MainFn>(g_shipDamage.originals[0])(out, design, modifiers, ship, weighted,
                                                                               tooltip);
    if (kind == 1)
        return reinterpret_cast<shipdamage::AdjustFn>(g_shipDamage.originals[1])(design, out, modifiers, weighted,
                                                                                 tooltip);
    return reinterpret_cast<shipdamage::PenaltyFn>(g_shipDamage.originals[2])(out, ship, id);
}

int64_t* __fastcall ShipDamage(int64_t* o, uint8_t* d, uint8_t* m, uint8_t* s, bool w, void* t) {
    return ShipDamageValue(0, o, d, m, s, w, t, 0);
}
int64_t* __fastcall ShipComponentAdjustments(uint8_t* d, int64_t* o, uint8_t* m, bool w, void* t) {
    return ShipDamageValue(1, o, d, m, nullptr, w, t, 0);
}
int64_t* __fastcall ShipDamagePenalty(int64_t* o, uint8_t* s, int id) {
    return ShipDamageValue(2, o, nullptr, nullptr, s, false, nullptr, id);
}

void InstallShipDamage(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "fleet_stat_arithmetic") && !NumericFix(self, "fleet_power_arithmetic")) {
        Log(LogLevel::Info, "ship_damage", "off (fleet_stat_arithmetic=0 and fleet_power_arithmetic=0)");
        return;
    }
    std::string error;
    if (!shipdamage::Locate(img, g_shipDamage, error)) {
        Log(LogLevel::Error, "ship_damage", "%s; off", error.c_str());
        return;
    }
    void* const functions[] = {reinterpret_cast<void*>(&ShipDamage), reinterpret_cast<void*>(&ShipComponentAdjustments),
                               reinterpret_cast<void*>(&ShipDamagePenalty)};
    JumpHook hooks[3];
    for (int i = 0; i < 3; ++i) {
        hooks[i] = {g_shipDamage.functions[i], shipdamage::kStolen[i], functions[i]};
    }
    if (!InstallJumpHooks(hooks, 3, g_shipDamage.originals)) {
        g_shipDamageFailed = true;
        Log(LogLevel::Error, "ship_damage", "cannot install; off");
        return;
    }
    Log(LogLevel::Info, "ship_damage", "exact modifier chains, component adjustments and damaged-hull penalties");
}

void InstallComponentDamage(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "fleet_stat_arithmetic") && !NumericFix(self, "fleet_power_arithmetic")) {
        Log(LogLevel::Info, "component_damage", "off (fleet_stat_arithmetic=0 and fleet_power_arithmetic=0)");
        return;
    }
    std::string error;
    if (!componentmath::Locate(img, g_componentMath, error)) {
        Log(LogLevel::Error, "component_damage", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.AddAll(g_componentMath.blocks);
    if (!InstallBlocks(blocks, "component_damage"))
        return;
    Log(LogLevel::Info, "component_damage",
        "exact weapon/strike-craft damage, range means, counts and military weights");
}

// ---- fleet_stat_arithmetic: a ship's maximum hull, armor and shield ----

std::atomic<bool> g_shipStatsFailed{false};

// kind 0: hull; 1: armor; 2: shield.
bool CalculateMaximumShipStat(int kind, int64_t* out, uint8_t* design, uint8_t* modifiers, uint8_t* fleet,
                              void* tooltip, bool& writing) {
    try {
        int64_t value = shipstats::Maximum(g_shipStats, kind, design, modifiers, fleet).Clamp();
        // Native formatting lists the unchanged base and individual modifiers. Retain its
        // localization/ownership path for the corrected final result, which is written first
        // so that a fault in the native formatter cannot leave *out unwritten.
        writing = true;
        *out = value;
        if (tooltip) {
            int64_t ignored;
            g_shipStats.originals[kind](&ignored, design, modifiers, fleet, tooltip);
        }
        return true;
    } catch (const std::exception& e) {
        Log(LogLevel::Error, "ship_stats", "calculation failed: kind=%d %s", kind, e.what());
        return false;
    } catch (...) {
        return false;
    }
}

int64_t* MaximumShipStat(int kind, int64_t* out, uint8_t* design, uint8_t* modifiers, uint8_t* fleet, void* tooltip) {
    bool writing = false;
    if (!g_shipStatsFailed.load()) {
        __try {
            if (CalculateMaximumShipStat(kind, out, design, modifiers, fleet, tooltip, writing))
                return out;
        } __except (ShipNumericFault("ship_stats", kind, GetExceptionInformation(), design, tooltip)) {
        }
        if (!g_shipStatsFailed.exchange(true))
            Log(LogLevel::Error, "ship_stats", "memory/formatting fault; off from next call");
    }
    if (!writing)
        return g_shipStats.originals[kind](out, design, modifiers, fleet, tooltip);
    return out;
}

int64_t* __fastcall MaximumShipHull(int64_t* o, uint8_t* d, uint8_t* m, uint8_t* f, void* t) {
    return MaximumShipStat(0, o, d, m, f, t);
}
int64_t* __fastcall MaximumShipArmor(int64_t* o, uint8_t* d, uint8_t* m, uint8_t* f, void* t) {
    return MaximumShipStat(1, o, d, m, f, t);
}
int64_t* __fastcall MaximumShipShield(int64_t* o, uint8_t* d, uint8_t* m, uint8_t* f, void* t) {
    return MaximumShipStat(2, o, d, m, f, t);
}

void InstallShipStats(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "fleet_stat_arithmetic", "ship_stats"))
        return;
    std::string error;
    if (!shipstats::Locate(img, g_shipStats, error)) {
        Log(LogLevel::Error, "ship_stats", "%s; off", error.c_str());
        return;
    }
    const shipstats::MaximumFn functions[] = {MaximumShipHull, MaximumShipArmor, MaximumShipShield};
    JumpHook hooks[3];
    for (int i = 0; i < 3; ++i) {
        hooks[i] = {g_shipStats.functions[i], shipstats::kStolen[i], reinterpret_cast<void*>(functions[i])};
    }
    uint8_t* originals[3]{};
    bool hooked = InstallJumpHooks(hooks, 3, originals);
    for (int i = 0; i < 3; ++i) {
        g_shipStats.originals[i] = reinterpret_cast<shipstats::MaximumFn>(originals[i]);
    }
    if (!hooked) {
        g_shipStatsFailed = true;
        Log(LogLevel::Error, "ship_stats", "cannot install; off");
        return;
    }
    Log(LogLevel::Info, "ship_stats", "exact hull/armor/shield modifiers, bioship growth and final products");
}

// ---- fleet_power_arithmetic: a ship's power formula ----

void InstallShipFormula(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "fleet_power_arithmetic", "ship_power"))
        return;
    std::string error;
    if (!shipformula::Locate(img, g_shipFormula, error)) {
        Log(LogLevel::Error, "ship_power", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.Add(g_shipFormula.block);
    if (!InstallBlocks(blocks, "ship_power"))
        return;
    Log(LogLevel::Info, "ship_power", "wide Q7 arithmetic; integer interval exponent fallback on native overflow");
}
