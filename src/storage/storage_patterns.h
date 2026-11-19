// Locations for script storage (scriptstorage_runtime.inl; re/script_storage.md): the log effect
// and trigger, variables, saves, loads, new games and the multiplayer check.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "../core/locate.h"

namespace locate {

// --- Effects, saves, loads and new games ----------------------------------------------------

// Saving and loading for the Paradox script save store.
struct SaveLocations {
    uint8_t* startSave =
        nullptr; // CSaveGameTaskManager::StartSaveToPath(manager, const CString& path, bool, bool, bool) -> 0 when started (hooked)
    uint8_t* load = nullptr;        // CGameState::Load(state, CFile*, const CString& path) (hooked)
    uint8_t* newGame = nullptr;     // CGameState::OnNewGameStarted(state) (hooked)
    uint8_t* readingSave = nullptr; // &g_bIsReadingSaveFile, which Load sets
};

// OnExecute_Effect(SResult* out, const CPdxArray<CString>& args), the console's `effect`.
//   +58 cmp [rip+g_CurrentGameState],rbx   +95 movsd xmm1,[rip+"No game state"]
constexpr char kEffectCommand[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 4C 8B F2 48 8B F9 45 33 ED 41 8B DD 89 5C 24 30 48 39 1D ?? ?? ?? ?? 75 ?? "
    "44 89 6C 24 58 4C 89 6C 24 60 48 C7 45 80 0F 00 00 00 48 C7 44 24 78 0D 00 00 00 F2 0F 10 0D";

constexpr size_t kEffectCommandSearch = 0x290; // it calls ReadAndExecuteEffect within this many bytes

constexpr char kReadAndExecuteEffect[] =
    "48 89 5C 24 08 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 4C 8B F1";

// Inside it: +0 mov rcx,[rip+g_CurrentGameState]  +7 call CGameState::GetLocalObserved  +12 mov rsi,rax
constexpr char kCallLocalObserved[] = "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F0";

constexpr size_t kReadAndExecuteEffectSearch = 0x100;

constexpr size_t kReadAndExecuteEffectStringSearch = 0x1000;

// CSaveGameTaskManager::StartSaveToPath: the prologue (15 bytes of spills taken), then
//   +41 movzx r12d,r8b  +44 mov r13,rcx  +60 lea r14,[rdx+10h] and cmp qword [r14+18h],10h: the path
//   is a CString (buffer at +0x10, capacity at +0x28)
constexpr char kStartSaveToPath[] =
    "48 89 5C 24 10 44 88 4C 24 20 44 88 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 45 0F B6 E0 4C 8B E9 45 33 FF 44 89 BD ?? ?? ?? ?? 4C 8D 72 10 49 8B D6 49 83 7E 18 10 "
    "72 03 49 8B 16";

constexpr size_t kStartSaveToPathStolenBytes = 15;

// CGameState::Load(state, CFile*, const CString& path): the prologue (15 bytes of spills taken), then
//   +39 mov r12,r8 (the path)  +56 lea rax,[g_bIsReadingSaveFile]  +67 movzx eax,byte [it]  +77 mov byte [it],1
constexpr char kGameStateLoad[] =
    "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 4D 8B E0 48 8B F2 4C 8B F9 33 D2 89 95 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 45 ?? "
    "0F B6 05 ?? ?? ?? ?? 88 45 ?? C6 05 ?? ?? ?? ?? 01";

constexpr size_t kGameStateLoadStolenBytes = 15;

// CGameState::OnNewGameStarted: the prologue (15 bytes of spills taken), then
//   +42 movzx ebx,byte [g_AllowGetPlayer]  +55 mov byte [g_AllowGetPlayer],0  +62 call
constexpr char kNewGameStarted[] =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? "
    "48 81 EC ?? ?? ?? ?? 4C 8B F1 0F B6 1D ?? ?? ?? ?? 88 9D ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 E8";

constexpr size_t kNewGameStartedStolenBytes = 15;

// The console's `effect` command, OnExecute_Effect, and the ReadAndExecuteEffect it calls; `gameState` is
// TickLocations::gameState. Script storage runs stored text through it.
inline bool LocateEffectCommand(const Image& img, uint8_t** gameState, uint8_t** effectCommand, uint8_t** readExecute,
                                std::string& error) {
    struct Found {
        uint8_t* effectCommand = nullptr;
        uint8_t* readExecuteEffect = nullptr;
    } out;
    // The console's `trigger` command is compiled to the same bytes up to its call, so the call tells them apart.
    Pattern runner = Parse(kReadAndExecuteEffect);
    uint8_t* run = nullptr;
    for (uint8_t* command : FindInCode(img, kEffectCommand)) {
        for (uint8_t* p = command; p < command + kEffectCommandSearch; ++p) {
            if (*p != 0xE8)
                continue;
            uint8_t* target = RelTarget(p + 1, p + 5); // not always a call: the byte may be part of another instruction
            bool inCode = false;
            for (const Section& s : img.code) {
                inCode = inCode || (target >= s.begin && target + kReadAndExecuteEffectStringSearch <= s.end);
            }
            if (!inCode || !MatchAt(target, runner))
                continue;
            if (run) {
                error = "OnExecute_Effect: more than one match";
                return false;
            }
            run = target;
            out.effectCommand = command;
            out.readExecuteEffect = target;
            break;
        }
    }
    if (!run) {
        error = "OnExecute_Effect: not found";
        return false;
    }
    static const char kExecuting[] = "Executing effect script.";
    bool executes = false;
    for (uint8_t* p = run; p < run + kReadAndExecuteEffectStringSearch && !executes; ++p) {
        const uint8_t* text = RelTarget(p, p + 4);
        for (const Section& s : img.data) {
            if (text >= s.begin && text + sizeof(kExecuting) <= s.end &&
                memcmp(text, kExecuting, sizeof(kExecuting) - 1) == 0) {
                executes = true;
            }
        }
    }
    if (!executes) {
        error = "ReadAndExecuteEffect does not use its message";
        return false;
    }
    if (reinterpret_cast<uint8_t**>(RelTarget(out.effectCommand + 58, out.effectCommand + 62)) != gameState) {
        error = "OnExecute_Effect checks a different game state";
        return false;
    }
    const char* noGameState = reinterpret_cast<const char*>(RelTarget(out.effectCommand + 95, out.effectCommand + 99));
    if (memcmp(noGameState, "No game state", 13) != 0) {
        error = "OnExecute_Effect's message is not the one expected";
        return false;
    }
    auto observed = FindAll(run, run + kReadAndExecuteEffectSearch, Parse(kCallLocalObserved), 1);
    if (observed.empty() || reinterpret_cast<uint8_t**>(RelTarget(observed[0] + 3, observed[0] + 7)) != gameState) {
        error = "ReadAndExecuteEffect does not get the local country as expected";
        return false;
    }
    *effectCommand = out.effectCommand;
    *readExecute = out.readExecuteEffect;
    return true;
}

inline bool LocateSaves(const Image& img, SaveLocations& out, std::string& error) {
    out.startSave = FindOne(img, "CSaveGameTaskManager::StartSaveToPath", kStartSaveToPath, error);
    if (!out.startSave)
        return false;
    out.load = FindOne(img, "CGameState::Load", kGameStateLoad, error);
    if (!out.load)
        return false;
    const uint8_t* p = out.load;
    out.readingSave = RelTarget(p + 59, p + 63);
    if (RelTarget(p + 70, p + 74) != out.readingSave || RelTarget(p + 79, p + 84) != out.readingSave) {
        error = "CGameState::Load: it doesn't save and set one flag";
        return false;
    }
    out.newGame = FindOne(img, "CGameState::OnNewGameStarted", kNewGameStarted, error);
    if (!out.newGame)
        return false;
    p = out.newGame;
    const uint8_t* flag = RelTarget(p + 45, p + 49);
    if (RelTarget(p + 57, p + 62) != flag) {
        error = "CGameState::OnNewGameStarted: it doesn't clear g_AllowGetPlayer";
        return false;
    }
    return true;
}

// --- Writing a save -------------------------------------------------------------------------

struct SaveWriteLocations {
    uint8_t* zipAndWrite = nullptr;
    size_t paramsOffset = 0; // SSaveParameters in CSaveGameTaskManager, beginning with the normalized path
    uint8_t* destroyArchive = nullptr;
    uint8_t* archiveDestroyCalls[2]{};
    uint8_t* closeFile = nullptr;
    uint8_t* closeCall = nullptr;
    uint8_t* cloudWrite = nullptr;
    uint8_t* cloudCall = nullptr;
    uint8_t* addFile = nullptr;
    uint8_t* addFileCalls[2]{};
};

// SaveGameOnCurrentThread calls CreateMemoryFiles, copies the manager's SSaveParameters, then calls ZipAndWrite.
constexpr char kSaveOnCurrentThread[] =
    "40 53 48 81 EC ?? ?? ?? ?? 48 8B D9 E8 ?? ?? ?? ?? 48 8D 53 ?? 48 8D 4C 24 20 E8 ?? ?? ?? ?? "
    "48 8B D0 48 8B CB E8 ?? ?? ?? ?? 48 81 C4 ?? ?? ?? ?? 5B C3";

// StartSaveToPath stores the normalized path, then marks the request pending and returns success.
constexpr char kAcceptedSavePath[] = "49 8D 4D ?? 4C 8B C3 E8 ?? ?? ?? ?? 45 88 A5 ?? ?? ?? ?? 4D 89 BD ?? ?? ?? ?? "
                                     "0F B6 85 ?? ?? ?? ?? 41 88 85 ?? ?? ?? ?? 41 C6 45 ?? 01 33 F6";

constexpr char kZipAndWrite[] =
    "48 89 54 24 10 53 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 4C 8B F2 48 8B F1 45 33 ED "
    "44 89 AC 24 ?? ?? ?? ?? 45 8D 4D 01 44 38 6A ?? 0F 84";

constexpr size_t kZipAndWriteStolenBytes = 16;

constexpr char kZipAddFile[] =
    "48 8B 56 ?? 4C 8D 42 20 48 8D 4C 24 30 E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 41 B9 03 00 01 00";

constexpr char kZipCloudWrite[] = "48 8B 56 48 49 8B CC E8 ?? ?? ?? ?? 84 C0 74 ?? 48 85 DB";

// Both local output paths destroy the archive in normal control flow. The raw path then destroys the save parameters.
constexpr char kZipDestroyRaw[] = "48 8D 8C 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 49 8B CE E8 ?? ?? ?? ?? E9";

constexpr char kZipDestroyPacked[] =
    "48 8B 56 48 48 8D 8C 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 8D 8C 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 41 8B FD";

// CArchiveFile::~CArchiveFile calls PHYSFS_close and tests its result, but returns void.
constexpr char kArchiveClose[] = "48 8B B9 ?? ?? ?? ?? 48 85 FF 74 ?? 48 8B 0F E8 ?? ?? ?? ?? 85 C0 75";

inline bool LocateSaveWrite(const Image& img, const SaveLocations& saves, SaveWriteLocations& out, std::string& error) {
    uint8_t* sync = FindOne(img, "CSaveGameTaskManager::SaveGameOnCurrentThread", kSaveOnCurrentThread, error);
    if (!sync)
        return false;
    out.zipAndWrite = FindOne(img, "CSaveGameTaskManager::ZipAndWrite", kZipAndWrite, error);
    if (!out.zipAndWrite)
        return false;
    if (RelTarget(sync + 38, sync + 42) != out.zipAndWrite) {
        error = "SaveGameOnCurrentThread calls a different ZipAndWrite";
        return false;
    }
    out.paramsOffset = sync[20];
    auto accepted = FindAll(saves.startSave, saves.startSave + 0x510, Parse(kAcceptedSavePath), 2);
    if (accepted.size() != 1 || accepted[0][3] != out.paramsOffset) {
        error = "StartSaveToPath writes the path at a different offset from SaveGameOnCurrentThread";
        return false;
    }
    auto inZip = [&](const char* pattern) {
        return FindAll(out.zipAndWrite, out.zipAndWrite + 0x800, Parse(pattern), 3);
    };
    auto add = inZip(kZipAddFile);
    auto cloud = inZip(kZipCloudWrite);
    auto raw = inZip(kZipDestroyRaw);
    auto packed = inZip(kZipDestroyPacked);
    if (add.size() != 2 || cloud.size() != 1 || raw.size() != 1 || packed.size() != 1) {
        error = "ZipAndWrite: expected two AddFile calls, a cloud write and two local archive closes";
        return false;
    }
    out.addFileCalls[0] = add[0] + 13;
    out.addFileCalls[1] = add[1] + 13;
    out.addFile = RelTarget(out.addFileCalls[0] + 1, out.addFileCalls[0] + 5);
    out.cloudCall = cloud[0] + 7;
    out.cloudWrite = RelTarget(out.cloudCall + 1, out.cloudCall + 5);
    out.archiveDestroyCalls[0] = raw[0] + 8;
    out.archiveDestroyCalls[1] = packed[0] + 26;
    out.destroyArchive = RelTarget(out.archiveDestroyCalls[0] + 1, out.archiveDestroyCalls[0] + 5);
    if (RelTarget(out.archiveDestroyCalls[1] + 1, out.archiveDestroyCalls[1] + 5) != out.destroyArchive ||
        RelTarget(out.addFileCalls[1] + 1, out.addFileCalls[1] + 5) != out.addFile) {
        error = "ZipAndWrite: the archive calls disagree";
        return false;
    }
    auto close = FindAll(out.destroyArchive, out.destroyArchive + 0x60, Parse(kArchiveClose), 2);
    if (close.size() != 1) {
        error = "CArchiveFile::~CArchiveFile: expected a checked PHYSFS_close";
        return false;
    }
    out.closeCall = close[0] + 15;
    out.closeFile = RelTarget(out.closeCall + 1, out.closeCall + 5);
    return true;
}

// --- Variables ------------------------------------------------------------------------------

// Script variables, as set_variable, is_variable_set and check_variable reach them.
struct VariableLocations {
    uint8_t* access =
        nullptr;             // AccessVariablePointer(CEventScope&, const CString& name) -> CVariables*, as set_variable
    uint8_t* get = nullptr;  // GetVariablePointer(const CEventScope&, const CString& name) -> const CVariables*
    uint8_t* find = nullptr; // the variables' map: find(map, SMapEntry** out, const CString& key)
    uint8_t* set = nullptr;  // CVariables::SetVariable(variables, const CString& name, CFixedPoint)
    size_t map = 0;          // CVariables: the map
    size_t entryEmpty = 0;   // SMapEntry: byte, 0xFF when nothing is there
    size_t entryValue = 0;   // SMapEntry: CFixedPoint value
    size_t effectName = 0;   // CSetVariableEffect, CClearVariableEffect: CString name, right after the CEffect base
    size_t triggerName = 0;  // CIsVariableSetTrigger: CString name, right after the CTrigger base
};

// CSetVariableEffect::ExecuteActual: the value, then
//   +46 lea rdx,[rdi+name]  +56 call AccessVariablePointer  +69 lea rdx,[rdi+name]  +79 call CVariables::SetVariable
constexpr char kSetVariableEffect[] =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 4C 8B C2 48 81 C1 ?? ?? ?? ?? 48 8D 54 24 30 "
    "E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8D 97 ?? ?? ?? ?? 48 8B CE E8 ?? ?? ?? ?? 48 85 C0 74 12 4C 8B C3 48 8D 97 "
    "?? ?? ?? ?? 48 8B C8 E8";

// CClearVariableEffect::ExecuteActual:
//   +13 lea rbx,[rcx+name]  +26 call AccessVariablePointer  +39 lea rcx,[rax+map]  +51 call find
//   +61 cmp byte [rcx+empty],0FFh  +73 call CVariables::RemoveVariable
constexpr char kClearVariableEffect[] =
    "48 89 5C 24 10 57 48 83 EC 20 48 8B C2 48 8D 99 ?? ?? ?? ?? 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 "
    "74 27 48 8D 48 ?? 4C 8B C3 48 8D 54 24 30 E8 ?? ?? ?? ?? 48 8B 4C 24 30 80 79 ?? FF 74 0B 48 8B D3 48 8B CF E8";

// CIsVariableSetTrigger::ActualEvaluate:
//   +9 lea rbx,[rcx+name]  +22 call GetVariablePointer  +38 lea rcx,[rax+map]  +50 call find  +60 cmp byte [rax+empty],0FFh
constexpr char kIsVariableSetTrigger[] =
    "40 53 48 83 EC 20 48 8B C2 48 8D 99 ?? ?? ?? ?? 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 75 06 48 83 C4 20 5B C3 "
    "48 8D 48 ?? 4C 8B C3 48 8D 54 24 30 E8 ?? ?? ?? ?? 48 8B 44 24 30 80 78 ?? FF 0F 95 C0";

// CCheckVariableTrigger::ActualEvaluate: +31 call GetVariablePointer  +86 lea rcx,[rbp+map]  +101 call find
//   +111 cmp byte [rcx+empty],0FFh  +121 mov rcx,[rcx+value]
constexpr char kCheckVariableTrigger[] =
    "48 89 5C 24 20 55 56 57 48 83 EC 20 48 8B FA 48 8D B1 ?? ?? ?? ?? 48 8B D9 48 8B D6 48 8B CF E8 ?? ?? ?? ?? "
    "48 8B E8 48 85 C0 75 0D 48 8B 5C 24 58 48 83 C4 20 5F 5E 5D C3 4C 89 74 24 48 48 8D 8B ?? ?? ?? ?? 4C 8B C7 "
    "48 8D 54 24 50 4C 8D 73 ?? E8 ?? ?? ?? ?? 48 8D 4D ?? 4C 8B C6 48 8D 54 24 40 48 8B D8 E8 ?? ?? ?? ?? "
    "48 8B 4C 24 40 80 79 ?? FF 75 04 33 C9 EB 04 48 8B 49 ??";

inline bool LocateVariables(const Image& img, VariableLocations& out, std::string& error) {
    const uint8_t* p = FindOne(img, "CSetVariableEffect::ExecuteActual", kSetVariableEffect, error);
    if (!p)
        return false;
    out.effectName = static_cast<size_t>(Disp32(p + 49));
    out.access = RelTarget(p + 57, p + 61);
    out.set = RelTarget(p + 80, p + 84);
    if (static_cast<size_t>(Disp32(p + 72)) != out.effectName) {
        error = "CSetVariableEffect: two different names";
        return false;
    }
    if (!(p = FindOne(img, "CClearVariableEffect::ExecuteActual", kClearVariableEffect, error)))
        return false;
    out.map = p[42];
    out.find = RelTarget(p + 52, p + 56);
    out.entryEmpty = p[63];
    if (static_cast<size_t>(Disp32(p + 16)) != out.effectName || RelTarget(p + 27, p + 31) != out.access) {
        error = "CClearVariableEffect: a different name or variables than set_variable's";
        return false;
    }
    if (!(p = FindOne(img, "CIsVariableSetTrigger::ActualEvaluate", kIsVariableSetTrigger, error)))
        return false;
    out.triggerName = static_cast<size_t>(Disp32(p + 12));
    out.get = RelTarget(p + 23, p + 27);
    if (p[41] != out.map || RelTarget(p + 51, p + 55) != out.find || p[62] != out.entryEmpty) {
        error = "CIsVariableSetTrigger: a different map than clear_variable's";
        return false;
    }
    if (!(p = FindOne(img, "CCheckVariableTrigger::ActualEvaluate", kCheckVariableTrigger, error)))
        return false;
    out.entryValue = p[124];
    if (RelTarget(p + 32, p + 36) != out.get || p[89] != out.map || RelTarget(p + 102, p + 106) != out.find ||
        p[113] != out.entryEmpty || out.entryValue % 8 != 0) {
        error = "CCheckVariableTrigger: a different map than is_variable_set's";
        return false;
    }
    return true;
}

// --- The log effect and trigger -------------------------------------------------------------

// CLogEffect::ExecuteActual and CLogTrigger::ActualEvaluate, and the call that builds the text they log.
struct LogScriptLocations {
    uint8_t* logEffect = nullptr;     // CLogEffect::ExecuteActual(effect, scope) (hooked)
    uint8_t* logTrigger = nullptr;    // CLogTrigger::ActualEvaluate(trigger, scope) (hooked)
    size_t logEffectText = 0;         // CLogEffect: CString text
    size_t logTriggerText = 0;        // CLogTrigger: CString text
    uint8_t* textCall = nullptr;      // in the log effect: call CTextBase::ProcessString (redirected while recording)
    uint8_t* processString = nullptr; // CTextBase::ProcessString(text, CString* out, SStringView* in, bool, ...)
};

// CLogEffect::ExecuteActual and CLogTrigger::ActualEvaluate, compiled alike but for their text:
//   +99 mov ecx,[rdi+text+0x20] (its size)  +106 lea rax,[rdi+text+0x10]
constexpr char kLogScript[] =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? "
    "48 8B DA 48 8B F9 45 33 FF 44 89 BD ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 8D 8D ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? 48 89 9D ?? ?? ?? ?? 44 89 BD ?? ?? ?? ?? 48 89 9D ?? ?? ?? ?? 8B 8F ?? ?? ?? ?? 48 8D 87 ?? ?? ?? ?? 48 83";

constexpr size_t kLogScriptStolenBytes = 15;

// In both, after the text's view is built: +24 call ProcessString(gameText, &out, &view, false, 0, 0).
constexpr char kLogTextCall[] = "4C 89 7C 24 20 45 33 C9 4C 8D 44 24 60 48 8D 55 30 48 8D 8D ?? ?? ?? ?? E8";

constexpr size_t kLogTextCallAt = 0x8B;

// The two log functions. `effectText` is where the log effect's text must be (the CEffect base's size),
// `triggerText` the trigger's, or 0 for whichever of the two isn't the effect.
inline bool LocateLogScript(const Image& img, size_t effectText, size_t triggerText, LogScriptLocations& out,
                            std::string& error) {
    auto logs = FindInCode(img, kLogScript, 4);
    if (logs.size() != 2) {
        error = "CLogEffect and CLogTrigger: " + std::to_string(logs.size()) + " matches";
        return false;
    }
    Pattern call = Parse(kLogTextCall);
    for (uint8_t* hit : logs) {
        size_t buf = static_cast<size_t>(Disp32(hit + 106)), size = static_cast<size_t>(Disp32(hit + 99));
        if (size != buf + offsetof(game::String, size) - offsetof(game::String, buf) ||
            buf < offsetof(game::String, buf)) {
            error = "CLogEffect or CLogTrigger reads its text unexpectedly";
            return false;
        }
        size_t text = buf - offsetof(game::String, buf);
        uint8_t* at = hit + kLogTextCallAt;
        if (!MatchAt(at, call)) {
            error = "CLogEffect or CLogTrigger: the call that builds the text is not where expected";
            return false;
        }
        uint8_t* process = RelTarget(at + 25, at + 29);
        if (out.processString && process != out.processString) {
            error = "CLogEffect and CLogTrigger build their text differently";
            return false;
        }
        out.processString = process;
        if (text == effectText) {
            out.logEffect = hit;
            out.logEffectText = text;
            out.textCall = at + 24;
        } else if (triggerText == 0 || text == triggerText) {
            out.logTrigger = hit;
            out.logTriggerText = text;
        }
    }
    if (!out.logEffect || !out.logTrigger) {
        error = "CLogEffect and CLogTrigger: their texts are not right after the CEffect and CTrigger bases";
        return false;
    }
    return true;
}

// --- Multiplayer ----------------------------------------------------------------------------

// What the is_multiplayer trigger reads (CIsMultiplayerTrigger::ActualEvaluate): multiplayer when the object at
// `*session` exists and its byte at `flag` is set.
struct MultiplayerLocations {
    uint8_t** session = nullptr;
    size_t flag = 0;
};

//   +0 mov rax,[rip+session]  +7 test rax,rax  +12 cmp byte [rax+flag],0
constexpr char kIsMultiplayerTrigger[] =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 1B 80 B8 ?? ?? ?? ?? 00 74 12 0F B6 81 ?? ?? ?? ?? BA 01 00 00 00 3B D0 0F 94 C0 C3";

inline bool LocateMultiplayer(const Image& img, MultiplayerLocations& out, std::string& error) {
    const uint8_t* p = FindOne(img, "CIsMultiplayerTrigger::ActualEvaluate", kIsMultiplayerTrigger, error);
    if (!p)
        return false;
    out.session = reinterpret_cast<uint8_t**>(RelTarget(p + 3, p + 7));
    out.flag = static_cast<size_t>(Disp32(p + 14));
    return true;
}

} // namespace locate
