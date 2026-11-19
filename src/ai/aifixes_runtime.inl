// The country AI's gameplay fixes ([ai_fixes] in stellaris-engine-fixes.ini; re/ai.md, section 2),
// the AI fleet trace ([diagnostics]) and the income target shortcut (fast_income_target; re/ai.md,
// section 6). Included by dllmain.cpp inside its anonymous namespace, after the hooking helpers,
// sef_bench's tick and date code, and numericfixes_runtime.inl.
// The code is in parts this file includes: aifixes_planning.inl, aifixes_war.inl and
// aifixes_schedule.inl (the fixes, by what they change), aifleettrace_runtime.inl and
// incometarget_runtime.inl. This file keeps AiIni, the stats and InstallAiFixes.
//
// Each fix changes how AI empires play, so each is off unless its setting is 1. None changes the
// save format. They run on the AI's side only, so in multiplayer every player needs the same ones
// (untested there).
//
//   build_plan_keep        2.1  the monthly build plan lasts the month instead of one week
//   opportunist_wars       2.3  "opportunist" counts the target's wars, not our own
//   war_distance_slope     2.2  the war score's distance malus per jump above the malus distance
//   truce_war_divisor=N    2.4  a truce divides war appetite by N, not by AI_AGGRESSIVENESS_BOXED_IN_MULT
//   expansion_refresh_monthly  2.5  the colonize/expansion lists are rebuilt every month, all game
//   ai_schedule_from_date  2.8/2.9  weekly, monthly and yearly AI updates on fixed days of the calendar
//   ai_random_from_save    2.9  a loaded AI continues its saved random stream
//   build_plan_skip_unaffordable  2.16  the weekly walk passes over an item it can't order, instead of stopping
//   colonizer_idle_days=N  2.17  a colony ship idle for N days no longer stops another being built
//   constructor_idle_days=N  2.18  a construction ship without work for N days looks further away
//   survey_blacklist_monthly  2.15  systems a science ship couldn't survey are tried again after a month, not a year
//   war_plan_check         2.19  a war the game's rules forbid declaring is neither prepared nor kept in preparation
//   boss_threat_floor      2.19  a hostile boss counts for its cached fleet power when that exceeds BOSS_MILITARY_POWER

locate::AiFixLocations g_ai;
bool g_aiDefinesWanted = false; // log the defines the war distance and truce fixes use, once loaded

// A setting of [ai_fixes]; 0 (off) when it is missing.
int AiIni(HMODULE self, const char* key) {
    return IniInt(self, "ai_fixes", key, 0);
}

// The fixes, by what they change; each part has its own installers.
#include "aifixes_planning.inl"
#include "aifixes_war.inl"
#include "aifixes_schedule.inl"

#ifdef SEF_DEV_STATS
void LogAiFixStats() {
    // The defines are read after stellaris-engine-fixes starts; report them once they're there.
    if (g_aiDefinesWanted && g_ai.boxedInMult && *g_ai.boxedInMult != 0) {
        g_aiDefinesWanted = false;
        if (g_ai.malusDistance && g_ai.malus) {
            Log(LogLevel::Info, "ai_defines",
                "AI_WAR_DECLARATION_MALUS_DISTANCE %d, malus %.2f per jump; "
                "AI_AGGRESSIVENESS_BOXED_IN_MULT %d",
                *g_ai.malusDistance, *g_ai.malus / 100000.0, *g_ai.boxedInMult);
        }
    }
    if (g_origPlanWalk) {
        Log(LogLevel::Stats, "ai_build_plan", "walks=%llu blocked_deferred=%llu ordered=%llu kept=%llu faulted=%d",
            g_planWalks.load(), g_planDeferred.load(), g_planOrdered.load(), g_planKept.load(), g_planFailed.load());
    }
    if (uint64_t seen = g_colonizerSeen.load()) {
        Log(LogLevel::Stats, "ai_colony_ships", "idle_seen=%llu long_idle_skipped=%llu", seen,
            g_colonizerPassed.load());
    }
    if (uint64_t calls = g_stationCalls.load()) {
        Log(LogLevel::Stats, "ai_construction_ships", "searches=%llu widened=%llu widened_found_work=%llu", calls,
            g_stationWidened.load(), g_stationWidenedFound.load());
    }
    if (uint64_t calls = g_opportunistCalls.load()) {
        Log(LogLevel::Stats, "ai_opportunist", "checks=%llu enemies_counted=%llu faulted=%d", calls,
            g_opportunistAdded.load(), g_opportunistFailed.load());
    }
}
#endif

void InstallAiFixes(HMODULE self, const locate::Image& img) {
    bool plan = AiIni(self, "build_plan_keep") != 0;
    bool opportunist = AiIni(self, "opportunist_wars") != 0;
    bool distance = AiIni(self, "war_distance_slope") != 0;
    bool warGuard = AiIni(self, "war_plan_check") != 0;
    bool bossThreat = AiIni(self, "boss_threat_floor") != 0;
    int truce = AiIni(self, "truce_war_divisor");
    bool expansion = AiIni(self, "expansion_refresh_monthly") != 0;
    bool schedule = AiIni(self, "ai_schedule_from_date") != 0;
    bool random = AiIni(self, "ai_random_from_save") != 0;
    bool skip = AiIni(self, "build_plan_skip_unaffordable") != 0;
    int colonizer = AiIni(self, "colonizer_idle_days");
    int constructor = AiIni(self, "constructor_idle_days");
    bool blacklist = AiIni(self, "survey_blacklist_monthly") != 0;
    if (!plan && !opportunist && !distance && !warGuard && !bossThreat && truce <= 0 && !expansion && !schedule &&
        !random && !skip && colonizer <= 0 && constructor <= 0 && !blacklist) {
        Log(LogLevel::Info, "ai_fixes", "all off ([ai_fixes] in stellaris-engine-fixes.ini)");
        return;
    }
    std::string error;
    auto find = [&](const char* what, bool (*locateFn)(const locate::Image&, locate::AiFixLocations&, std::string&)) {
        if (locateFn(img, g_ai, error))
            return true;
        Log(LogLevel::Error, "ai_fixes", "AI %s: %s; off", what, error.c_str());
        return false;
    };
    if (plan && find("build plan", locate::LocateBuildPlan))
        InstallBuildPlanFix();
    if (skip && find("build plan skip", locate::LocatePlanSkip))
        InstallPlanSkipFix();
    if (colonizer > 0 && find("colony ships", locate::LocateColonizerIdle))
        InstallColonizerFix(colonizer);
    if (constructor > 0 && find("construction ships", locate::LocateStationRange))
        InstallStationRangeFix(constructor);
    if (blacklist && find("survey blacklist", locate::LocateSurveyBlacklist))
        InstallSurveyBlacklistFix();
    if (opportunist && find("opportunist", locate::LocateOpportunist))
        InstallOpportunistFix();
    if (distance && find("war distance", locate::LocateWarDistance))
        InstallWarDistanceFix();
    if (warGuard)
        InstallAiWarGuard(img);
    if (bossThreat)
        InstallAiBossThreat(img);
    if (truce > 0 && find("truce divisor", locate::LocateTruce))
        InstallTruceFix(truce);
    if (expansion && find("expansion lists", locate::LocateExpansionRefresh))
        InstallExpansionFix();
    if ((schedule || random) && find("schedule", locate::LocateAiSchedule))
        InstallAiSchedule(schedule, random);
    if (random && find("random", locate::LocateSavedRandom))
        InstallSavedRandom();
}

// The AI fleet trace ([diagnostics]) and the income target shortcut.
#include "aifleettrace_runtime.inl"
#include "incometarget_runtime.inl"
