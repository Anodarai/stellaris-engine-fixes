// Part of scriptstorage_runtime.inl: the stores over a session. The frame pass (queued runs, open
// recordings, the global store's writes, multiplayer retries), saving the save store next to a save
// once it succeeds, loading it with a save, emptying it for a new game, exit, and the dev-build stats.

// The host, while a snapshot the session refused or a request taken without the session is
// pending: post it, after 1 s and then twice as long each time up to 30 s. Storage is closed on
// every machine since the request, so the stores still hold what the request saw.
void MpRetrySnapshot() {
    uint64_t answers = 0;
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        ULONGLONG now = GetTickCount64();
        if (!g_mpRetryPending || now < g_mpRetryAt)
            return;
        g_mpRetryDelay = (std::min)(g_mpRetryDelay * 2, ULONGLONG(30000));
        g_mpRetryAt = now + g_mpRetryDelay;
        answers = g_mpRetryAnswers;
    }
    uint8_t* session = MpSession();
    bool host = false;
    int type = 0;
    if (!MpReadSession(session, &host, &type))
        return; // try again at the next deadline
    if (!host || g_mpSyncedSession.load()) {
        std::lock_guard<std::mutex> lock(g_storeLock);
        g_mpRetryPending = false; // no longer the host, or storage reopened: nothing to resend
        return;
    }
    if (!MpPostSnapshot(session, "posted from the frame after its request", answers))
        return;
    std::lock_guard<std::mutex> lock(g_storeLock);
    if (g_mpRetryAnswers == answers)
        g_mpRetryPending = false;
}

// Once per frame, on the main thread between ticks: runs what `run` queued, ends a recording left
// open, and writes the global store at most once a second.
void StoreFrame() {
    if (!g_storeOn)
        return;
    if (g_mpStoreOn)
        MpRetrySnapshot();
    std::vector<StoreRun> runs;
    std::vector<std::string> texts;
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        if (g_recording.active) {
            StoreProblem("record %s.%s: no record_end in the same frame; dropped", StoreName(g_recording.store),
                         g_recording.key.c_str());
            g_recording = StoreRecording();
        }
        runs.swap(g_storeRuns);
        for (const StoreRun& r : runs) {
            const std::string* stored = StoreFor(r.store).Get(r.key);
            texts.push_back(stored ? *stored : std::string());
        }
        if (g_globalStore.Dirty() && GetTickCount64() - g_globalWritten >= 1000)
            WriteGlobalStore();
    }
    if (runs.empty())
        return;
    if (!*g_bench.gameState) {
        StoreProblem("run: %zu queued with no game running; dropped", runs.size());
        return;
    }
    for (size_t i = 0; i < runs.size(); ++i) {
        const char* name = StoreName(runs[i].store);
        if (texts[i].empty()) {
            StoreProblem("run %s.%s: nothing stored", name, runs[i].key.c_str());
            continue;
        }
        if (StoreMultiplayer())
            continue;
        game::ConsoleResult result{};
        bool ran = RunStoredEffect(texts[i].c_str(), &result);
        g_storeRan.fetch_add(1, std::memory_order_relaxed);
        if (ran) {
            Log(LogLevel::Info, "script_storage", "ran %s.%s (%zu bytes)%s", name, runs[i].key.c_str(), texts[i].size(),
                result.ok ? "" : "; the game refused it, see error.log");
            FreeGameStringGuarded(&result.text);
        } else {
            StoreProblem("run %s.%s: the game faulted running it", name, runs[i].key.c_str());
        }
    }
}

// StartSaveToPath finishes any prior write before accepting a new one on this manager.
// Capture the accepted, normalized path before Update (or Ironman exit) starts the write.
void StoreSaveStarted(void* manager) {
    if (!g_storeOn)
        return;
    char path[1024];
    auto* params = static_cast<const uint8_t*>(manager) + g_saveWrite.paramsOffset;
    bool tracked = g_origZipAndWrite && MpRoleNow() != MpRole::Client &&
                   CopySavePath(reinterpret_cast<const game::String*>(params), path, sizeof(path));
    std::string key = tracked ? pdxstore::SaveKey(path, true) : std::string();
    std::lock_guard<std::mutex> lock(g_storeLock);
    if (!CaptureStarbaseTemplates()) {
        Log(LogLevel::Error, "starbase_templates",
            "template snapshot exceeds sidecar limits; save data was not updated");
        // Do not associate an older plan snapshot with this newer game save.
        g_pendingStoreSaves.erase(manager);
        return;
    }
    WriteGlobalStore();
    if (key.empty()) {
        g_pendingStoreSaves.erase(manager);
        return;
    }
    g_pendingStoreSaves.insert_or_assign(manager, PendingStoreSave{std::move(key), g_saveStore});
}

PendingStoreSave TakeStoreSave(void* manager, const game::String* path) {
    char text[1024];
    const std::string key = CopySavePath(path, text, sizeof(text)) ? pdxstore::SaveKey(text, true) : std::string();
    std::lock_guard<std::mutex> lock(g_storeLock);
    auto save = g_pendingStoreSaves.find(manager);
    if (save == g_pendingStoreSaves.end() || save->second.key != key)
        return {};
    PendingStoreSave pending = std::move(save->second);
    g_pendingStoreSaves.erase(save);
    return pending;
}

// Both the scheduled save task and the synchronous Ironman exit go through this writer.
void __fastcall StoreZipAndWriteHook(void* manager, const game::String* params) {
    PendingStoreSave save = TakeStoreSave(manager, params);
    SaveWriteStatus status(!save.key.empty());
    g_origZipAndWrite(manager, params);
    if (save.key.empty() || !status.written || !status.archiveOk)
        return;
    std::lock_guard<std::mutex> lock(g_storeLock);
    size_t count = save.snapshot.Count();
    if (!pdxstore::WriteStore(pdxstore::SaveFile(g_storeDir, save.key), save.snapshot)) {
        StoreProblem("cannot write the save store for %s (error %lu)", save.key.c_str(), GetLastError());
    } else if (count) {
        Log(LogLevel::Info, "script_storage", "save store written for %s: %zu keys", save.key.c_str(), count);
    }
}

bool __fastcall StoreSaveAddFileHook(void* archive, void* file, const game::String* name, int method, int level) {
    bool added = g_origSaveAddFile(archive, file, name, method, level);
    if (!added && g_saveWriteStatus)
        g_saveWriteStatus->archiveOk = false;
    return added;
}

bool __fastcall StoreCloudSaveWriteHook(void* file, const void* data, uint32_t size) {
    bool written = g_origCloudSaveWrite(file, data, size);
    if (g_saveWriteStatus)
        g_saveWriteStatus->written = written;
    return written;
}

// The two normal local-write paths reach the destructor after WriteData succeeds.
// Only observe PHYSFS_close during those calls. Unwind cleanup calls the original destructor.
void __fastcall StoreDestroySaveArchiveHook(void* archive) {
    SaveWriteStatus* status = g_saveWriteStatus;
    if (status)
        status->closingArchive = true;
    g_origDestroySaveArchive(archive);
    if (status)
        status->closingArchive = false;
}

int __fastcall StoreCloseSaveFileHook(void* file) {
    int closed = g_origCloseSaveFile(file);
    if (g_saveWriteStatus && g_saveWriteStatus->closingArchive)
        g_saveWriteStatus->written = closed != 0;
    return closed;
}

void StoreLoading(const char* path) {
    if (!g_storeOn)
        return;
    if (g_mpStoreOn && MpRoleNow() == MpRole::Client)
        return; // the host supplies the save store
    std::string key = pdxstore::SaveKey(path, false);
    std::lock_guard<std::mutex> lock(g_storeLock);
    if (key.empty()) {
        g_saveStore.Clear();
        RestoreStarbaseTemplateJobs();
        return;
    }
    std::string error;
    if (!pdxstore::ReadStore(pdxstore::SaveFile(g_storeDir, key), g_saveStore, error)) {
        StoreProblem("the save store for %s: %s; it starts empty", key.c_str(), error.c_str());
    } else if (g_saveStore.Count()) {
        Log(LogLevel::Info, "script_storage", "save store read for %s: %zu keys", key.c_str(), g_saveStore.Count());
    }
    RestoreStarbaseTemplateJobs();
}

void StoreNewGame() {
    if (!g_storeOn)
        return;
    std::lock_guard<std::mutex> lock(g_storeLock);
    g_saveStore.Clear();
    g_saveStore.SetClean();
    RestoreStarbaseTemplateJobs();
    g_mpSave.Clear(); // a client's copy: every machine starts the new game with an empty save store
    g_mpSave.SetClean();
}

// At exit, if the main thread isn't in the middle of a change.
void StoreExit() {
    if (!g_storeOn || !g_storeLock.try_lock())
        return;
    WriteGlobalStore();
    g_storeLock.unlock();
}

#ifdef SEF_DEV_STATS
void LogStoreStats() {
    if (!g_storeOn)
        return;
    uint64_t sets = g_storeSets.load(), gets = g_storeGets.load(), tests = g_storeTests.load(),
             rec = g_storeRecorded.load(), ran = g_storeRan.load(), errors = g_storeErrors.load();
    if (!sets && !gets && !tests && !rec && !ran && !errors)
        return;
    Log(LogLevel::Stats, "script_storage", "sets=%llu gets=%llu has=%llu recorded=%llu runs=%llu problems=%llu", sets,
        gets, tests, rec, ran, errors);
}
#endif
