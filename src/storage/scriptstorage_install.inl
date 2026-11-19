// Part of scriptstorage_runtime.inl: the hooks on saves, loads and new games and on the game's log
// effect and trigger, then the installers.

// ---- Script storage save hooks ----

locate::SaveLocations g_saves;
using StartSaveFn = uint32_t(__fastcall*)(void* manager, const game::String* path, bool a, bool b, bool c);
using GameStateLoadFn = void(__fastcall*)(void* state, void* file, const game::String* path);
using NewGameFn = void(__fastcall*)(void* state);
StartSaveFn g_origStartSave = nullptr;
GameStateLoadFn g_origLoad = nullptr;
NewGameFn g_origNewGame = nullptr;

uint32_t __fastcall StartSaveHook(void* manager, const game::String* path, bool a, bool b, bool c) {
    uint32_t result = g_origStartSave(manager, path, a, b, c);
    if (result == 0)
        StoreSaveStarted(manager); // 0: the save has started
    return result;
}

void __fastcall LoadHook(void* state, void* file, const game::String* path) {
    char text[1024];
    bool read = CopySavePath(path, text, sizeof(text));
    if (read)
        StoreLoading(text); // before the load, which may already run script
    g_origLoad(state, file, path);
}

void __fastcall NewGameHook(void* state) {
    StoreNewGame(); // before on_game_start runs
    g_origNewGame(state);
}

// Hook saves, loads and new games for the save store.
bool InstallSaves(const locate::Image& img) {
    std::string error;
    if (!locate::LocateSaves(img, g_saves, error)) {
        Log(LogLevel::Error, "save_hooks", "%s; save storage is off", error.c_str());
        return false;
    }
    // Install save tracking after the load and new-game hooks.
    auto newGame =
        InstallJumpHook(g_saves.newGame, locate::kNewGameStartedStolenBytes, reinterpret_cast<void*>(&NewGameHook));
    auto load =
        newGame ? InstallJumpHook(g_saves.load, locate::kGameStateLoadStolenBytes, reinterpret_cast<void*>(&LoadHook))
                : nullptr;
    if (load)
        g_origLoad = reinterpret_cast<GameStateLoadFn>(load);
    if (newGame)
        g_origNewGame = reinterpret_cast<NewGameFn>(newGame);
    auto save = load ? InstallJumpHook(g_saves.startSave, locate::kStartSaveToPathStolenBytes,
                                       reinterpret_cast<void*>(&StartSaveHook))
                     : nullptr;
    if (!save) {
        Log(LogLevel::Error, "save_hooks", "failed to hook saving and loading; save storage is off");
        return false;
    }
    g_origStartSave = reinterpret_cast<StartSaveFn>(save);
    Log(LogLevel::Debug, "save_hooks", "hooks: StartSaveToPath +%llX, CGameState::Load +%llX, OnNewGameStarted +%llX",
        Rva(g_saves.startSave), Rva(g_saves.load), Rva(g_saves.newGame));
    return true;
}

// ---- Script storage log hooks ----

locate::LogScriptLocations g_storeLogs;
using LogEffectFn = void(__fastcall*)(uint8_t* effect, uint8_t* scope);
using LogTriggerFn = bool(__fastcall*)(uint8_t* trigger, uint8_t* scope);
LogEffectFn g_origLogEffect = nullptr;
LogTriggerFn g_origLogTrigger = nullptr;

// Copy the command following the storage prefix, or leave the log call to the game.
bool ReadStoreCommand(const uint8_t* object, size_t textOffset, char* buf, size_t size) {
    constexpr char prefix[] = "@sef:";
    constexpr size_t prefixSize = sizeof(prefix) - 1;
    __try {
        const auto* text = reinterpret_cast<const game::String*>(object + textOffset);
        if (text->size < prefixSize || text->size >= size || memcmp(text->data(), prefix, prefixSize) != 0)
            return false;
        size_t length = text->size - prefixSize;
        memcpy(buf, text->data() + prefixSize, length);
        buf[length] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall LogEffectHook(uint8_t* effect, uint8_t* scope) {
    char buf[1024];
    if (g_storeOn && ReadStoreCommand(effect, g_storeLogs.logEffectText, buf, sizeof(buf)))
        return StoreEffect(buf, scope);
    g_origLogEffect(effect, scope);
}

bool __fastcall LogTriggerHook(uint8_t* trigger, uint8_t* scope) {
    char buf[1024];
    if (g_storeOn && ReadStoreCommand(trigger, g_storeLogs.logTriggerText, buf, sizeof(buf)))
        return StoreTrigger(buf, scope);
    return g_origLogTrigger(trigger, scope);
}

bool HookLogs() {
    uint8_t* e =
        InstallJumpHook(g_storeLogs.logEffect, locate::kLogScriptStolenBytes, reinterpret_cast<void*>(&LogEffectHook));
    if (!e)
        return false;
    g_origLogEffect = reinterpret_cast<LogEffectFn>(e);
    // If the trigger hook fails, the effect hook still forwards to the game.
    uint8_t* t = InstallJumpHook(g_storeLogs.logTrigger, locate::kLogScriptStolenBytes,
                                 reinterpret_cast<void*>(&LogTriggerHook));
    if (!t)
        return false;
    g_origLogTrigger = reinterpret_cast<LogTriggerFn>(t);
    return true;
}

// ---- Installers ----

bool InstallStoreSaveWrite(const locate::Image& img) {
    std::string error;
    if (!locate::LocateSaveWrite(img, g_saves, g_saveWrite, error)) {
        Log(LogLevel::Error, "script_storage", "%s", error.c_str());
        return false;
    }
    g_origDestroySaveArchive = reinterpret_cast<decltype(g_origDestroySaveArchive)>(g_saveWrite.destroyArchive);
    g_origCloseSaveFile = reinterpret_cast<decltype(g_origCloseSaveFile)>(g_saveWrite.closeFile);
    g_origCloudSaveWrite = reinterpret_cast<decltype(g_origCloudSaveWrite)>(g_saveWrite.cloudWrite);
    g_origSaveAddFile = reinterpret_cast<decltype(g_origSaveAddFile)>(g_saveWrite.addFile);
    if (!RedirectCall(g_saveWrite.closeCall, reinterpret_cast<const void*>(&StoreCloseSaveFileHook)) ||
        !RedirectCall(g_saveWrite.cloudCall, reinterpret_cast<const void*>(&StoreCloudSaveWriteHook)))
        return false;
    for (uint8_t* call : g_saveWrite.archiveDestroyCalls) {
        if (!RedirectCall(call, reinterpret_cast<const void*>(&StoreDestroySaveArchiveHook)))
            return false;
    }
    for (uint8_t* call : g_saveWrite.addFileCalls) {
        if (!RedirectCall(call, reinterpret_cast<const void*>(&StoreSaveAddFileHook)))
            return false;
    }
    // Until this last hook installs, the call redirects only forward to the game.
    auto* zip = InstallJumpHook(g_saveWrite.zipAndWrite, locate::kZipAndWriteStolenBytes,
                                reinterpret_cast<void*>(&StoreZipAndWriteHook));
    g_origZipAndWrite = reinterpret_cast<decltype(g_origZipAndWrite)>(zip);
    return zip != nullptr;
}

// [script_storage] multiplayer=1: the host's stores go to every player (mpstore.h).
void InstallMpStore(const std::string& ini, const locate::Image& img) {
    if (ini.empty() || GetPrivateProfileIntA("script_storage", "multiplayer", 0, ini.c_str()) == 0) {
        Log(LogLevel::Info, "script_storage", "off in multiplayer (multiplayer=0)");
        return;
    }
    std::string error;
    if (!mpstore::Locate(img, g_mpStore, error)) {
        Log(LogLevel::Error, "script_storage", "%s; off in multiplayer although multiplayer=1", error.c_str());
        return;
    }
    auto init = InstallJumpHook(g_mpStore.sessionInit, mpstore::kSessionInitStolenBytes,
                                reinterpret_cast<void*>(&MpSessionInitHook));
    if (!init) {
        Log(LogLevel::Error, "script_storage",
            "failed to hook CSession::Init; off in multiplayer although multiplayer=1");
        return;
    }
    g_origSessionInit = reinterpret_cast<decltype(g_origSessionInit)>(init);
    // The two jump hooks only forward to the game until g_mpStoreOn is set.
    auto sync =
        InstallJumpHook(g_mpStore.startSync, mpstore::kStartSyncStolenBytes, reinterpret_cast<void*>(&MpStartSyncHook));
    if (!sync) {
        Log(LogLevel::Error, "script_storage",
            "failed to hook CGameSetup::StartSyncPlayers; off in multiplayer although multiplayer=1");
        return;
    }
    g_origStartSync = reinterpret_cast<decltype(g_origStartSync)>(sync);
    auto send =
        InstallJumpHook(g_mpStore.startSend, mpstore::kStartSendStolenBytes, reinterpret_cast<void*>(&MpStartSendHook));
    if (!send) {
        Log(LogLevel::Error, "script_storage",
            "failed to hook CLargeFileHandler::StartSend; off in multiplayer although multiplayer=1");
        return;
    }
    g_origStartSend = reinterpret_cast<decltype(g_origStartSend)>(send);
    g_origChatExecute = reinterpret_cast<decltype(g_origChatExecute)>(g_mpStore.chatExecute);
    if (!SwapVtableSlot(g_mpStore.chatExecuteSlot, g_mpStore.chatExecute,
                        reinterpret_cast<void*>(&MpChatExecuteHook))) {
        Log(LogLevel::Error, "script_storage",
            "failed to hook CChatCommand::Execute; off in multiplayer although multiplayer=1");
        return;
    }
    g_mpStoreOn = true;
    Log(LogLevel::Info, "script_storage",
        "on in multiplayer, the host sends its stores; chat command +%llX, Execute +%llX, Post +%llX, "
        "StartSend +%llX, StartSyncPlayers +%llX, operator new +%llX, host flag +0x%zx",
        Rva(g_mpStore.chatCtor), Rva(g_mpStore.chatExecute), Rva(g_mpStore.post), Rva(g_mpStore.startSend),
        Rva(g_mpStore.startSync), Rva(g_mpStore.operatorNew), g_mpStore.hostFlag);
}

void InstallScriptStorage(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (!ini.empty() && GetPrivateProfileIntA("script_storage", "enabled", 1, ini.c_str()) == 0) {
        Log(LogLevel::Info, "script_storage", "off (enabled=0)");
        return;
    }
    std::string error;
    locate::LexerLocations lexer;
    if (!locate::LocateVariables(img, g_vars, error) || !locate::LocateLexer(img, lexer, error) ||
        !locate::LocateMultiplayer(img, g_mp, error) ||
        !locate::LocateLogScript(img, g_vars.effectName, g_vars.triggerName, g_storeLogs, error)) {
        Log(LogLevel::Error, "script_storage", "%s; off", error.c_str());
        return;
    }
    g_storeAssign = lexer.assign;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    const wchar_t* slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (!slash)
        return;
    g_storeDir = std::wstring(path, static_cast<size_t>(slash - path)) + L"\\sefdata";

    // The text call: a near stub to LogTextHook, then the call's rel32 pointed at it.
    uint8_t* call = g_storeLogs.textCall;
    Code code;
    code.JumpTo(reinterpret_cast<const void*>(&LogTextHook));
    uint8_t* stub = PlaceStub(code, call);
    int32_t rel = 0;
    if (!stub || call[0] != 0xE8 || !Rel32(call + 5, stub, &rel)) {
        Log(LogLevel::Error, "script_storage", "no room for the log text stub; off");
        return;
    }
    g_origProcessString = reinterpret_cast<ProcessStringFn>(g_storeLogs.processString);
    if (!HookLogs()) {
        Log(LogLevel::Error, "script_storage", "failed to hook CLogEffect or CLogTrigger; off");
        return;
    }
    if (!WriteCode(call + 1, &rel, sizeof(rel))) {
        Log(LogLevel::Error, "script_storage", "failed to redirect the log text call; off");
        return;
    }
    if (!InstallSaves(img) || !InstallStoreSaveWrite(img)) {
        Log(LogLevel::Warn, "script_storage", "save persistence is off; save hooks could not be installed");
    }
    uint8_t* readExecute = nullptr;
    if (!g_bench.gameState || !g_origCheckTick) {
        Log(LogLevel::Warn, "script_storage", "no tick hook; run is off");
    } else if (!locate::LocateEffectCommand(img, g_bench.gameState, &g_storeEffectCommand, &readExecute, error)) {
        g_storeEffectCommand = nullptr;
        Log(LogLevel::Warn, "script_storage", "%s; run is off", error.c_str());
    }
    g_storeOn = true;
    Log(LogLevel::Info, "script_storage",
        "on; log = \"@sef:...\"; variables: access +%llX, get +%llX, set +%llX, map +0x%zx (value +0x%zx); "
        "log text call +%llX; multiplayer flag +0x%zx; data in sefdata\\",
        Rva(g_vars.access), Rva(g_vars.get), Rva(g_vars.set), g_vars.map, g_vars.entryValue, Rva(call), g_mp.flag);
    InstallMpStore(ini, img);
}
