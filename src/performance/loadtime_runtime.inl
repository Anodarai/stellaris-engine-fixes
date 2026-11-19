// [speed] savings while the game loads (re/load_time.md): token strings (fast_token_rebuild),
// modifier documentation (skip_modifier_docs), deferred references (fast_deferred_unregister) and
// file lookups (fast_file_lookups, pathcache.h). Included by dllmain.cpp inside its anonymous
// namespace, after simulation_runtime.inl.

// ---- Token strings ----
//
// CStaticLexer keeps a table of every token's text. GetString, and code that inlines it, calls
// RebuildLookup whenever the table's size differs from the token count, and RebuildLookup empties
// and refills the whole table. Script parsing adds dynamic tokens and asks for strings in turn, so
// each new token costs a pass over all of them (re/load_time.md).
//
// Tokens are only added at the end, and a token's text doesn't change, so once the table exists the
// hook grows it and fills in the new tokens. A table that should shrink goes to the game's function.
//
// [speed] fast_token_rebuild=0 turns this off; fast_token_rebuild_verify=1 also runs the game's
// function on every 16th call and logs any string that differs.

locate::LexerLocations g_lexer;
game::RebuildLookupFn g_origRebuild = nullptr;
bool g_lexerVerify = false;
devstats::Counter g_lexerFull{0}, g_lexerNew{0}, g_lexerCompared{0};
std::atomic<uint64_t> g_lexerIncremental{0}, g_lexerMismatches{0}; // pick the verified calls; limit the verify log

void __fastcall RebuildLookupHook() {
    auto instance = reinterpret_cast<game::LexerInstanceFn>(g_lexer.instance);
    uint8_t* lexer = instance();
    int have = At<int32_t>(lexer, g_lexer.count);
    int wanted = At<int32_t>(lexer, g_lexer.wanted);
    if (have <= 0 || wanted < have) {
        g_lexerFull.fetch_add(1, std::memory_order_relaxed);
        g_origRebuild();
        return;
    }
    reinterpret_cast<game::SetSizeFn>(g_lexer.setSize)(lexer + g_lexer.array, wanted);
    auto assign = reinterpret_cast<game::AssignFn>(g_lexer.assign);
    auto strings = At<game::String*>(lexer, g_lexer.data);
    auto tokens = At<uint8_t**>(lexer, g_lexer.dynData);
    for (int i = At<int32_t>(lexer, g_lexer.dynCount) - 1; i >= 0; --i) {
        int id = At<int32_t>(tokens[i], 0);
        if (id < have)
            break;
        const char* name = At<const char*>(tokens[i], g_lexer.tokenName);
        assign(&strings[id], name, strlen(name));
        g_lexerNew.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t call = g_lexerIncremental.fetch_add(1, std::memory_order_relaxed);
    if (!g_lexerVerify || call % 16 != 0)
        return;
    std::vector<std::string> mine;
    mine.reserve(static_cast<size_t>(wanted));
    for (int i = 0; i < wanted; ++i)
        mine.emplace_back(strings[i].data(), strings[i].size);
    g_origRebuild();
    strings = At<game::String*>(lexer, g_lexer.data);
    g_lexerCompared.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < wanted; ++i) {
        if (mine[i].size() == strings[i].size && memcmp(mine[i].data(), strings[i].data(), mine[i].size()) == 0)
            continue;
        if (g_lexerMismatches.fetch_add(1) < 20) {
            Log(LogLevel::Error, "token_strings", "MISMATCH: token %d: hook \"%s\", game \"%.*s\"", i, mine[i].c_str(),
                static_cast<int>(strings[i].size), strings[i].data());
        }
    }
}

void InstallLexer(HMODULE self, const locate::Image& img) {
    if (!IniOn(self, "speed", "fast_token_rebuild", true)) {
        Log(LogLevel::Info, "token_strings", "off (fast_token_rebuild=0)");
        return;
    }
    g_lexerVerify = IniOn(self, "speed", "fast_token_rebuild_verify", false);
    std::string error;
    if (!locate::LocateLexer(img, g_lexer, error)) {
        Log(LogLevel::Error, "token_strings", "%s; off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "token_strings",
        "RebuildLookup +%llX; table +0x%zx (data +0x%zx, size +0x%zx), wanted +0x%zx, dynamic +0x%zx/+0x%zx",
        Rva(g_lexer.rebuild), g_lexer.array, g_lexer.data, g_lexer.count, g_lexer.wanted, g_lexer.dynData,
        g_lexer.dynCount);
    auto tramp = InstallJumpHook(g_lexer.rebuild, locate::kRebuildLookupStolenBytes,
                                 reinterpret_cast<void*>(&RebuildLookupHook));
    if (!tramp) {
        Log(LogLevel::Error, "token_strings", "failed to hook RebuildLookup; off");
        return;
    }
    g_origRebuild = reinterpret_cast<game::RebuildLookupFn>(tramp);
    Log(LogLevel::Info, "token_strings", "hook installed%s",
        g_lexerVerify ? "; verifying every 16th call against the game's" : "");
}

#ifdef SEF_DEV_STATS
void LogLexerStats() {
    if (!g_origRebuild)
        return;
    Log(LogLevel::Stats, "token_strings",
        "full_rebuilds=%llu incremental_rebuilds=%llu tokens_added=%llu "
        "verify=%d compared=%llu mismatches=%llu",
        g_lexerFull.load(), g_lexerIncremental.load(), g_lexerNew.load(), g_lexerVerify, g_lexerCompared.load(),
        g_lexerMismatches.load());
}
#endif

// ---- Modifier documentation ----
//
// CModifier::LogDefinitions builds the text of logs/script_documentation/modifiers.log, one very
// long message, and logs it: seconds of the load, and nothing in the game reads it. The file is
// a reference for modders.
//
// [speed] skip_modifier_docs=0 writes it as usual.

void __fastcall LogDefinitionsHook() {
}

void InstallModifierDocs(HMODULE self, const locate::Image& img) {
    if (!IniOn(self, "speed", "skip_modifier_docs", true)) {
        Log(LogLevel::Info, "modifier_docs", "written as usual (skip_modifier_docs=0)");
        return;
    }
    locate::ModifierDocsLocations docs;
    std::string error;
    if (!locate::LocateModifierDocs(img, docs, error)) {
        Log(LogLevel::Error, "modifier_docs", "%s; written as usual", error.c_str());
        return;
    }
    if (!InstallJumpHook(docs.logDefinitions, locate::kLogDefinitionsStolenBytes,
                         reinterpret_cast<void*>(&LogDefinitionsHook))) {
        Log(LogLevel::Error, "modifier_docs", "failed to hook LogDefinitions; written as usual");
        return;
    }
    Log(LogLevel::Info, "modifier_docs", "LogDefinitions +%llX skipped; modifiers.log stays empty",
        Rva(docs.logDefinitions));
}

// ---- Deferred references ----
//
// While the databases load, every object that destroys a CGlobalDeferredDatabaseObject scans the
// whole list of deferred references for entries it owns (Unregister), and nearly all own none
// (re/load_time.md). Only Register_Internal adds to the list, so an owner it never saw has no
// entries, and Unregister would do nothing for it.
//
// The set of owners only has to contain every owner in the list; an owner in it needlessly costs
// the game's scan. Register_Internal adds after the game's function, and the set is emptied only
// when, under the lock, the list is empty: an entry added meanwhile can't be lost.

locate::DeferredRefLocations g_deferred;
game::DeferredRegisterFn g_origDeferredRegister = nullptr;
game::DeferredUnregisterFn g_origDeferredUnregister = nullptr;
SRWLOCK g_deferredLock = SRWLOCK_INIT;
std::unordered_set<const void*>* g_deferredOwners = nullptr; // never freed: objects die in the exe's exit
bool g_deferredVerify = false;
devstats::Counter g_deferredRegistered{0};
devstats::Counter g_deferredCalls{0};
devstats::Counter g_deferredSkipped{0};
std::atomic<uint64_t> g_deferredMismatches{0};

void __fastcall DeferredRegisterHook(uint8_t* resolver, const void* owner, void* a, void* b, void* c) {
    g_origDeferredRegister(resolver, owner, a, b, c);
    g_deferredRegistered.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_deferredLock);
    g_deferredOwners->insert(owner);
    ReleaseSRWLockExclusive(&g_deferredLock);
}

int DeferredCount(const uint8_t* resolver) {
    return *reinterpret_cast<const int32_t*>(resolver + g_deferred.count);
}

void __fastcall DeferredUnregisterHook(uint8_t* resolver, const void* owner) {
    g_deferredCalls.fetch_add(1, std::memory_order_relaxed);
    if (DeferredCount(resolver) <= 0) {
        g_deferredSkipped.fetch_add(1, std::memory_order_relaxed);
        AcquireSRWLockShared(&g_deferredLock);
        bool stale = !g_deferredOwners->empty();
        ReleaseSRWLockShared(&g_deferredLock);
        if (stale) {
            AcquireSRWLockExclusive(&g_deferredLock);
            if (DeferredCount(resolver) <= 0)
                g_deferredOwners->clear();
            ReleaseSRWLockExclusive(&g_deferredLock);
        }
        return;
    }
    AcquireSRWLockShared(&g_deferredLock);
    bool known = g_deferredOwners->count(owner) != 0;
    ReleaseSRWLockShared(&g_deferredLock);
    if (known) {
        g_origDeferredUnregister(resolver, owner);
        return;
    }
    if (g_deferredVerify) {
        const uint8_t* data = *reinterpret_cast<uint8_t* const*>(resolver + g_deferred.data);
        for (int i = DeferredCount(resolver) - 1; i >= 0; --i) {
            if (*reinterpret_cast<const void* const*>(data + i * g_deferred.entrySize + g_deferred.owner) == owner) {
                if (g_deferredMismatches.fetch_add(1, std::memory_order_relaxed) == 0) {
                    Log(LogLevel::Error, "deferred_refs", "MISMATCH, owner %p has an entry the set doesn't know",
                        owner);
                }
                g_origDeferredUnregister(resolver, owner);
                return;
            }
        }
    }
    g_deferredSkipped.fetch_add(1, std::memory_order_relaxed);
}

void InstallDeferredRefs(HMODULE self, const locate::Image& img) {
    if (!IniOn(self, "speed", "fast_deferred_unregister", true)) {
        Log(LogLevel::Info, "deferred_refs", "off (fast_deferred_unregister=0)");
        return;
    }
    g_deferredVerify = IniOn(self, "speed", "fast_deferred_unregister_verify", false);
    std::string error;
    if (!locate::LocateDeferredRefs(img, g_deferred, error)) {
        Log(LogLevel::Error, "deferred_refs", "%s; off", error.c_str());
        return;
    }
    Log(LogLevel::Debug, "deferred_refs",
        "Register_Internal +%llX, Unregister +%llX; list +0x%zx/+0x%zx, entries 0x%zx bytes, owner +0x%zx",
        Rva(g_deferred.registerRef), Rva(g_deferred.unregister), g_deferred.data, g_deferred.count,
        g_deferred.entrySize, g_deferred.owner);
    g_deferredOwners = new std::unordered_set<const void*>();
    // Nothing has registered yet at this point, so the set starts complete. Register first: with
    // only Unregister hooked, an owner it hasn't seen would wrongly be skipped.
    auto reg = InstallJumpHook(g_deferred.registerRef, locate::kDeferredRegisterStolenBytes,
                               reinterpret_cast<void*>(&DeferredRegisterHook));
    if (!reg) {
        Log(LogLevel::Error, "deferred_refs", "failed to hook Register_Internal; off");
        return;
    }
    g_origDeferredRegister = reinterpret_cast<game::DeferredRegisterFn>(reg);
    auto unreg = InstallJumpHook(g_deferred.unregister, locate::kDeferredUnregisterStolenBytes,
                                 reinterpret_cast<void*>(&DeferredUnregisterHook));
    if (!unreg) {
        Log(LogLevel::Error, "deferred_refs", "failed to hook Unregister; off");
        return;
    }
    g_origDeferredUnregister = reinterpret_cast<game::DeferredUnregisterFn>(unreg);
    Log(LogLevel::Info, "deferred_refs", "hooks installed%s",
        g_deferredVerify ? ", verifying every skip against the list" : "");
}

#ifdef SEF_DEV_STATS
void LogDeferredStats() {
    if (!g_origDeferredUnregister)
        return;
    uint64_t calls = g_deferredCalls.load(), skipped = g_deferredSkipped.load();
    Log(LogLevel::Stats, "deferred_refs",
        "registered=%llu unregisters=%llu skipped=%llu skipped_pct=%.1f "
        "verify=%d mismatches=%llu",
        g_deferredRegistered.load(), calls, skipped, calls ? 100.0 * skipped / calls : 0.0, g_deferredVerify,
        g_deferredMismatches.load());
}
#endif

// ---- File lookups ----
//
// The game's virtual file system (PhysFS) tries each mounted folder in turn, so a load makes over a
// million CreateFileW and GetFileAttributesExW calls that fail, nearly all because the folder doesn't
// exist in that mod (re/load_time.md). pathcache.h answers those from folder listings. The hooks
// sit in the exe's import table, so they need no patterns and see only the game's own calls.
//
// Only the game folder, workshop mods and the user folder's mod folder are answered: the rest of the
// user folder changes as the game runs. Calls from the exe that may change a folder drop its listing.
//
// [speed] fast_file_lookups=0 turns this off; fast_file_lookups_verify=1 also asks the file system
// for every answer, returns what it says, and logs any difference.

pathcache::Cache g_files;
bool g_filesVerify = false;
bool g_filesOn = false;
devstats::Counter g_fileLookups{0}, g_fileAnswered{0}, g_fileCompared{0};
std::atomic<uint64_t> g_fileMismatches{0}; // limits the verify log

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetFileAttributesExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using CreateDirectoryWFn = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using PathOnlyFn = BOOL(WINAPI*)(LPCWSTR);
using MoveFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using MoveFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
CreateFileWFn g_origCreateFileW = nullptr;
GetFileAttributesExWFn g_origGetAttributes = nullptr;
CreateDirectoryWFn g_origCreateDirectoryW = nullptr;
PathOnlyFn g_origDeleteFileW = nullptr, g_origRemoveDirectoryW = nullptr;
MoveFileExWFn g_origMoveFileExW = nullptr;
MoveFileWFn g_origMoveFileW = nullptr;

// The cache's error for a path, or 0 to ask the file system.
DWORD MissingError(LPCWSTR path) {
    g_fileLookups.fetch_add(1, std::memory_order_relaxed);
    switch (g_files.Query(path)) {
    case pathcache::Answer::MissingFile:
        return ERROR_FILE_NOT_FOUND;
    case pathcache::Answer::MissingPath:
        return ERROR_PATH_NOT_FOUND;
    default:
        return 0;
    }
}

std::string LogPath(LPCWSTR path) {
    int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (!size)
        return "[unavailable path]";
    std::string text(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, text.data(), size, nullptr, nullptr)) {
        return "[unavailable path]";
    }
    text.pop_back(); // The size returned by Windows includes the terminator.
    return text;
}

void CompareFileAnswer(LPCWSTR path, DWORD cached, bool found, DWORD real) {
    g_fileCompared.fetch_add(1, std::memory_order_relaxed);
    if ((found || real != cached) && g_fileMismatches.fetch_add(1) < 20) {
        Log(LogLevel::Error, "file_lookups", "MISMATCH: cache says error %lu, file system says %s %lu: %s", cached,
            found ? "found" : "error", found ? 0 : real, LogPath(path).c_str());
    }
}

HANDLE WINAPI CreateFileWHook(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE templateFile) {
    constexpr DWORD kWrites = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    if (!path)
        return g_origCreateFileW(path, access, share, security, disposition, flags, templateFile);
    if (disposition != OPEN_EXISTING || (access & kWrites)) {
        HANDLE h = g_origCreateFileW(path, access, share, security, disposition, flags, templateFile);
        DWORD error = GetLastError();
        g_files.Invalidate(path);
        SetLastError(error);
        return h;
    }
    DWORD cached = MissingError(path);
    if (cached == 0)
        return g_origCreateFileW(path, access, share, security, disposition, flags, templateFile);
    if (g_filesVerify) {
        HANDLE h = g_origCreateFileW(path, access, share, security, disposition, flags, templateFile);
        DWORD error = GetLastError();
        CompareFileAnswer(path, cached, h != INVALID_HANDLE_VALUE, error);
        SetLastError(error);
        return h;
    }
    g_fileAnswered.fetch_add(1, std::memory_order_relaxed);
    SetLastError(cached);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI GetAttributesHook(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID out) {
    if (!path)
        return g_origGetAttributes(path, level, out);
    DWORD cached = MissingError(path);
    if (cached == 0)
        return g_origGetAttributes(path, level, out);
    if (g_filesVerify) {
        BOOL ok = g_origGetAttributes(path, level, out);
        DWORD error = GetLastError();
        CompareFileAnswer(path, cached, ok != FALSE, error);
        SetLastError(error);
        return ok;
    }
    g_fileAnswered.fetch_add(1, std::memory_order_relaxed);
    SetLastError(cached);
    return FALSE;
}

// The calls that change a folder: run them, then forget the listings of the paths they touched.
void InvalidateKeepingError(LPCWSTR a, LPCWSTR b = nullptr) {
    DWORD error = GetLastError();
    if (a)
        g_files.Invalidate(a);
    if (b)
        g_files.Invalidate(b);
    SetLastError(error);
}
BOOL WINAPI CreateDirectoryWHook(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    BOOL r = g_origCreateDirectoryW(path, security);
    InvalidateKeepingError(path);
    return r;
}
BOOL WINAPI DeleteFileWHook(LPCWSTR path) {
    BOOL r = g_origDeleteFileW(path);
    InvalidateKeepingError(path);
    return r;
}
BOOL WINAPI RemoveDirectoryWHook(LPCWSTR path) {
    BOOL r = g_origRemoveDirectoryW(path);
    InvalidateKeepingError(path);
    return r;
}
BOOL WINAPI MoveFileExWHook(LPCWSTR from, LPCWSTR to, DWORD flags) {
    BOOL r = g_origMoveFileExW(from, to, flags);
    InvalidateKeepingError(from, to);
    return r;
}
BOOL WINAPI MoveFileWHook(LPCWSTR from, LPCWSTR to) {
    BOOL r = g_origMoveFileW(from, to);
    InvalidateKeepingError(from, to);
    return r;
}

// Points the exe's import of a KERNEL32 function at `hook`. False if the exe doesn't import it.
bool HookKernel32Import(const char* function, void* hook, void** original) {
    auto base = reinterpret_cast<uint8_t*>(g_base);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return false;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "KERNEL32.dll") != 0 || !d->OriginalFirstThunk)
            continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(byName->Name, function) != 0)
                continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                return false;
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

void InstallFileLookups(HMODULE self) {
    if (!IniOn(self, "speed", "fast_file_lookups", true)) {
        Log(LogLevel::Info, "file_lookups", "off (fast_file_lookups=0)");
        return;
    }
    g_filesVerify = IniOn(self, "speed", "fast_file_lookups_verify", false);
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        Log(LogLevel::Error, "file_lookups", "no exe path; off");
        return;
    }
    if (wchar_t* slash = wcsrchr(exe, L'\\'))
        *slash = 0;
    g_files.AddRoot(exe);
    g_files.AddRootMarker(L"\\workshop\\content\\281990\\");
    g_files.AddRootMarker(L"\\paradox interactive\\stellaris\\mod\\");
    // The calls that change folders first: nothing is answered until the two lookups are hooked.
    struct Import {
        const char* name;
        void* hook;
        void** original;
    };
    const Import writers[] = {
        {"CreateDirectoryW", reinterpret_cast<void*>(&CreateDirectoryWHook),
         reinterpret_cast<void**>(&g_origCreateDirectoryW)},
        {"DeleteFileW", reinterpret_cast<void*>(&DeleteFileWHook), reinterpret_cast<void**>(&g_origDeleteFileW)},
        {"RemoveDirectoryW", reinterpret_cast<void*>(&RemoveDirectoryWHook),
         reinterpret_cast<void**>(&g_origRemoveDirectoryW)},
        {"MoveFileExW", reinterpret_cast<void*>(&MoveFileExWHook), reinterpret_cast<void**>(&g_origMoveFileExW)},
        {"MoveFileW", reinterpret_cast<void*>(&MoveFileWHook), reinterpret_cast<void**>(&g_origMoveFileW)},
    };
    int writerHooks = 0;
    for (const Import& i : writers)
        writerHooks += HookKernel32Import(i.name, i.hook, i.original);
    // CreateFileW must be in place for the cache to hear of the game's writes: without it, nothing
    // is answered, and GetFileAttributesExW isn't touched.
    if (!HookKernel32Import("CreateFileW", reinterpret_cast<void*>(&CreateFileWHook),
                            reinterpret_cast<void**>(&g_origCreateFileW))) {
        Log(LogLevel::Error, "file_lookups", "CreateFileW isn't imported; off");
        return;
    }
    if (!HookKernel32Import("GetFileAttributesExW", reinterpret_cast<void*>(&GetAttributesHook),
                            reinterpret_cast<void**>(&g_origGetAttributes))) {
        Log(LogLevel::Warn, "file_lookups", "GetFileAttributesExW isn't imported; only opens are answered");
    }
    g_filesOn = true;
    Log(LogLevel::Info, "file_lookups", "import hooks installed (%d of 5 calls that change folders); game folder %s%s",
        writerHooks, LogPath(exe).c_str(), g_filesVerify ? "; verifying every answer against the file system" : "");
}

#ifdef SEF_DEV_STATS
void LogFileLookupStats() {
    if (!g_filesOn)
        return;
    uint64_t lookups = g_fileLookups.load();
    uint64_t answered = g_filesVerify ? g_fileCompared.load() : g_fileAnswered.load();
    Log(LogLevel::Stats, "file_lookups",
        "lookups=%llu answered_missing=%llu answered_missing_pct=%.1f folder_listings=%llu "
        "verify=%d mismatches=%llu",
        lookups, answered, lookups ? 100.0 * answered / lookups : 0.0, g_files.Listings(), g_filesVerify,
        g_fileMismatches.load());
}
#endif
