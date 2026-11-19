// Part of aifixes_runtime.inl: the AI's schedule and random stream (re/ai.md sections 2.8, 2.9). Weekly,
// monthly and yearly AI updates on fixed days of the calendar, and a loaded AI continuing its saved
// random stream. Uses the date field bench_runtime.inl finds.

// ---- 2.8/2.9 The AI's schedule and random stream --------------------------------------------------
//
// Each AI counts days to its weekly, monthly and yearly update from id % 7, % 30, % 360 when its
// CAICore is created, also after every load; residues 0 and 1 fall on the same day. With
// ai_schedule_from_date, before each daily update the counters are set so that the update falls
// on days where (id + day) % 7 (30, 360) == 0: the same days before and after a load, spread evenly.
//
// The AI's own (unsynced) random generator is saved, but read back only with -loadrandomseed,
// which also restores the game's synchronized one. ai_random_from_save drops the check for the
// AI's generator only. The other project's notes saw a restored AI behave as if restarted; the
// summary says which AIs, on the first day after a load, continue their saved stream.

using AiUpdateFn = uint64_t(__fastcall*)(uint8_t* core);
AiUpdateFn g_origAiUpdate = nullptr;
bool g_aiSchedule = false, g_aiRandomCheck = false;
std::atomic<int64_t> g_aiLastDay{-1}, g_aiCheckDay{-1};
std::atomic<uint32_t> g_aiFromSave{0}, g_aiRestarted{0};

void ScheduleAiDay(uint8_t* core, int64_t day) {
    uint64_t id = *reinterpret_cast<uint32_t*>(core + g_ai.coreId);
    // The game decrements, then updates when the counter is <= 0: 1 fires today, 2 doesn't.
    *reinterpret_cast<int32_t*>(core + g_ai.weekCounter) = (id + day) % 7 == 0 ? 1 : 2;
    *reinterpret_cast<int32_t*>(core + g_ai.monthCounter) = (id + day) % 30 == 0 ? 1 : 2;
    *reinterpret_cast<int32_t*>(core + g_ai.yearCounter) = (id + day) % 360 == 0 ? 1 : 2;
}

void CheckAiRandom(uint8_t* core, int64_t day) {
    int64_t last = g_aiLastDay.exchange(day);
    if (last != day) {
        if (last >= 0 && day == g_aiCheckDay.load() + 1) {
            Log(LogLevel::Info, "ai_random",
                "first AI day after start or load: %u AIs continue a saved stream, %u start "
                "at their id",
                g_aiFromSave.load(), g_aiRestarted.load());
        }
        if (last < 0 || day < last || day > last + 1) { // a load, or a new game
            g_aiFromSave = 0;
            g_aiRestarted = 0;
            g_aiCheckDay = day;
        }
    }
    if (day != g_aiCheckDay.load())
        return;
    uint32_t id = *reinterpret_cast<uint32_t*>(core + g_ai.coreId);
    bool restarted = *reinterpret_cast<uint32_t*>(core + g_ai.randomPosition) == id &&
                     *reinterpret_cast<uint32_t*>(core + g_ai.randomSeed) == id;
    (restarted ? g_aiRestarted : g_aiFromSave).fetch_add(1, std::memory_order_relaxed);
}

std::atomic<bool> g_aiUpdateFailed{false};

bool BeforeAiUpdate(uint8_t* core) {
    __try {
        uint8_t* state = g_bench.gameState ? *g_bench.gameState : nullptr;
        if (!state)
            return true;
        int64_t day = *reinterpret_cast<int32_t*>(state + g_bench.stateDate) / 24;
        if (g_aiRandomCheck)
            CheckAiRandom(core, day);
        if (g_aiSchedule)
            ScheduleAiDay(core, day);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall AiUpdateHook(uint8_t* core) {
    if (!g_aiUpdateFailed && !BeforeAiUpdate(core)) {
        g_aiUpdateFailed = true;
        Log(LogLevel::Error, "ai_schedule", "fault; off from now on");
    }
    return g_origAiUpdate(core);
}

void InstallAiSchedule(bool schedule, bool random) {
    if (!g_bench.gameState) {
        Log(LogLevel::Error, "ai_schedule", "the game date wasn't found (see the ticks line); off");
        return;
    }
    g_aiSchedule = schedule;
    g_aiRandomCheck = random;
    uint8_t* tramp =
        InstallJumpHook(g_ai.aiUpdate, locate::kAiUpdateStolenBytes, reinterpret_cast<void*>(&AiUpdateHook));
    if (!tramp) {
        Log(LogLevel::Error, "ai_schedule", "failed to hook CAICore::Update; off");
        g_aiSchedule = g_aiRandomCheck = false;
        return;
    }
    g_origAiUpdate = reinterpret_cast<AiUpdateFn>(tramp);
    if (schedule)
        Log(LogLevel::Info, "ai_schedule",
            "on; weekly, monthly and yearly updates on days where (id + day) %% 7, 30, 360 is 0");
}

void InstallSavedRandom() {
    const uint8_t nop6[6] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
    for (uint8_t* jump : g_ai.savedRandomJumps) {
        if (jump[0] != 0x0F || jump[1] != 0x84 || !WriteCode(jump, nop6, sizeof(nop6))) {
            Log(LogLevel::Warn, "ai_random", "failed to patch; a load may have restored part of it");
            return;
        }
    }
    Log(LogLevel::Info, "ai_random",
        "on; a loaded AI keeps its saved random_seed and random_count (-loadrandomseed is %s)",
        *g_ai.useSavedRandom ? "on" : "off");
}
