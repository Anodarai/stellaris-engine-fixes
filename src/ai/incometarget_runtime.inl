// Part of aifixes_runtime.inl: fast_income_target (incometarget.h; re/ai.md section 6). Counts the
// rounds of CAIEconomicPlan::CalculateIncomeTarget's scaling loop instead of running them.

// ---- The AI's income targets ----
//
// CAIEconomicPlan::CalculateIncomeTarget (re/ai.md section 6) adds the income of the plan's
// scaling subplans to the total, round after round, until some resource's total passes the target
// the forecast set, and counts the rounds (r13d; later rounds also scale the optional subplans).
// There is no limit: a scaling subplan that adds 5 of a resource whose target is 6 billion needs
// a billion rounds, half an hour of the main thread at 700,000 rounds a second, every AI month.
//
// Every round is the same: the same subplans, scope and target, so the same income R is added.
// After round 1 the total is T0 + R, and the round count k that the game would reach is the first
// at which T0 + k * R passes the target in some resource. When round 2 starts, the stub below
// sets the total to T0 + (k - 1) * R and the counter to k - 1; round 2 then adds R and counts
// itself, and the game's own check ends the loop at k, with the total and count the game's
// rounds would have given.
//
// [speed] fast_income_target=0 turns this off; income_target_verify=1 also lets rounds 2 and 3
// run and checks they add the same R before skipping the rest.

locate::IncomeTargetLocations g_income;
bool g_incomeOn = false;
bool g_incomeVerify = false;
std::atomic<bool> g_incomeFailed{false};
devstats::Counter g_incomeLoops{0}, g_incomeSkips{0}, g_incomeBails{0};
std::atomic<uint64_t> g_incomeMismatches{0}; // limits the verify log
devstats::Counter g_incomeRoundsSkipped{0}, g_incomeMaxRounds{0};

constexpr int kIncomeMaxResources = 64;
struct IncomeLoop {
    uintptr_t frame; // the CalculateIncomeTarget activation's rsp
    int count;
    int64_t t0[kIncomeMaxResources]; // the total before round 1
    int64_t r[kIncomeMaxResources];  // what round 1 added
};
// The loops in progress on this thread, outermost first (CalculateIncomeTarget recurses into
// subplans, and the AI runs on several threads).
thread_local IncomeLoop t_incomeLoops[16];
thread_local int t_incomeDepth = 0;

// A CPdxArray<int64> behind a CFixedResourceTable*: data at +0, count at +0xc (the loop's reads).
int64_t* ResourceData(const uint8_t* table, int* count) {
    const uint8_t* inner = *reinterpret_cast<uint8_t* const*>(table);
    *count = *reinterpret_cast<const int*>(inner + 0xc);
    return *reinterpret_cast<int64_t* const*>(inner);
}

int IncomeRound(uint8_t* total, uint8_t* target, int round, uintptr_t frame) {
    IncomeLoop* loops = t_incomeLoops;
    int& depth = t_incomeDepth;
    // Loops below this frame have returned; a loop at this frame starting over is a new one.
    while (depth > 0 && (loops[depth - 1].frame < frame || (round == 0 && loops[depth - 1].frame == frame)))
        --depth;
    int n = 0, nt = 0;
    int64_t* tot = ResourceData(total, &n);
    const int64_t* tgt = ResourceData(target, &nt);
    if (round == 0) {
        if (depth == _countof(t_incomeLoops) || n != nt || n <= 0 || n > kIncomeMaxResources)
            return round;
        IncomeLoop& l = loops[depth++];
        l.frame = frame;
        l.count = n;
        memcpy(l.t0, tot, n * sizeof(int64_t));
        return round;
    }
    if (depth == 0 || loops[depth - 1].frame != frame)
        return round;
    IncomeLoop& l = loops[depth - 1];
    if (l.count != n || n != nt) {
        --depth;
        return round;
    }
    if (round == 1) {
        g_incomeLoops.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            if (income::Difference(tot[i], l.t0[i], l.r[i]))
                continue;
            --depth;
            g_incomeBails.fetch_add(1, std::memory_order_relaxed);
            return round;
        }
    } else {
        // Verifying: rounds after the first must have added the same.
        for (int i = 0; i < n; ++i) {
            int64_t expected;
            if (!income::Total(l.t0[i], l.r[i], round, expected)) {
                --depth;
                g_incomeBails.fetch_add(1, std::memory_order_relaxed);
                return round;
            }
            if (tot[i] == expected)
                continue;
            if (g_incomeMismatches.fetch_add(1) < 20) {
                Log(LogLevel::Error, "ai_income_targets", "MISMATCH: round %d, resource %d: total %lld, expected %lld",
                    round, i, static_cast<long long>(tot[i]), static_cast<long long>(expected));
            }
            --depth;
            return round;
        }
    }
    if (g_incomeVerify && round < 3)
        return round;
    --depth;
    int64_t k = income::Rounds(l.t0, l.r, tgt, n, round);
    if (k < 0) {
        g_incomeBails.fetch_add(1, std::memory_order_relaxed);
        return round;
    }
    if (k == static_cast<int64_t>(round) + 1)
        return round; // this round is the last anyway
    // Compute every result before changing the table. No partial update on a failed check.
    int64_t next[kIncomeMaxResources];
    for (int i = 0; i < n; ++i) {
        if (income::Total(l.t0[i], l.r[i], k - 1, next[i]))
            continue;
        g_incomeBails.fetch_add(1, std::memory_order_relaxed);
        return round;
    }
    memcpy(tot, next, n * sizeof(int64_t));
    g_incomeSkips.fetch_add(1, std::memory_order_relaxed);
    g_incomeRoundsSkipped.fetch_add(static_cast<uint64_t>(k - 1 - round), std::memory_order_relaxed);
    uint64_t max = g_incomeMaxRounds.load(std::memory_order_relaxed);
    while (static_cast<uint64_t>(k) > max && !g_incomeMaxRounds.compare_exchange_weak(max, static_cast<uint64_t>(k))) {
    }
    return static_cast<int>(k - 1);
}

// Called by the stub with the caller's rbx (total), r12 (target), r13d (rounds so far) and rsp;
// returns the new r13d.
int __fastcall IncomeRoundHook(uint8_t* total, uint8_t* target, int round, uintptr_t frame) {
    if (g_incomeFailed)
        return round;
    __try {
        return IncomeRound(total, target, round, frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_incomeFailed = true;
        return round;
    }
}

void InstallIncomeTarget(HMODULE self, const locate::Image& img) {
    if (g_incomeArithmeticInstalled) {
        Log(LogLevel::Info, "ai_income_targets", "covered by exact income_target_arithmetic");
        return;
    }
    if (!IniOn(self, "speed", "fast_income_target", true)) {
        Log(LogLevel::Info, "ai_income_targets", "off (fast_income_target=0)");
        return;
    }
    g_incomeVerify = IniOn(self, "speed", "income_target_verify", false);
    std::string error;
    if (!locate::LocateIncomeTarget(img, g_income, error)) {
        Log(LogLevel::Error, "ai_income_targets", "%s; off", error.c_str());
        return;
    }
    // The redirected call lands here: rsp is 8 below a 16-byte boundary, as at any call.
    Code c;
    c.Put({0x51, 0x52, 0x41, 0x50});       // push rcx / push rdx / push r8 (CalcScalingSubplans' arguments)
    c.Put({0x48, 0x83, 0xEC, 0x20});       // sub rsp, 20h (now 16-aligned)
    c.Put({0x48, 0x89, 0xD9});             // mov rcx, rbx  (total)
    c.Put({0x4C, 0x89, 0xE2});             // mov rdx, r12  (target)
    c.Put({0x45, 0x89, 0xE8});             // mov r8d, r13d (rounds)
    c.Put({0x4C, 0x8D, 0x4C, 0x24, 0x40}); // lea r9, [rsp+40h] (the caller's rsp)
    c.Put({0x48, 0xB8}).Put64(reinterpret_cast<uint64_t>(&IncomeRoundHook)); // mov rax, hook
    c.Put({0xFF, 0xD0});                                                     // call rax
    c.Put({0x41, 0x89, 0xC5});                                               // mov r13d, eax
    c.Put({0x48, 0x83, 0xC4, 0x20});                                         // add rsp, 20h
    c.Put({0x41, 0x58, 0x5A, 0x59});                                         // pop r8 / pop rdx / pop rcx
    c.JumpTo(g_income.calcScaling); // on to CalcScalingSubplans, which returns to the game
    uint8_t* stub = PlaceStub(c, g_income.scalingCall);
    int32_t rel;
    if (!stub || !Rel32(g_income.scalingCall + 5, stub, &rel) || !WriteCode(g_income.scalingCall + 1, &rel, 4)) {
        Log(LogLevel::Error, "ai_income_targets", "no memory near the game's code; off");
        return;
    }
    g_incomeOn = true;
    Log(LogLevel::Info, "ai_income_targets",
        "CalculateIncomeTarget's scaling loop +%llX, CalcScalingSubplans +%llX; rounds after the "
        "first are counted, not run%s",
        Rva(g_income.loop), Rva(g_income.calcScaling), g_incomeVerify ? " (verifying rounds 2 and 3)" : "");
}

#ifdef SEF_DEV_STATS
void LogIncomeStats() {
    if (!g_incomeOn)
        return;
    Log(LogLevel::Stats, "ai_income_targets",
        "loops_past_round_one=%llu shortened=%llu rounds_skipped=%llu "
        "max_rounds_skipped=%llu native_loops=%llu faulted=%d verify=%d mismatches=%llu",
        g_incomeLoops.load(), g_incomeSkips.load(), g_incomeRoundsSkipped.load(), g_incomeMaxRounds.load(),
        g_incomeBails.load(), g_incomeFailed.load(), g_incomeVerify, g_incomeMismatches.load());
}
#endif
