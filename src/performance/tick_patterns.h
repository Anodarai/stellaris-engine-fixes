// Locations for tick pacing and sef_bench (bench_runtime.inl): the console command manager,
// CInGameIdler::CheckTick, HandleTurnTick and RenderFrame, and the fields they read. The AI
// schedule and script storage use the tick and date fields found here (re/simulation_speed.md).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "../core/locate.h"

namespace locate {

// The console, the tick loop and frame rendering, for tick pacing and the benchmark command
// (re/simulation_speed.md).
struct TickLocations {
    int32_t* commandsPerTurn = nullptr; // &NDefines::NGameplay::NUM_COMMANDS_PER_TURN: ticks queued per frame
    int32_t* ticksPerTurn = nullptr;    // &g_nTicksPerTurn (console ticks_per_turn; 1)
    uint8_t** gameState = nullptr;      // &g_CurrentGameState
    size_t idlerMultiplayer = 0;        // CInGameIdler: bool multiplayer
    uint8_t* execute =
        nullptr; // CConsoleCmdManager::Execute(manager, SResult* out, const CPdxArray<CString>& args) (hooked)
    size_t consoleLockedFlag = 0;  // CConsoleCmdManager: bool, set in multiplayer and ironman
    uint8_t* runCommand = nullptr; // CConsole::RunCommandNow(console, const CString& line)
    uint8_t* addToHistory =
        nullptr; // &g_bAddToHistory: RunCommandNow records the line in the history and console_history.txt
    uint8_t** console = nullptr;       // &CConsole::_pInstance
    uint8_t* checkTick = nullptr;      // CInGameIdler::CheckTick(idler, float dt): once per frame (hooked)
    size_t idlerSpeed = 0;             // CInGameIdler: int game speed index
    size_t idlerPaused = 0;            // CInGameIdler: bool paused
    float* speedTurnTimes = nullptr;   // gGameSpeedTurnTimes: seconds between tick batches, per speed
    uint8_t* turnTick = nullptr;       // CGameState::HandleTurnTick(state, checksums): one tick (hooked)
    size_t stateTick = 0;              // CGameState: int tick of the day
    size_t stateDate = 0;              // CGameState: int date in hours
    uint8_t* renderFrame = nullptr;    // CGameState::RenderFrame: a frame drawn from inside the simulation (hooked)
    uint8_t* frameSmoothing = nullptr; // &g_bFrameSmoothing (the console command `smooth`)
};

// CConsoleCmdManager::Execute. Its first 15 bytes (three `mov [rsp+N], reg`) are moved by the hook.
//   +82 cmp [rcx+releaseFlag],r12b   +91 cmp [rcx+lockedFlag],r12b (multiplayer or ironman)
constexpr char kConsoleExecute[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC ?? ?? ?? ?? "
    "49 8B F8 48 8B F2 48 8B D9 C6 45 ?? 00 45 33 E4 44 89 65 ?? 0F 57 C0 F3 0F 7F 45 ?? 66 0F 6F 05 ?? ?? ?? ?? "
    "F3 0F 7F 45 ?? 44 88 65 ?? 44 38 A1 ?? ?? ?? ?? 74 ?? 44 38 A1 ?? ?? ?? ?? 74";

constexpr size_t kConsoleExecuteStolenBytes = 15;

// OnExecute_UnchartedSpace runs another command: RunCommandNow(CConsole::_pInstance, "TerraIncognita").
//   +64 mov rcx,[rip+console]   +71 call RunCommandNow
constexpr char kRunTerraIncognita[] =
    "48 C7 44 24 48 0F 00 00 00 48 C7 44 24 40 0E 00 00 00 F2 0F 10 05 ?? ?? ?? ?? F2 0F 11 44 24 30 "
    "8B 05 ?? ?? ?? ?? 89 44 24 38 0F B7 05 ?? ?? ?? ?? 66 89 44 24 3C 40 88 7C 24 3E 48 8D 54 24 20 "
    "48 8B 0D ?? ?? ?? ?? E8";

//   +44 cmp byte [rip+g_bAddToHistory],r15b
constexpr char kRunCommandPrologue[] =
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B DA 4C 8B F1 "
    "45 33 FF 41 8B F7 44 89 7D ?? 44 38 3D ?? ?? ?? ?? 0F 84";

constexpr size_t kRunCommandSearch = 0x200; // RunCommandNow calls Execute within this many bytes

// CInGameIdler::CheckTick. Its first 16 bytes (mov [rsp+8],rbx and seven pushes) are moved by the hook.
//   +92 cmp byte [rcx+paused],0
constexpr char kCheckTick[] =
    "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 0F 29 74 24 50 0F 28 F1 48 8B E9 "
    "4C 8B 15 ?? ?? ?? ?? 4D 85 D2 0F 84 ?? ?? ?? ?? 41 80 BA A0 00 00 00 00 0F 84 ?? ?? ?? ?? "
    "48 8B 81 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 80 ?? ?? ?? ?? C1 E8 06 A8 01 0F 85 ?? ?? ?? ?? "
    "80 B9 ?? ?? ?? ?? 00 0F 85";

constexpr size_t kCheckTickStolenBytes = 16;

// Inside CheckTick: +0 movsxd rax,[rbp+speed]  +7 lea rcx,[rip+gGameSpeedTurnTimes]  +14 movss xmm1,[rcx+rax*4]
constexpr char kCheckTickSpeed[] = "48 63 85 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? F3 0F 10 0C 81";

// Inside CheckTick, the multiplayer check: +0 movzx edi,byte [rbp+multiplayer]  +23 mov eax,[r10+tick]
constexpr char kCheckTickPlayers[] =
    "0F B6 BD ?? ?? ?? ?? 49 8B D5 48 8B D9 41 8B 82 ?? ?? ?? ?? 8D 0C 80 41 8B 42 ?? 8D 34 48";

// Inside CheckTick, the tick count: max(min(NUM_COMMANDS_PER_TURN, 10), g_nTicksPerTurn)
//   +0 lea rax,[rip+commands]  +14 cmp [rip+commands],10  +25 mov eax,[rip+ticksPerTurn]  +31 lea rcx,[rip+ticksPerTurn]
constexpr char kCheckTickBatch[] =
    "48 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 0A 48 0F 4C D0 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? "
    "3B 02 48 0F 4C CA 44 8B 21";

constexpr size_t kCheckTickSearch = 0x200;

// CGameState::HandleTurnTick. Its first 16 bytes (mov [rsp+10h],rbx and seven pushes) are moved by the hook.
//   +125 mov rax,[rip+g_CurrentInGameIdler]  +139 mov byte [rax+inTick],1  +158 call RenderFrame
//   +190 inc dword [rsi+tick]  +204 mov ebx,[rsi+tick]  +207 lea rcx,[rsi+date]
constexpr char kTurnTick[] =
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
    "48 8B DA 48 8B F1 45 33 E4 44 89 A5 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0F 48 8B 89 40 01 00 00 "
    "48 8B 01 FF 50 70 EB 03 49 8B C4 44 38 A0 ?? ?? ?? ?? 75 ?? 44 39 63 14 74 ?? 48 8B D3 48 8B CE "
    "E8 ?? ?? ?? ?? 84 C0 74 06 66 C7 46 40 01 00 48 8D 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 07 C6 80 ?? ?? ?? ?? 01 33 D2 48 8B CE E8 ?? ?? ?? ?? 84 C0 74 05 "
    "E8 ?? ?? ?? ?? 45 32 ED 44 88 A5 ?? ?? ?? ?? 45 32 FF 44 88 BD ?? ?? ?? ?? 44 88 A5 ?? ?? ?? ?? "
    "FF 46 ?? E8 ?? ?? ?? ?? F6 40 38 01 74 ?? 8B 5E ?? 48 8D 8E ?? ?? ?? ??";

constexpr size_t kTurnTickStolenBytes = 16;

// CGameState::RenderFrame. Its first 15 bytes (three `mov [rsp+N], reg`) are moved by the hook.
//   +26 mov rax,[rip+g_CurrentInGameIdler]  +40 movzx ebx,byte [rax+inTick]
constexpr char kRenderFrame[] =
    "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 48 89 54 24 10 41 56 48 83 EC 40 48 8B 05 ?? ?? ?? ?? "
    "4C 8B B0 40 01 00 00 0F B6 98 ?? ?? ?? ?? C6 80 ?? ?? ?? ?? 01";

constexpr size_t kRenderFrameStolenBytes = 15;

// CGameState::RenderFrameBetweenCommands: +56 cmp byte [rip+g_bFrameSmoothing],0  +65 cmp [rip+g_nTicksPerTurn],1
constexpr char kRenderBetweenCommands[] =
    "40 53 48 83 EC 20 80 B9 A0 00 00 00 00 48 8B D9 74 ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 80 B9 ?? ?? ?? ?? 00 "
    "75 ?? F2 0F 10 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 80 3D ?? ?? ?? ?? 00 74 ?? 83 3D ?? ?? ?? ?? 01";

struct BenchControls {
    uint8_t* setSpeed = nullptr;
    uint8_t* setPaused = nullptr;
};

// The speed setter reads the pause flag and reads/writes the same speed field as CheckTick.
constexpr char kBenchSetSpeed[] =
    "48 89 5C 24 08 57 48 83 EC 20 80 B9 ?? ?? ?? ?? 00 8B FA 48 8B D9 75 0D "
    "8B 91 ?? ?? ?? ?? FF C2 E8 ?? ?? ?? ?? 89 BB ?? ?? ?? ?? 85 FF 79";

// SetPaused takes SPauseGameSettings. Later it calls SetGameSpeed and stores the new pause flag.
constexpr char kBenchSetPaused[] =
    "48 89 5C 24 10 57 48 83 EC 60 80 B9 ?? ?? ?? ?? 00 48 8B FA 48 8B D9 74 4D "
    "48 8D 91 ?? ?? ?? ?? 48 83 7A 18 10 72 03 48 8B 12 48 83 7F 28 10 48 8D 4F 10";

inline bool LocateBenchControls(const Image& img, const TickLocations& ticks, BenchControls& out, std::string& error) {
    auto* speed = FindOne(img, "CInGameIdler::SetGameSpeed", kBenchSetSpeed, error);
    if (!speed)
        return false;
    auto* pause = FindOne(img, "CInGameIdler::SetPaused", kBenchSetPaused, error);
    if (!pause)
        return false;
    if (static_cast<size_t>(Disp32(speed + 0xC)) != ticks.idlerPaused ||
        static_cast<size_t>(Disp32(speed + 0x1A)) != ticks.idlerSpeed ||
        static_cast<size_t>(Disp32(speed + 0x27)) != ticks.idlerSpeed ||
        pause[0xC8] != 0xE8 || RelTarget(pause + 0xC9, pause + 0xCD) != speed ||
        pause[0x12A] != 0x88 || pause[0x12B] != 0x8B || static_cast<size_t>(Disp32(pause + 0x12C)) != ticks.idlerPaused) {
        error = "benchmark setters do not agree with CheckTick";
        return false;
    }
    out = {speed, pause};
    return true;
}

// Fills `out` for the benchmark command; false and `error` on failure.
inline bool LocateTicks(const Image& img, TickLocations& out, std::string& error) {
    auto one = [&](const char* what, const char* pattern) { return FindOne(img, what, pattern, error); };
    auto in = [&](const char* what, uint8_t* begin, size_t size, const char* pattern) -> uint8_t* {
        auto hits = FindAll(begin, begin + size, Parse(pattern), 1);
        if (hits.empty())
            error = std::string(what) + ": not found";
        return hits.empty() ? nullptr : hits[0];
    };

    if (!(out.execute = one("CConsoleCmdManager::Execute", kConsoleExecute)))
        return false;
    out.consoleLockedFlag = static_cast<size_t>(Disp32(out.execute + 91));

    uint8_t* run = one("OnExecute_UnchartedSpace's RunCommandNow call", kRunTerraIncognita);
    if (!run)
        return false;
    out.console = reinterpret_cast<uint8_t**>(RelTarget(run + 67, run + 71));
    out.runCommand = RelTarget(run + 72, run + 76);
    if (!MatchAt(out.runCommand, Parse(kRunCommandPrologue))) {
        error = "CConsole::RunCommandNow: unexpected prologue";
        return false;
    }
    out.addToHistory = RelTarget(out.runCommand + 47, out.runCommand + 51);
    bool callsExecute = false;
    for (uint8_t* p = out.runCommand; p < out.runCommand + kRunCommandSearch && !callsExecute; ++p) {
        callsExecute = *p == 0xE8 && RelTarget(p + 1, p + 5) == out.execute;
    }
    if (!callsExecute) {
        error = "CConsole::RunCommandNow does not call the Execute found";
        return false;
    }

    if (!(out.checkTick = one("CInGameIdler::CheckTick", kCheckTick)))
        return false;
    out.idlerPaused = static_cast<size_t>(Disp32(out.checkTick + 92));
    uint8_t* speed = in("CheckTick game speed", out.checkTick, kCheckTickSearch, kCheckTickSpeed);
    if (!speed)
        return false;
    out.idlerSpeed = static_cast<size_t>(Disp32(speed + 3));
    out.speedTurnTimes = reinterpret_cast<float*>(RelTarget(speed + 10, speed + 14));
    out.gameState = reinterpret_cast<uint8_t**>(RelTarget(out.checkTick + 34, out.checkTick + 38));
    uint8_t* players = in("CheckTick multiplayer check", out.checkTick, kCheckTickSearch, kCheckTickPlayers);
    if (!players)
        return false;
    out.idlerMultiplayer = static_cast<size_t>(Disp32(players + 3));
    uint8_t* batch = in("CheckTick tick count", out.checkTick, kCheckTickSearch, kCheckTickBatch);
    if (!batch)
        return false;
    out.commandsPerTurn = reinterpret_cast<int32_t*>(RelTarget(batch + 3, batch + 7));
    out.ticksPerTurn = reinterpret_cast<int32_t*>(RelTarget(batch + 27, batch + 31));
    if (reinterpret_cast<int32_t*>(RelTarget(batch + 16, batch + 21)) != out.commandsPerTurn ||
        reinterpret_cast<int32_t*>(RelTarget(batch + 34, batch + 38)) != out.ticksPerTurn) {
        error = "CheckTick's tick count reads two different globals";
        return false;
    }

    if (!(out.turnTick = one("CGameState::HandleTurnTick", kTurnTick)))
        return false;
    out.stateTick = out.turnTick[192];
    out.stateDate = static_cast<size_t>(Disp32(out.turnTick + 210));
    if (out.turnTick[206] != out.stateTick || players[26] != out.stateTick) {
        error = "HandleTurnTick and CheckTick read the tick count at different offsets";
        return false;
    }

    if (!(out.renderFrame = one("CGameState::RenderFrame", kRenderFrame)))
        return false;
    if (RelTarget(out.turnTick + 159, out.turnTick + 163) != out.renderFrame ||
        RelTarget(out.turnTick + 128, out.turnTick + 132) != RelTarget(out.renderFrame + 29, out.renderFrame + 33) ||
        Disp32(out.turnTick + 139) != Disp32(out.renderFrame + 43)) {
        error = "HandleTurnTick and RenderFrame disagree (call target, idler or in-tick flag)";
        return false;
    }

    uint8_t* between = one("CGameState::RenderFrameBetweenCommands", kRenderBetweenCommands);
    if (!between)
        return false;
    out.frameSmoothing = RelTarget(between + 58, between + 63);
    return true;
}

} // namespace locate
