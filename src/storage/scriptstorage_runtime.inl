// Script storage ([script_storage] in stellaris-engine-fixes.ini; re/script_storage.md).
//
// Paradox script keeps data outside the game without C#, through log = "@sef:..." effects and
// triggers: numbers from variables, and text recorded from log lines, which `run` executes later.
// Two stores, global (shared by every game, sefdata\global.txt) and save (sefdata\saves\<folder>\
// <name>.txt, written once a save has succeeded, read when that save loads, empty for a new game).
// In multiplayer each machine has its own files, so storage is off there unless multiplayer=1,
// which has the host send its stores to every player (mpstore_runtime.inl).
//
// In this file: the stores' state and shared helpers. The rest is in parts, included in this order:
//   starbasetemplates_sidecar.inl  the starbase features' per-save state (interface/starbase/)
//   mpstore_runtime.inl            the multiplayer copies (mpstore.h)
//   scriptstorage_commands.inl     the @sef: commands, variables, recording and run
//   scriptstorage_lifecycle.inl    the frame pass, saves, loads, new games and exit
//   scriptstorage_install.inl      the save, load and log hooks, then the installers
// dllmain.cpp includes it inside its anonymous namespace, after aifixes_runtime.inl. It uses
// dllmain.cpp's hooking helpers (InstallJumpHook, RedirectCall, PlaceStub, WriteCode), its string
// helpers (CopyGameString, FreeGameString, CopySavePath) and the tick hook's locations (g_bench).

bool g_storeOn = false; // set by InstallScriptStorage once the log hooks are in place
locate::VariableLocations g_vars;
locate::MultiplayerLocations g_mp;
uint8_t* g_storeAssign = nullptr;        // CString::assign(string, text, length)
uint8_t* g_storeEffectCommand = nullptr; // OnExecute_Effect, for `run`; null: run is off
std::wstring g_storeDir;                 // sefdata\ in the game folder

std::mutex g_storeLock; // the stores, the recording and the run queue; effects may run on worker threads
pdxstore::Store g_globalStore, g_saveStore;

#include "../interface/starbase/starbasetemplates_sidecar.inl"

bool g_globalLoaded = false;
ULONGLONG g_globalWritten = 0;

struct PendingStoreSave {
    std::string key;
    pdxstore::Store snapshot;
};
std::map<void*, PendingStoreSave> g_pendingStoreSaves;
locate::SaveWriteLocations g_saveWrite;
void(__fastcall* g_origZipAndWrite)(void*, const game::String*) = nullptr;
void(__fastcall* g_origDestroySaveArchive)(void*) = nullptr;
int(__fastcall* g_origCloseSaveFile)(void*) = nullptr;
bool(__fastcall* g_origCloudSaveWrite)(void*, const void*, uint32_t) = nullptr;
bool(__fastcall* g_origSaveAddFile)(void*, void*, const game::String*, int, int) = nullptr;

struct SaveWriteStatus;
thread_local SaveWriteStatus* g_saveWriteStatus = nullptr;

struct SaveWriteStatus {
    explicit SaveWriteStatus(bool tracked) : previous(g_saveWriteStatus) {
        g_saveWriteStatus = tracked ? this : nullptr;
    }
    ~SaveWriteStatus() { g_saveWriteStatus = previous; }

    SaveWriteStatus* const previous;
    bool archiveOk = true;
    bool closingArchive = false;
    bool written = false;
};

struct StoreRecording {
    bool active = false;
    DWORD thread = 0;
    pdxstore::Which store = pdxstore::Which::Global;
    std::string key, text;
    int lines = 0, dropped = 0;
};
StoreRecording g_recording;
struct StoreRun {
    pdxstore::Which store;
    std::string key;
};
std::vector<StoreRun> g_storeRuns;

devstats::Counter g_storeSets{0}, g_storeGets{0}, g_storeTests{0}, g_storeRecorded{0}, g_storeRan{0};
std::atomic<uint64_t> g_storeErrors{0}; // limits the problem log

using ProcessStringFn = game::String*(__fastcall*)(void* text, game::String* out, void* in, void* a, void* b, void* c);
using AccessVariablesFn = uint8_t*(__fastcall*)(uint8_t* scope, const game::String* name);
using FindVariableFn = void*(__fastcall*)(uint8_t* map, uint8_t** entry, const game::String* key);
using SetVariableFn = void(__fastcall*)(uint8_t* variables, const game::String* name, int64_t value);
ProcessStringFn g_origProcessString = nullptr;

const char* StoreName(pdxstore::Which which) {
    return which == pdxstore::Which::Global ? "global" : "save";
}

// Logs at most 100 problems a session, so a script that errs every day doesn't fill the log.
void StoreProblem(const char* fmt, ...) {
    if (g_storeErrors.fetch_add(1) >= 100)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log(LogLevel::Error, "script_storage", "%s", buf);
}

bool StoreMultiplayer() {
    __try {
        const uint8_t* session = *g_mp.session;
        return session && session[g_mp.flag] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

#include "mpstore_runtime.inl"
#include "scriptstorage_commands.inl"
#include "scriptstorage_lifecycle.inl"
#include "scriptstorage_install.inl"
