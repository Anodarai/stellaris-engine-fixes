// Tick pacing ([speed] in_tick_frame_ms, batch_ticks) and the console command sef_bench, which
// reads the counters of simulation_runtime.inl. The tick, date and console hooks here are also used
// by the AI schedule and script storage, which are included after this file. Included by dllmain.cpp
// inside its anonymous namespace. See re/simulation_speed.md.

// ===========================================================================
// Tick pacing and the benchmark
//
// The game simulates and draws on one thread. Two settings ([speed] in stellaris-engine-fixes.ini) give the
// simulation more of it, without changing what it computes:
//   in_tick_frame_ms  frames the game draws from inside a tick (RenderFrame, "frame smoothing")
//                     are drawn at most once per this many ms; the game draws one whenever a
//                     refresh interval has passed (about 11 ms at a 90 Hz cap).
//   batch_ticks       at the fastest speed, CheckTick queues the ticks up to the next AI tick
//                     (tick 4) or day tick (tick 10) per frame instead of NUM_COMMANDS_PER_TURN (2):
//                     2 frames per day instead of 5. Commands the AI posts on its tick, and those
//                     posted on the day tick, still run before the next tick, as with the game's 2.
//
// The console command `sef_bench [seconds]` measures for that much wall time (default 60): the
// game ticks and days that pass, and where the main thread's time goes: inside ticks
// (HandleTurnTick), in frames drawn from inside a tick, and between ticks (the normal frames: UI,
// rendering, the frame limiter). `sef_bench stop` ends a run early and `sef_bench show` prints the
// last results. A run that ends by itself prints its results by running `sef_bench show`, the way
// the game's own commands run another command (OnExecute_UnchartedSpace). `sef_bench combat
// off|on` switches skip_empty_combat_entries (above) for A/B runs.
// See re/simulation_speed.md.

struct PacingSettings {
    int inTickFrameMs = 33; // 0: the game's own rate
    bool batchTicks = true;
};
PacingSettings g_pacing;
LONGLONG g_minFrameGap = 0; // in_tick_frame_ms in QPC ticks
LONGLONG g_lastFrame = 0;   // when the last frame (normal or drawn inside a tick) started

locate::TickLocations g_bench;
game::ExecuteFn g_origExecute = nullptr;
game::CheckTickFn g_origCheckTick = nullptr;
game::TurnTickFn g_origTurnTick = nullptr;
game::RenderFrameFn g_origRenderFrame = nullptr;
DWORD g_mainThread = 0;

int g_renderDepth = 0; // RenderFrame calls in progress: a CheckTick inside one is a nested frame
bool g_inTurnTick = false;

struct BenchRun {
    bool active = false;
    double seconds = 0;
    LONGLONG start = 0, deadline = 0, lastFrame = 0;
    int32_t speed = -1; // the game speed index at the run's first frame
    bool speedChanged = false;
    bool smoothingAtStart = false;
    uint64_t ticks = 0, days = 0;
    uint64_t frames = 0;                           // normal frames, not paused
    uint64_t insideFrames = 0, betweenFrames = 0;  // drawn by RenderFrame inside a tick, or between commands
    uint64_t skippedFrames = 0;                    // RenderFrame calls skipped by in_tick_frame_ms
    uint64_t batches = 0;                          // CheckTick calls that queued a batch of ticks
    uint64_t intelSearches = 0, intelFast = 0;     // the fleet intel counters at the start
    uint64_t combatInserts = 0, combatSkipped = 0; // the combat map counters at the start
    LONGLONG tickTime = 0, insideTime = 0, betweenTime = 0, pausedTime = 0, longestTick = 0;
    int32_t firstDate = 0, lastDate = 0, longestTickDate = 0;
    bool haveDate = false;
};
BenchRun g_run;
std::string g_benchReport; // the last run's results
bool g_benchPrintPending = false;

// One run requested by the external benchmark runner. Its clock starts and stops on day boundaries.
enum class AutoBenchPhase { Off, Waiting, Warmup, Measuring, Finished };
struct AutoBench {
    AutoBenchPhase phase = AutoBenchPhase::Off;
    locate::BenchControls controls;
    int days = 0;
    int warmupDays = 0;
    int32_t saveDate = 0;
    int speed = 4; // zero-based game speed
    uint8_t* state = nullptr;
    LONGLONG end = 0;
};
AutoBench g_autoBench;

double Seconds(LONGLONG t) {
    return static_cast<double>(t) / g_qpcFreq.QuadPart;
}

bool ReadInt(const uint8_t* p, int32_t* out) {
    __try {
        *out = *reinterpret_cast<const int32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void StartBench(LONGLONG now, double seconds) {
    g_run = BenchRun{};
    g_run.active = true;
    g_run.seconds = seconds;
    g_run.start = g_run.lastFrame = now;
    if (seconds > 0)
        g_run.deadline = now + static_cast<LONGLONG>(seconds * g_qpcFreq.QuadPart);
    g_run.smoothingAtStart = *g_bench.frameSmoothing != 0;
    g_run.intelSearches = g_intelSearches.load();
    g_run.intelFast = g_intelFast.load();
    g_run.combatInserts = g_combatInserts;
    g_run.combatSkipped = g_combatSkipped;
}

void FailAutoBench(const char* reason) {
    g_run.active = false;
    g_autoBench.phase = AutoBenchPhase::Off;
    Log(LogLevel::Error, "sef_auto_bench", "%s", reason);
}

// Sets a flag for the duration of a call, also when an exception passes through.
struct FlagScope {
    bool& flag;
    explicit FlagScope(bool& f) : flag(f) { flag = true; }
    ~FlagScope() { flag = false; }
};
struct DepthScope {
    int& depth;
    explicit DepthScope(int& d) : depth(d) { ++depth; }
    ~DepthScope() { --depth; }
};

void __fastcall TurnTickHook(uint8_t* state, void* checksums) {
    if (GetCurrentThreadId() != g_mainThread || g_inTurnTick) { // neither happens in the game
        g_origTurnTick(state, checksums);
        return;
    }
    const bool measure = g_run.active;
    const bool warming = g_autoBench.phase == AutoBenchPhase::Warmup && state == g_autoBench.state;
    int32_t before = 0;
    const bool dated = (measure || warming) && ReadInt(state + g_bench.stateDate, &before);
    LONGLONG t0 = measure ? Now() : 0;
    {
        FlagScope inTick(g_inTurnTick);
        g_origTurnTick(state, checksums);
    }
    if (warming) {
        int32_t after = 0;
        const int32_t start = g_autoBench.saveDate + 24 * g_autoBench.warmupDays;
        if (!dated || !ReadInt(state + g_bench.stateDate, &after) || after > start) {
            FailAutoBench("missed the warm-up end date");
        } else if (after == start && after > before) {
            StartBench(Now(), 0.0);
            g_run.firstDate = g_run.lastDate = after;
            g_run.haveDate = true;
            g_run.speed = g_autoBench.speed;
            g_autoBench.phase = AutoBenchPhase::Measuring;
        }
        return;
    }
    if (!measure || !g_run.active)
        return; // `sef_bench stop` can run in a frame drawn during the tick
    const LONGLONG now = Now();
    const LONGLONG t = now - t0;
    ++g_run.ticks;
    g_run.tickTime += t;
    int32_t after = 0;
    if (dated && ReadInt(state + g_bench.stateDate, &after)) {
        if (!g_run.haveDate) {
            g_run.firstDate = before;
            g_run.haveDate = true;
        }
        g_run.lastDate = after;
        if (after > before)
            g_run.days += (after - before) / 24;
        if (t > g_run.longestTick) {
            g_run.longestTick = t;
            g_run.longestTickDate = after;
        }
        if (g_autoBench.phase == AutoBenchPhase::Measuring && g_run.days >= static_cast<uint64_t>(g_autoBench.days)) {
            const int32_t end = g_autoBench.saveDate + 24 * (g_autoBench.warmupDays + g_autoBench.days);
            if (state != g_autoBench.state || after != end || g_run.days != static_cast<uint64_t>(g_autoBench.days)) {
                FailAutoBench("missed the measurement end date");
            } else {
                g_run.active = false; // no work beyond the last day is counted
                g_autoBench.end = now;
                g_autoBench.phase = AutoBenchPhase::Finished;
            }
        }
    }
}

void __fastcall RenderFrameHook(void* a, void* b, void* c, void* d) {
    if (GetCurrentThreadId() != g_mainThread) {
        g_origRenderFrame(a, b, c, d);
        return;
    }
    LONGLONG t0 = Now();
    if (g_renderDepth == 0) {
        // Skipping the frame is what the game does when its own check says no frame is due yet.
        if (g_inTurnTick && g_minFrameGap > 0 && t0 - g_lastFrame < g_minFrameGap) {
            if (g_run.active)
                ++g_run.skippedFrames;
            return;
        }
        g_lastFrame = t0;
    }
    bool measure = g_run.active && g_renderDepth == 0;
    {
        DepthScope depth(g_renderDepth);
        g_origRenderFrame(a, b, c, d);
    }
    if (!measure || !g_run.active)
        return;
    LONGLONG t = Now() - t0;
    if (g_inTurnTick) {
        ++g_run.insideFrames;
        g_run.insideTime += t;
    } else {
        ++g_run.betweenFrames;
        g_run.betweenTime += t;
    }
}

void AddLine(std::string& out, const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(line, fmt, args);
    va_end(args);
    if (!out.empty())
        out += '\n';
    out += line;
}

// Ends the run and writes its results to g_benchReport and the log.
void FinishBench(LONGLONG now) {
    BenchRun& r = g_run;
    r.active = false;
    double wall = Seconds(now - r.start);
    double paused = (std::min)(Seconds(r.pausedTime), wall);
    double active = (std::max)(wall - paused, 1e-6);
    double inTicks = Seconds(r.tickTime), inside = Seconds(r.insideTime), between = Seconds(r.betweenTime);
    double sim = (std::max)(inTicks - inside, 0.0);
    double rest = (std::max)(active - inTicks, 0.0); // between ticks, frames drawn between commands included
    double normal = (std::max)(rest - between, 0.0); // the normal frames
    auto pct = [&](double s) { return 100.0 * s / active; };
    double days = static_cast<double>(r.days);
    bool smoothing = *g_bench.frameSmoothing != 0;
    float turnTime = r.speed >= 0 && r.speed < 8 ? g_bench.speedTurnTimes[r.speed] : -1.0f;

    std::string out;
    char pausedNote[64] = "";
    if (paused >= 0.05)
        sprintf_s(pausedNote, " (%.1f s paused, not counted)", paused);
    char speedNote[48] = "";
    if (turnTime == 0.0f) {
        strcpy_s(speedNote, " (no wait between ticks)");
    } else if (turnTime > 0.0f) {
        sprintf_s(speedNote, " (%.3f s between batches of ticks)", turnTime);
    }
    AddLine(out, "sef_bench: %.1f s%s, game speed %d%s, frame smoothing %s", active, pausedNote, r.speed + 1, speedNote,
            smoothing ? "on" : "off");
    char throttleNote[96] = "the game's rate";
    if (g_pacing.inTickFrameMs > 0) {
        sprintf_s(throttleNote, "at most one per %d ms (%llu skipped)", g_pacing.inTickFrameMs, r.skippedFrames);
    }
    char intelNote[96] = "off";
    if (g_intelOn && devstats::kOn) {
        sprintf_s(intelNote, "on%s (%llu searches, %llu lookups)", g_intelVerify ? ", verifying" : "",
                  g_intelSearches.load() - r.intelSearches, g_intelFast.load() - r.intelFast);
    } else if (g_intelOn) {
        sprintf_s(intelNote, "on%s", g_intelVerify ? ", verifying" : ""); // counts need a dev build
    }
    AddLine(out, "stellaris-engine-fixes: frames inside ticks %s; tick batching %s (%llu batches); fast fleet intel %s",
            throttleNote, g_pacing.batchTicks ? "on" : "off", r.batches, intelNote);
    if (g_origCombatInsert && devstats::kOn) {
        AddLine(out, "stellaris-engine-fixes: skip empty combat entries %s (%llu inserts, %llu skipped)",
                g_combatWanted ? "on" : "off", g_combatInserts - r.combatInserts, g_combatSkipped - r.combatSkipped);
    } else if (g_origCombatInsert) {
        AddLine(out, "stellaris-engine-fixes: skip empty combat entries %s", g_combatWanted ? "on" : "off");
    }
    if (r.days == 0) {
        AddLine(out, "%llu ticks, no full day passed: was the game paused?", r.ticks);
    } else {
        AddLine(out, "%s to %s: %llu days, %llu ticks; %.2f days per second, %.0f ms per day",
                FormatDate(r.firstDate).c_str(), FormatDate(r.lastDate).c_str(), r.days, r.ticks, days / active,
                1e3 * active / days);
    }
    AddLine(
        out,
        "in ticks %.1f s (%.0f%%): simulation %.1f s (%.0f%%), %llu frames drawn inside ticks %.1f s (%.0f%%, %.1f ms each)",
        inTicks, pct(inTicks), sim, pct(sim), r.insideFrames, inside, pct(inside),
        r.insideFrames ? 1e3 * inside / r.insideFrames : 0.0);
    char betweenNote[96] = "";
    if (r.betweenFrames) {
        sprintf_s(betweenNote, ", plus %llu frames drawn between commands %.1f s", r.betweenFrames, between);
    }
    AddLine(out, "between ticks %.1f s (%.0f%%): %llu frames, %.1f ms each, %.1f ticks per frame%s", rest, pct(rest),
            r.frames, r.frames ? 1e3 * normal / r.frames : 0.0,
            r.frames ? static_cast<double>(r.ticks) / r.frames : 0.0, betweenNote);
    if (r.days) {
        AddLine(
            out,
            "per day: simulation %.0f ms + frames inside ticks %.0f ms + between ticks %.0f ms; %.1f frames per day",
            1e3 * sim / days, 1e3 * inside / days, 1e3 * rest / days,
            static_cast<double>(r.frames + r.insideFrames + r.betweenFrames) / days);
    }
    if (r.ticks) {
        AddLine(out, "longest tick %.0f ms (%s)", 1e3 * Seconds(r.longestTick), FormatDate(r.longestTickDate).c_str());
    }
    if (r.speedChanged)
        AddLine(out, "the game speed changed during the run");
    if (smoothing != r.smoothingAtStart)
        AddLine(out, "frame smoothing was switched during the run");
    Log(LogLevel::Stats, "sef_bench",
        "wall_s=%.3f active_s=%.3f paused_s=%.3f speed=%d turn_time_s=%.3f "
        "frame_smoothing=%d speed_changed=%d smoothing_changed=%d in_tick_frame_ms=%d skipped_frames=%llu "
        "batch_ticks=%d batches=%llu intel_on=%d intel_verify=%d intel_searches=%llu intel_fast_lookups=%llu "
        "combat_skip_on=%d combat_inserts=%llu combat_skipped=%llu days=%llu ticks=%llu days_per_s=%.3f ms_per_day=%.3f "
        "first_date=%s last_date=%s simulation_s=%.3f in_tick_s=%.3f in_tick_frames=%llu in_tick_frame_s=%.3f "
        "between_tick_s=%.3f between_tick_frames=%llu between_command_frames=%llu between_command_frame_s=%.3f "
        "longest_tick_ms=%.3f longest_tick_date=%s",
        wall, active, paused, r.speed + 1, turnTime, smoothing, r.speedChanged, smoothing != r.smoothingAtStart,
        g_pacing.inTickFrameMs, r.skippedFrames, g_pacing.batchTicks, r.batches, g_intelOn.load(), g_intelVerify,
        g_intelSearches.load() - r.intelSearches, g_intelFast.load() - r.intelFast, g_combatWanted,
        g_combatInserts - r.combatInserts, g_combatSkipped - r.combatSkipped, r.days, r.ticks, days / active,
        r.days ? 1e3 * active / days : 0.0, r.days ? FormatDate(r.firstDate).c_str() : "none",
        r.days ? FormatDate(r.lastDate).c_str() : "none", sim, inTicks, r.insideFrames, inside, rest, r.frames,
        r.betweenFrames, between, 1e3 * Seconds(r.longestTick),
        r.ticks ? FormatDate(r.longestTickDate).c_str() : "none");
    if (!r.days)
        Log(LogLevel::Warn, "sef_bench", "no full game day elapsed");
    if (r.speedChanged || smoothing != r.smoothingAtStart) {
        Log(LogLevel::Warn, "sef_bench", "game speed or frame smoothing changed during the run");
    }
    g_benchReport = out;
}

// Runs one of stellaris-engine-fixes' own commands so its reply reaches the console, without recording it
// in the console's history or console_history.txt.
template <size_t N> void RunOwnCommand(const char (&text)[N]) {
    static_assert(N <= 16, "fits the inline buffer");
    uint8_t* console = *g_bench.console;
    if (!console)
        return;
    game::String line = game::Borrow(text);
    uint8_t history = *g_bench.addToHistory;
    *g_bench.addToHistory = 0;
    reinterpret_cast<game::RunCommandFn>(g_bench.runCommand)(console, &line);
    *g_bench.addToHistory = history;
}

void PrintBenchReport() {
    RunOwnCommand("sef_bench show");
}

bool SetBenchPaused(uint8_t* idler, bool paused) {
    // SPauseGameSettings starts with the engine's CString, followed by the requested state and source.
    struct PauseSettings {
        game::String who;
        uint8_t paused;
        uint8_t source;
    } settings{game::Borrow(""), static_cast<uint8_t>(paused), 2};
    __try {
        reinterpret_cast<void (*)(uint8_t*, const PauseSettings*)>(g_autoBench.controls.setPaused)(idler, &settings);
        return (idler[g_bench.idlerPaused] != 0) == paused;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AutoBenchFrame(uint8_t* idler) {
    const auto phase = g_autoBench.phase;
    auto* state = *g_bench.gameState;
    if (phase == AutoBenchPhase::Waiting) {
        // CheckTick itself waits for this ready byte before it will queue any game commands.
        if (!state || !state[0xA0])
            return;
        wchar_t profileLog[MAX_PATH];
        constexpr wchar_t suffix[] = L"\\logs\\system.log";
        const DWORD length = GetEnvironmentVariableW(L"SEF_BENCH_USERDIR", profileLog, _countof(profileLog));
        if (!length || length + _countof(suffix) > _countof(profileLog)) {
            FailAutoBench("missing or overlong benchmark profile path");
            return;
        }
        memcpy(profileLog + length, suffix, sizeof(suffix));
        if (GetFileAttributesW(profileLog) == INVALID_FILE_ATTRIBUTES) {
            FailAutoBench("the redirected benchmark profile has no system.log");
            return;
        }
        int32_t date = 0;
        if (!ReadInt(state + g_bench.stateDate, &date) || date != g_autoBench.saveDate ||
            idler[g_bench.idlerMultiplayer] || !idler[g_bench.idlerPaused]) {
            FailAutoBench("expected the requested save, paused in single player");
            return;
        }
        const auto ready = GetTickCount64();
        reinterpret_cast<void (*)(uint8_t*, int)>(g_autoBench.controls.setSpeed)(idler, g_autoBench.speed);
        if (!SetBenchPaused(idler, false)) {
            FailAutoBench("the game's pause setter did not unpause the save");
            return;
        }
        g_autoBench.state = state;
        g_autoBench.phase = AutoBenchPhase::Warmup;
        Log(LogLevel::Info, "sef_auto_bench", "ready_tick_ms=%llu save_date=%s", ready, FormatDate(date).c_str());
        return;
    }
    if (state != g_autoBench.state || idler[g_bench.idlerMultiplayer]) {
        FailAutoBench("the loaded game changed during the benchmark");
        return;
    }
    int32_t speed = -1;
    if (!ReadInt(idler + g_bench.idlerSpeed, &speed) || speed != g_autoBench.speed || idler[g_bench.idlerPaused]) {
        FailAutoBench("the game paused or its speed changed during the benchmark");
        return;
    }
    if (phase == AutoBenchPhase::Finished) {
        FinishBench(g_autoBench.end);
        if (!SetBenchPaused(idler, true)) {
            FailAutoBench("the game's pause setter did not pause after the benchmark");
            return;
        }
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        const bool haveMemory =
            K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), memory.cb);
        g_autoBench.phase = AutoBenchPhase::Off;
        Log(LogLevel::Stats, "sef_auto_bench", "complete=1 peak_working_set_bytes=%llu peak_commit_bytes=%llu",
            haveMemory ? static_cast<unsigned long long>(memory.PeakWorkingSetSize) : 0,
            haveMemory ? static_cast<unsigned long long>(memory.PeakPagefileUsage) : 0);
    }
}

// Once per normal frame (not for frames drawn from inside the simulation).
void BenchFrame(uint8_t* idler) {
    if (g_run.active) {
        LONGLONG now = Now();
        int32_t speed = -1;
        ReadInt(idler + g_bench.idlerSpeed, &speed);
        if (idler[g_bench.idlerPaused]) {
            g_run.pausedTime += now - g_run.lastFrame;
        } else {
            ++g_run.frames;
        }
        if (g_run.speed < 0) {
            g_run.speed = speed;
        } else if (speed != g_run.speed) {
            g_run.speedChanged = true;
        }
        g_run.lastFrame = now;
        if (g_run.deadline && now >= g_run.deadline) {
            FinishBench(now);
            g_benchPrintPending = true;
        }
    }
    if (g_benchPrintPending) {
        g_benchPrintPending = false;
        PrintBenchReport();
    }
}

// The ticks to queue this frame with batch_ticks, or 0 to leave the game's count: at the fastest
// speed in single player, the ticks up to and including the next AI tick (4) or day tick (10).
int TickBatch(uint8_t* idler) {
    if (!g_pacing.batchTicks || idler[g_bench.idlerMultiplayer] || *g_bench.ticksPerTurn != 1)
        return 0;
    int32_t speed = -1, tick = -1;
    if (!ReadInt(idler + g_bench.idlerSpeed, &speed) || speed < 0 || speed > 15)
        return 0;
    if (g_bench.speedTurnTimes[speed] != 0.0f)
        return 0; // only the fastest speed has no wait
    uint8_t* state = *g_bench.gameState;
    if (!state || !ReadInt(state + g_bench.stateTick, &tick) || tick < 0 || tick > 9)
        return 0;
    return tick < 4 ? 4 - tick : 10 - tick; // the tick count is 1..10 after a tick's increment
}

uint64_t __fastcall CheckTickHook(uint8_t* idler, float frameSeconds) {
    if (g_renderDepth != 0 || GetCurrentThreadId() != g_mainThread)
        return g_origCheckTick(idler, frameSeconds);
    g_lastFrame = Now();
    if (g_autoBench.phase != AutoBenchPhase::Off)
        AutoBenchFrame(idler);
    BenchFrame(idler);
    StoreFrame();
    StarbaseTemplateFrame();
    ShipDesignLibraryFrame();
    int batch = TickBatch(idler);
    if (batch == 0)
        return g_origCheckTick(idler, frameSeconds);
    // CheckTick queues max(min(NUM_COMMANDS_PER_TURN, 10), g_nTicksPerTurn) ticks; the define goes
    // back to its value right after, so nothing else sees the change.
    int32_t defined = *g_bench.commandsPerTurn;
    *g_bench.commandsPerTurn = batch;
    uint64_t queued = g_origCheckTick(idler, frameSeconds);
    *g_bench.commandsPerTurn = defined;
    if ((queued & 0xFF) && g_run.active)
        ++g_run.batches;
    return queued;
}

// The reply to `sef_bench <arg>`; false makes the console draw it in red.
bool BenchCommand(const std::string& arg, std::string& reply) {
    if (_stricmp(arg.c_str(), "show") == 0) {
        reply = g_benchReport.empty() ? "sef_bench: no results yet" : g_benchReport;
        return !g_benchReport.empty();
    }
    if (_strnicmp(arg.c_str(), "combat", 6) == 0) {
        std::string value = arg.size() > 6 && arg[6] == ' ' ? arg.substr(7) : std::string();
        if (!g_origCombatInsert) {
            reply = "sef_bench: skipping empty combat entries is not installed (see stellaris-engine-fixes.ini)";
            return false;
        }
        if (_stricmp(value.c_str(), "on") == 0 || _stricmp(value.c_str(), "off") == 0) {
            g_combatWanted = _stricmp(value.c_str(), "on") == 0;
            Log(LogLevel::Info, "sef_bench", "skip empty combat entries %s", g_combatWanted ? "on" : "off");
        } else if (!value.empty()) {
            reply = "usage: sef_bench combat [on | off]";
            return false;
        }
        reply = std::string("sef_bench: skip empty combat entries ") + (g_combatWanted ? "on" : "off") +
                (g_run.active ? " (a run is in progress: its results mix both)" : "");
        return true;
    }
    if (_stricmp(arg.c_str(), "stop") == 0) {
        if (!g_run.active) {
            reply = "sef_bench: not running";
            return false;
        }
        FinishBench(Now());
        reply = g_benchReport;
        return true;
    }
    if (g_run.active) {
        char buf[160];
        sprintf_s(buf, "sef_bench: running, %.0f s left, %llu days so far (sef_bench stop ends it now)",
                  (std::max)(0.0, Seconds(g_run.deadline - Now())), g_run.days);
        reply = buf;
        return true;
    }
    double seconds = 60;
    if (!arg.empty()) {
        char* end = nullptr;
        seconds = strtod(arg.c_str(), &end);
        if (*end != '\0' || seconds < 5 || seconds > 3600) {
            reply = "usage: sef_bench [seconds, 5 to 3600, default 60] | sef_bench stop | sef_bench show | "
                    "sef_bench combat [on | off]";
            return false;
        }
    }
    StartBench(Now(), seconds);
    Log(LogLevel::Info, "sef_bench", "started, %.0f s", seconds);
    char buf[200];
    sprintf_s(buf,
              "sef_bench: measuring for %.0f s; the results are printed here when it ends. "
              "Leave the game running at the speed to measure.",
              seconds);
    reply = buf;
    return true;
}

bool SameText(const game::String& s, const char* text) {
    size_t n = strlen(text);
    return s.size == n && _strnicmp(s.data(), text, n) == 0;
}

game::ConsoleResult* Reply(game::ConsoleResult* out, bool ok, const std::string& text) {
    memset(out, 0, sizeof(*out));
    out->ok = ok;
    game::String& s = out->text;
    if (text.size() <= 0xF) {
        memcpy(s.buf, text.data(), text.size());
        s.cap = 0xF;
    } else {
        // Kind 1: the game never frees it. Not freed here either: a few hundred bytes per command.
        char* p = new char[text.size() + 1];
        memcpy(p, text.c_str(), text.size() + 1);
        s.kind = 1;
        s.ptr = p;
        s.cap = text.size();
    }
    s.size = text.size();
    return out;
}

game::ConsoleResult* __fastcall ExecuteHook(void* manager, game::ConsoleResult* out, const uint8_t* args) {
    auto items = *reinterpret_cast<game::String* const*>(args + game::kArrayDataOffset);
    int count = *reinterpret_cast<const int*>(args + game::kArrayCountOffset);
    if (count < 1 || !items)
        return g_origExecute(manager, out, args);
    if (!SameText(items[0], "sef_bench"))
        return g_origExecute(manager, out, args);
    std::string arg;
    for (int i = 1; i < count; ++i) {
        if (i > 1)
            arg += ' ';
        arg.append(items[i].data(), items[i].size);
    }
    std::string reply;
    if (static_cast<uint8_t*>(manager)[g_bench.consoleLockedFlag]) {
        return Reply(out, false, "sef_bench: not available in multiplayer or ironman");
    }
    bool ok = BenchCommand(arg, reply);
    return Reply(out, ok, reply);
}

// [speed] of stellaris-engine-fixes.ini next to this DLL; the defaults when it is missing.
void ReadPacingSettings(HMODULE self) {
    int ms = IniInt(self, "speed", "in_tick_frame_ms", g_pacing.inTickFrameMs);
    g_pacing.inTickFrameMs = (std::max)(0, (std::min)(ms, 1000));
    g_pacing.batchTicks = IniOn(self, "speed", "batch_ticks", g_pacing.batchTicks);
}

void ArmAutoBench(HMODULE self, const locate::Image& img) {
    g_autoBench.days = IniInt(self, "benchmark", "days", 0);
    if (g_autoBench.days == 0)
        return;
    g_autoBench.warmupDays = IniInt(self, "benchmark", "warmup_days", 0);
    g_autoBench.saveDate = IniInt(self, "benchmark", "save_date_hours", 0);
    if (g_autoBench.days < 1 || g_autoBench.days > 3600 || g_autoBench.warmupDays < 1 ||
        g_autoBench.warmupDays > 360 || g_autoBench.saveDate < 43800000 || g_autoBench.saveDate > INT_MAX - 24 * 3960) {
        FailAutoBench("invalid benchmark days, warm-up days or save date");
        return;
    }
    std::string error;
    if (!locate::LocateBenchControls(img, g_bench, g_autoBench.controls, error)) {
        FailAutoBench(error.c_str());
        return;
    }
    g_autoBench.phase = AutoBenchPhase::Waiting;
    Log(LogLevel::Info, "sef_auto_bench", "armed days=%d warmup_days=%d", g_autoBench.days, g_autoBench.warmupDays);
}

void InstallBench(HMODULE self, const locate::Image& img) {
    std::string error;
    if (!locate::LocateTicks(img, g_bench, error)) {
        Log(LogLevel::Error, "ticks", "%s; tick pacing and the sef_bench command are off", error.c_str());
        return;
    }
    ReadPacingSettings(self);
    Log(LogLevel::Debug, "ticks",
        "idler speed +0x%zx, paused +0x%zx, multiplayer +0x%zx; game state tick +0x%zx, date +0x%zx; console lock flag +0x%zx",
        g_bench.idlerSpeed, g_bench.idlerPaused, g_bench.idlerMultiplayer, g_bench.stateTick, g_bench.stateDate,
        g_bench.consoleLockedFlag);
    g_mainThread = GetCurrentThreadId(); // DllMain runs on the game's main thread
    struct Hook {
        const char* what;
        uint8_t* target;
        size_t stolen;
        void* hook;
        void** orig;
    } hooks[] = {
        // The console command last: until it is in place nothing starts a run.
        {"RenderFrame", g_bench.renderFrame, locate::kRenderFrameStolenBytes, reinterpret_cast<void*>(&RenderFrameHook),
         reinterpret_cast<void**>(&g_origRenderFrame)},
        {"HandleTurnTick", g_bench.turnTick, locate::kTurnTickStolenBytes, reinterpret_cast<void*>(&TurnTickHook),
         reinterpret_cast<void**>(&g_origTurnTick)},
        {"CheckTick", g_bench.checkTick, locate::kCheckTickStolenBytes, reinterpret_cast<void*>(&CheckTickHook),
         reinterpret_cast<void**>(&g_origCheckTick)},
        {"CConsoleCmdManager::Execute", g_bench.execute, locate::kConsoleExecuteStolenBytes,
         reinterpret_cast<void*>(&ExecuteHook), reinterpret_cast<void**>(&g_origExecute)},
    };
    for (Hook& h : hooks) {
        uint8_t* tramp = InstallJumpHook(h.target, h.stolen, h.hook);
        if (!tramp) {
            // The hooks already in place pass everything through with pacing off.
            Log(LogLevel::Error, "ticks", "failed to hook %s; tick pacing and the sef_bench command are off", h.what);
            g_pacing = PacingSettings{0, false};
            return;
        }
        *h.orig = tramp;
    }
    g_minFrameGap = g_qpcFreq.QuadPart * g_pacing.inTickFrameMs / 1000;
    ArmAutoBench(self, img);
    Log(LogLevel::Info, "ticks",
        "hooks installed; frames inside ticks %s%d ms, tick batching at the fastest speed %s; console command sef_bench",
        g_pacing.inTickFrameMs ? "at most one per " : "at the game's rate, ", g_pacing.inTickFrameMs,
        g_pacing.batchTicks ? "on" : "off");
}
