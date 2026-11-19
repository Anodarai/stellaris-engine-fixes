// Part of aifixes_runtime.inl: the fixes to the AI's war decisions (re/ai.md sections 2.2-2.4,
// 2.19). The opportunist pass, the war distance malus, the truce divisor, wars the game's rules
// forbid declaring (aiwarguard.h) and hostile bosses' threat (aibossthreat.h).

// ---- 2.3 The opportunist pass ------------------------------------------------------------------
//
// IsReadyForWarWith compares our side's fleet power with the target's. With the opportunist trait
// and a target at war, the game adds half the power of one side of each of OUR wars to us; at
// peace, which PrepareWar requires, that is nothing. The fix adds half the power of the target's
// enemies in each of THE TARGET's wars (except us): they are busy fighting it.

int64_t HalfPower(const uint8_t* country) {
    // The game's rounding: (power << 15) / 65536, towards zero.
    int64_t v = static_cast<int64_t>(
        static_cast<uint64_t>(*reinterpret_cast<const int64_t*>(country + g_ai.countryPower)) << 15);
    return (v + (v < 0 ? 0xFFFF : 0)) >> 16;
}

std::atomic<bool> g_opportunistFailed{false};
devstats::Counter g_opportunistCalls{0}, g_opportunistAdded{0};

int64_t __fastcall OpportunistPower(uint8_t* our, uint8_t* target) {
    if (g_opportunistFailed)
        return 0;
    __try {
        const uint8_t* personality = *reinterpret_cast<uint8_t**>(our + g_ai.countryPersonality);
        int wars = *reinterpret_cast<int32_t*>(target + g_ai.countryWarCount);
        if (!personality || !personality[g_ai.personalityOpportunist] || wars <= 0)
            return 0;
        g_opportunistCalls.fetch_add(1, std::memory_order_relaxed);
        const uint32_t* warIds = *reinterpret_cast<uint32_t**>(target + g_ai.countryWars);
        uint32_t targetId = *reinterpret_cast<uint32_t*>(target + g_ai.countryId);
        uint32_t ourId = *reinterpret_cast<uint32_t*>(our + g_ai.countryId);
        int64_t sum = 0;
        for (int w = 0; w < wars; ++w) {
            uint8_t* war = ResolveRef(*g_ai.warDb, warIds[w], g_ai.warId);
            if (!war)
                continue;
            auto members = [&](size_t side, int* n) {
                *n = *reinterpret_cast<int32_t*>(war + side + 0x14);
                return *reinterpret_cast<uint8_t**>(war + side + 8);
            };
            int n;
            uint8_t* attackers = members(g_ai.warAttackers, &n);
            bool targetAttacks = false;
            for (int i = 0; i < n && !targetAttacks; ++i) {
                targetAttacks =
                    *reinterpret_cast<uint32_t*>(attackers + i * g_ai.memberStride + g_ai.memberId) == targetId;
            }
            uint8_t* enemies = members(targetAttacks ? g_ai.warDefenders : g_ai.warAttackers, &n);
            for (int i = 0; i < n; ++i) {
                uint32_t id = *reinterpret_cast<uint32_t*>(enemies + i * g_ai.memberStride + g_ai.memberId);
                if (id == ourId)
                    continue;
                uint8_t* country = ResolveRef(*g_ai.countryDb, id, g_ai.countryId);
                sum += HalfPower(country ? country : *g_ai.nullCountry);
            }
        }
        if (sum)
            g_opportunistAdded.fetch_add(1, std::memory_order_relaxed);
        return sum;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_opportunistFailed = true;
        return 0;
    }
}

void InstallOpportunistFix() {
    // Replaces the pass: rdi = OpportunistPower(r14, [rbp+target]), the registers the game reads
    // after the join as it leaves them, then on to the join.
    Code c;
    c.Put({0x4C, 0x89, 0xF1});                                               // mov rcx, r14
    c.Put({0x48, 0x8B, 0x95}).Put32(static_cast<uint32_t>(g_ai.targetSlot)); // mov rdx, [rbp+target]
    c.Put({0x48, 0x83, 0xEC, 0x20}); // sub rsp, 20h (the body keeps rsp 16-aligned)
    c.Put({0x48, 0xB8}).Put64(reinterpret_cast<uint64_t>(&OpportunistPower)); // mov rax, fn
    c.Put({0xFF, 0xD0});                                                      // call rax
    c.Put({0x48, 0x83, 0xC4, 0x20});                                          // add rsp, 20h
    c.Put({0x48, 0x89, 0xC7});                                                // mov rdi, rax
    c.Put({0x4C, 0x8B, 0x9D}).Put32(static_cast<uint32_t>(g_ai.targetSlot));  // mov r11, [rbp+target]
    c.Put({0x49, 0xB9}).Put64(reinterpret_cast<uint64_t>(g_ai.countryDb));    // mov r9, &countryDb
    c.Put({0x4D, 0x8B, 0x09});                                                // mov r9, [r9]
    c.Put({0x49, 0xBA}).Put64(reinterpret_cast<uint64_t>(g_ai.nullWar));      // mov r10, &nullWar
    c.Put({0x4D, 0x8B, 0x12});                                                // mov r10, [r10]
    c.JumpTo(g_ai.opportunistJoin);
    uint8_t* stub = PlaceStub(c, nullptr);
    if (!stub) {
        Log(LogLevel::Error, "ai_opportunist", "no memory for the stub; off");
        return;
    }
    Code jump;
    jump.JumpTo(stub);
    uint8_t patch[locate::kOpportunistStolenBytes];
    memset(patch, 0x90, sizeof(patch));
    memcpy(patch, jump.bytes, jump.n);
    if (!WriteCode(g_ai.opportunist, patch, sizeof(patch))) {
        Log(LogLevel::Error, "ai_opportunist", "failed to patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_opportunist", "on; opportunist AIs count the enemies of a target at war");
}

// ---- 2.2 The war distance malus ----------------------------------------------------------------
//
// factor = 1 - d * MALUS, for d > MALUS_DISTANCE, at least MINIMUM_SCORE: already the minimum one
// jump past the distance. The define's comment says "per jump above the malus distance", so the
// rewrite multiplies d - MALUS_DISTANCE instead:
//   mov eax,ebp; sub eax,[DISTANCE]; jle skip; imul rax,[MALUS]; nop    (19 bytes, as before)

void InstallWarDistanceFix() {
    uint8_t* p = g_ai.warDistance;
    Code c;
    int32_t relDistance = 0, relMalus = 0;
    ptrdiff_t skip = g_ai.malusSkip - (p + 10);
    bool fits = Rel32(p + 8, g_ai.malusDistance, &relDistance) && Rel32(p + 18, g_ai.malus, &relMalus) &&
                skip >= -128 && skip <= 127;
    c.Put({0x89, 0xE8});                                                    // mov eax, ebp
    c.Put({0x2B, 0x05}).Put32(static_cast<uint32_t>(relDistance));          // sub eax, [MALUS_DISTANCE]
    c.Put({0x7E, static_cast<uint8_t>(skip)});                              // jle skip
    c.Put({0x48, 0x0F, 0xAF, 0x05}).Put32(static_cast<uint32_t>(relMalus)); // imul rax, [MALUS]
    c.Put({0x90});
    if (!fits || c.n != locate::kWarDistanceBytes || !WriteCode(p, c.bytes, c.n)) {
        Log(LogLevel::Error, "ai_war_distance", "failed to patch; off");
        return;
    }
    g_aiDefinesWanted = true;
    Log(LogLevel::Info, "ai_war_distance", "on; the malus counts the jumps above AI_WAR_DECLARATION_MALUS_DISTANCE");
}

// ---- 2.4 The truce divisor ---------------------------------------------------------------------
//
// PrepareWar divides its chance by AI_AGGRESSIVENESS_BOXED_IN_MULT when any contact has a truce
// with us. The division's operand now points at a value of its own. (A truce only with the
// candidate wouldn't mean anything: a target under truce already scores 0.)

void InstallTruceFix(int divisor) {
    auto value = reinterpret_cast<int32_t*>(AllocNear(g_ai.truceOperand, sizeof(int32_t)));
    int32_t rel;
    if (!value || !Rel32(g_ai.truceOperand + 4, value, &rel)) {
        Log(LogLevel::Error, "ai_truce_divisor", "no memory near the game's code; off");
        return;
    }
    *value = divisor;
    if (!WriteCode(g_ai.truceOperand, &rel, 4)) {
        Log(LogLevel::Error, "ai_truce_divisor", "failed to patch; off");
        return;
    }
    g_aiDefinesWanted = true;
    Log(LogLevel::Info, "ai_truce_divisor",
        "on; a truce divides war appetite by %d instead of AI_AGGRESSIVENESS_BOXED_IN_MULT", divisor);
}

aiwarguard::Locations g_aiWarGuard;
using WarScoreFn = int64_t* (*)(void*, int64_t*, const uint8_t*, bool*, const uint8_t*, void*);
WarScoreFn g_origWarScore = nullptr;

const uint8_t* WarPlanDefender(const uint8_t* relation) {
    __try {
        const auto id = *reinterpret_cast<const uint32_t*>(relation + g_aiWarGuard.relationCountry);
        return ResolveRef(*g_aiWarGuard.countryDb, id, g_aiWarGuard.countryId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// PrepareWar and CheckShouldContinuePreparingWar both score the same way. A zero score
// rejects a new target or lets the existing weekly check post its normal cancellation.
int64_t* WarPlanScoreHook(void* minister, int64_t* score, const uint8_t* relation, bool* truce, const uint8_t* attacker,
                          void* warData) {
    auto result = g_origWarScore(minister, score, relation, truce, attacker, warData);
    if (*score <= 0)
        return result;
    const auto defender = WarPlanDefender(relation);
    if (!defender)
        return result;

    using CreateAction = void* (*)(uint32_t);
    using InitAction = void (*)(void*, const uint8_t*, const uint8_t*);
    using IsPossible = bool (*)(void*, void*);
    using DeleteAction = void (*)(void*, unsigned);
    auto action = reinterpret_cast<CreateAction>(g_aiWarGuard.createEmptyAction)(aiwarguard::kDeclareWar);
    if (!action)
        return result;
    reinterpret_cast<InitAction>(g_aiWarGuard.initAction)(action, attacker, defender);
    const bool possible = reinterpret_cast<IsPossible>(g_aiWarGuard.isPossible)(action, nullptr);
    reinterpret_cast<DeleteAction>(g_aiWarGuard.deleteAction)(action, 1);
    if (!possible)
        *score = 0;
    return result;
}

void InstallAiWarGuard(const locate::Image& img) {
    std::string error;
    if (!aiwarguard::Locate(img, g_aiWarGuard, error)) {
        Log(LogLevel::Error, "ai_war_plan", "%s; off", error.c_str());
        return;
    }

    auto refresh = g_aiWarGuard.refresh;
    const auto code = aiwarguard::RefreshStub(g_aiWarGuard, refresh + aiwarguard::kRefreshStolen);
    auto stub = VirtualAlloc(nullptr, code.size(), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!stub) {
        Log(LogLevel::Error, "ai_war_plan", "cannot allocate refresh stub; off");
        return;
    }
    memcpy(stub, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), stub, code.size());

    uint8_t original[aiwarguard::kRefreshStolen];
    memcpy(original, refresh, sizeof(original));
    Code patch;
    patch.JumpTo(stub).Put({0x90});
    if (!WriteCode(refresh, patch.bytes, patch.n)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        Log(LogLevel::Error, "ai_war_plan", "cannot install refresh; off");
        return;
    }
    g_origWarScore = reinterpret_cast<WarScoreFn>(
        InstallJumpHook(g_aiWarGuard.score, aiwarguard::kScoreStolen, reinterpret_cast<void*>(&WarPlanScoreHook)));
    if (!g_origWarScore) {
        if (WriteCode(refresh, original, sizeof(original)))
            VirtualFree(stub, 0, MEM_RELEASE);
        else
            Log(LogLevel::Error, "ai_war_plan", "could not restore the refresh patch");
        Log(LogLevel::Error, "ai_war_plan", "cannot install war score hook; off");
        return;
    }
    Log(LogLevel::Info, "ai_war_plan",
        "declaration eligibility checked and cancelled plans request a military refresh");
}

void InstallAiBossThreat(const locate::Image& img) {
    aibossthreat::Locations found;
    std::string error;
    if (!aibossthreat::Locate(img, found, error)) {
        Log(LogLevel::Error, "ai_boss_threat_floor", "%s; off", error.c_str());
        return;
    }
    const auto code = aibossthreat::ThreatStub(found);
    // AllocNear hands out pieces of one shared block: a piece is left unused, never freed.
    auto stub = AllocNear(found.site, code.size());
    int32_t jump;
    if (!stub || !Rel32(found.site + 5, stub, &jump)) {
        Log(LogLevel::Error, "ai_boss_threat_floor", "cannot allocate nearby stub; off");
        return;
    }
    memcpy(stub, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), stub, code.size());
    Code patch;
    patch.Put({0xE9}).Put32(static_cast<uint32_t>(jump)).Put({0x90, 0x90, 0x90});
    if (!WriteCode(found.site, patch.bytes, patch.n)) {
        Log(LogLevel::Error, "ai_boss_threat_floor", "cannot install estimate patch; off");
        return;
    }
    Log(LogLevel::Info, "ai_boss_threat_floor", "hostile bosses count for at least their cached fleet power");
}
