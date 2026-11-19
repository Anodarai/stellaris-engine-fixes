// Part of scriptstorage_runtime.inl: the "@sef:" commands. Which store a command uses, game strings
// and variables, the effect and trigger commands (StoreEffect, StoreTrigger), the log text hook that
// records lines, and running recorded script.

// The caller holds g_storeLock.
pdxstore::Store& StoreFor(pdxstore::Which which) {
    if (g_mpStoreOn && MpRoleNow() == MpRole::Client)
        return which == pdxstore::Which::Save ? g_mpSave : g_mpGlobal;
    if (which == pdxstore::Which::Save)
        return g_saveStore;
    if (!g_globalLoaded) {
        g_globalLoaded = true;
        std::string error;
        if (!pdxstore::ReadStore(g_storeDir + L"\\global.txt", g_globalStore, error)) {
            Log(LogLevel::Warn, "script_storage",
                "sefdata\\global.txt: %s; the global store starts empty (the file is kept until it is "
                "next written)",
                error.c_str());
        } else {
            Log(LogLevel::Info, "script_storage", "global store: %zu keys", g_globalStore.Count());
        }
    }
    return g_globalStore;
}

// The caller holds g_storeLock.
void WriteGlobalStore() {
    if (!g_globalLoaded || !g_globalStore.Dirty())
        return;
    g_globalWritten = GetTickCount64();
    if (!pdxstore::WriteStore(g_storeDir + L"\\global.txt", g_globalStore)) {
        StoreProblem("cannot write sefdata\\global.txt (error %lu)", GetLastError());
    }
}

// A CString the game owns, holding `text` (zeroed first, as the game's out-parameters are). No C++
// objects here, for __try.
bool MakeGameString(const char* text, size_t size, game::String* s) {
    memset(s, 0, sizeof(*s));
    s->cap = 0xF;
    __try {
        reinterpret_cast<game::AssignFn>(g_storeAssign)(s, text, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// FreeGameString for a string the game filled in a call that may have faulted part-way.
void FreeGameStringGuarded(game::String* s) {
    __try {
        FreeGameString(*s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    memset(s, 0, sizeof(*s));
}

// The scope's variable, as check_variable reads it: 1 set, 0 unset, -1 on a fault.
int ReadVariable(uint8_t* scope, const game::String* name, int64_t* value) {
    __try {
        uint8_t* variables = reinterpret_cast<AccessVariablesFn>(g_vars.get)(scope, name);
        if (!variables)
            return 0;
        uint8_t* entry = nullptr;
        reinterpret_cast<FindVariableFn>(g_vars.find)(variables + g_vars.map, &entry, name);
        if (!entry || entry[g_vars.entryEmpty] == 0xFF)
            return 0;
        *value = *reinterpret_cast<const int64_t*>(entry + g_vars.entryValue);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Sets the scope's variable, as set_variable does: 1 done, 0 when the scope has no variables, -1 on a fault.
int WriteVariable(uint8_t* scope, const game::String* name, int64_t value) {
    __try {
        uint8_t* variables = reinterpret_cast<AccessVariablesFn>(g_vars.access)(scope, name);
        if (!variables)
            return 0;
        reinterpret_cast<SetVariableFn>(g_vars.set)(variables, name, value);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// "@sef:" from a log effect: everything but `has`.
void StoreEffect(const char* text, uint8_t* scope) {
    pdxstore::Command c;
    std::string error;
    if (!pdxstore::ParseCommand(text, c, error)) {
        StoreProblem("log = \"@sef:%s\": %s", text, error.c_str());
        return;
    }
    MpRole role = MpRole::Single;
    if (!MpStoreReady(&role)) {
        StoreProblem("log = \"@sef:%s\": off in this multiplayer game (%s)", text,
                     g_mpStoreOn ? "the host sent no stores" : "[script_storage] multiplayer=0");
        return;
    }
    if (role != MpRole::Single && c.op == pdxstore::Op::Run) {
        StoreProblem("log = \"@sef:%s\": run is off in multiplayer", text);
        return;
    }
    if (c.op == pdxstore::Op::Has) {
        StoreProblem("log = \"@sef:%s\": has is a trigger; use it in a limit or trigger block", text);
        return;
    }
    std::unique_lock<std::mutex> lock(g_storeLock);
    switch (c.op) {
    case pdxstore::Op::Set: {
        int64_t value = c.value;
        int got = 1;
        if (!c.number) {
            lock.unlock(); // game code: no lock held while it runs
            game::String name;
            if (!MakeGameString(c.variable.data(), c.variable.size(), &name)) {
                StoreProblem("@sef:%s: the game faulted making the name", text);
                return;
            }
            got = ReadVariable(scope, &name, &value);
            FreeGameStringGuarded(&name);
            lock.lock();
            if (got < 0) {
                StoreProblem("@sef:%s: the game faulted reading the variable", text);
                return;
            }
        }
        pdxstore::Store& store = StoreFor(c.store);
        if (got == 0) {
            store.Remove(c.key); // an unset variable leaves the key unset
        } else if (!store.Set(c.key, pdxstore::FormatFixed(value))) {
            StoreProblem("@sef:%s: the %s store is full", text, StoreName(c.store));
        }
        g_storeSets.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    case pdxstore::Op::Get: {
        const std::string* stored = StoreFor(c.store).Get(c.key);
        if (!stored)
            return; // nothing stored: the variable stays as it is
        int64_t value = 0;
        if (!pdxstore::ParseFixed(*stored, &value)) {
            StoreProblem("@sef:%s: the %s value of %s is text, not a number", text, StoreName(c.store), c.key.c_str());
            return;
        }
        lock.unlock();
        game::String name;
        if (!MakeGameString(c.variable.data(), c.variable.size(), &name)) {
            StoreProblem("@sef:%s: the game faulted making the name", text);
            return;
        }
        int done = WriteVariable(scope, &name, value);
        FreeGameStringGuarded(&name);
        if (done < 0)
            StoreProblem("@sef:%s: the game faulted setting the variable", text);
        if (done == 0)
            StoreProblem("@sef:%s: this scope has no variables", text);
        g_storeGets.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    case pdxstore::Op::Clear:
        StoreFor(c.store).Remove(c.key);
        return;
    case pdxstore::Op::Record:
        if (g_recording.active) {
            StoreProblem("@sef:%s: already recording %s; record_end first", text, g_recording.key.c_str());
            return;
        }
        g_recording = StoreRecording();
        g_recording.active = true;
        g_recording.thread = GetCurrentThreadId();
        g_recording.store = c.store;
        g_recording.key = c.key;
        return;
    case pdxstore::Op::RecordEnd: {
        if (!g_recording.active) {
            StoreProblem("@sef:record_end with no recording");
            return;
        }
        if (g_recording.thread != GetCurrentThreadId()) {
            StoreProblem("@sef:record_end from a different thread than record");
            return;
        }
        StoreRecording r = std::move(g_recording);
        g_recording = StoreRecording();
        if (!StoreFor(r.store).Set(r.key, r.text)) {
            StoreProblem("record %s.%s: too long for the store", StoreName(r.store), r.key.c_str());
            return;
        }
        g_storeRecorded.fetch_add(1, std::memory_order_relaxed);
        Log(LogLevel::Info, "script_storage", "recorded %s.%s: %d lines kept, %d without script dropped",
            StoreName(r.store), r.key.c_str(), r.lines, r.dropped);
        return;
    }
    case pdxstore::Op::Run:
        if (!g_storeEffectCommand) {
            StoreProblem("@sef:%s: run is not available (the console's effect command was not found)", text);
            return;
        }
        g_storeRuns.push_back({c.store, c.key});
        return;
    default:
        return;
    }
}

// "@sef:" from a log trigger: only `has`.
bool StoreTrigger(const char* text, uint8_t* /*scope*/) {
    pdxstore::Command c;
    std::string error;
    if (!pdxstore::ParseCommand(text, c, error)) {
        StoreProblem("log = \"@sef:%s\": %s", text, error.c_str());
        return false;
    }
    if (c.op != pdxstore::Op::Has) {
        StoreProblem("log = \"@sef:%s\" in a trigger: only has works there", text);
        return false;
    }
    MpRole role = MpRole::Single;
    if (!MpStoreReady(&role))
        return false; // the same answer on every machine
    std::lock_guard<std::mutex> lock(g_storeLock);
    g_storeTests.fetch_add(1, std::memory_order_relaxed);
    return StoreFor(c.store).Get(c.key) != nullptr;
}

// Copies the text ProcessString built. No C++ objects here, for __try.
int32_t CopyLogText(const game::String* text, char* out, int32_t size) {
    __try {
        return CopyGameString(text, out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Replaces the log effect's call to CTextBase::ProcessString: the game's text, with \[...] filled in,
// added to the recording while one is open on this thread.
game::String* __fastcall LogTextHook(void* text, game::String* out, void* in, void* a, void* b, void* c) {
    game::String* result = g_origProcessString(text, out, in, a, b, c);
    std::lock_guard<std::mutex> lock(g_storeLock);
    if (!g_recording.active || g_recording.thread != GetCurrentThreadId())
        return result;
    char buf[8192];
    int32_t length = CopyLogText(out, buf, sizeof(buf));
    if (length < 0)
        return result;
    if (length >= static_cast<int32_t>(sizeof(buf))) {
        StoreProblem("record %s: a line of %d bytes was cut to %zu", g_recording.key.c_str(), length, sizeof(buf) - 1);
    }
    std::string kept;
    if (g_recording.text.size() > pdxstore::kMaxValue) {
        ++g_recording.dropped; // already too long to store: record_end reports it
    } else if (pdxstore::KeepRecordedLine(buf, &kept)) {
        g_recording.text += kept;
        g_recording.text += '\n';
        ++g_recording.lines;
    } else {
        ++g_recording.dropped;
    }
    return result;
}

// Runs `text` with the console's effect command. No C++ objects here, for __try.
bool RunStoredEffect(const char* text, game::ConsoleResult* result) {
    game::String line = game::Borrow(text);
    uint8_t args[0x18]{};
    game::String* items = &line;
    memcpy(args + game::kArrayDataOffset, &items, sizeof(items));
    *reinterpret_cast<int*>(args + game::kArrayCountOffset) = 1;
    __try {
        reinterpret_cast<game::EffectCommandFn>(g_storeEffectCommand)(result, args);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
