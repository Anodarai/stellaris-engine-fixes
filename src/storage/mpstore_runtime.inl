// Part of scriptstorage_runtime.inl: script storage in multiplayer (mpstore.h, re/multiplayer.md). The
// host's snapshots, the session and role checks, the start, send and session hooks, and the chat
// command hook through which every machine takes the host's stores.

// ---- Script storage in multiplayer (mpstore.h) ----
// With [script_storage] multiplayer=1 the host sends its stores to every player as chat commands
// the chat never shows. The host's stores are its real ones; a client has copies, which no file
// ever sees. Until this machine has the snapshot of the session it is in, storage is refused.

mpstore::Locations g_mpStore;
bool g_mpStoreOn = false;
pdxstore::Store g_mpGlobal, g_mpSave;                   // a client's copies; under g_storeLock
std::atomic<const uint8_t*> g_mpSyncedSession{nullptr}; // the session whose snapshot this machine executed
mpstore::Assembler g_mpAssembler;                       // under g_storeLock
uint64_t g_mpResyncPosted = 0; // the host: id of the latest resync request posted (main thread)
uint64_t g_mpResyncSeen = 0;   // the latest resync request executed here; under g_storeLock
// The host: a snapshot that the session refused, posted again from the frame (StoreFrame) while
// storage stays closed everywhere, so the stores it holds cannot change. Also set on any machine
// where a request executed without the session; the retry drops it if this is not the host.
// Under g_storeLock.
bool g_mpRetryPending = false;
uint64_t g_mpRetryAnswers = 0;
ULONGLONG g_mpRetryAt = 0, g_mpRetryDelay = 1000;
// A complete snapshot whose last piece executed while the session could not be found, so neither
// whether this machine is the host nor the session to open storage for was known. Storage stays
// refused here until the session is found again (MpStoreReady), and is opened then with it. The
// next store message drops it, as it revokes readiness on every machine. Under g_storeLock.
struct MpUnapplied {
    std::atomic<bool> pending{false};
    pdxstore::Store global, save;
    size_t bytes = 0;
} g_mpUnapplied;
void(__fastcall* g_origChatExecute)(void*) = nullptr;
void(__fastcall* g_origStartSend)(void*, void*, bool) = nullptr;
void(__fastcall* g_origStartSync)(void*) = nullptr;
void(__fastcall* g_origSessionInit)(void*, void*, int, int) = nullptr;

enum class MpRole { Single, Host, Client };

// The session commands go to, as the game finds it (the idler's application's AccessSession);
// null without one. No C++ objects here, for __try.
uint8_t* MpSession() {
    if (!g_mpStoreOn)
        return nullptr;
    __try {
        uint8_t* idler = *g_mpStore.idler;
        if (!idler)
            return nullptr;
        uint8_t* app = *reinterpret_cast<uint8_t**>(idler + g_mpStore.idlerApp);
        if (!app)
            return nullptr;
        using AccessFn = uint8_t*(__fastcall*)(uint8_t*);
        return (*reinterpret_cast<AccessFn*>(*reinterpret_cast<uint8_t**>(app) + g_mpStore.appSessionSlot))(app);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool MpReadSession(const uint8_t* session, bool* host, int* type) {
    *host = false;
    *type = -1;
    if (!session)
        return false;
    __try {
        *host = session[g_mpStore.hostFlag] != 0;
        *type = *reinterpret_cast<const int*>(session + g_mpStore.sessionType);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What this machine is in the running game. Without the session it counts as a client, which has
// no data.
MpRole MpRoleNow() {
    if (!StoreMultiplayer())
        return MpRole::Single;
    bool host = false;
    int type = 0;
    MpReadSession(MpSession(), &host, &type);
    return host ? MpRole::Host : MpRole::Client;
}

// Puts the host's stores in place and opens storage for `session`. The caller holds g_storeLock.
void MpApplySnapshot(const uint8_t* session, bool host, pdxstore::Store& global, pdxstore::Store& save, size_t bytes) {
    size_t globalKeys = global.Count(), saveKeys = save.Count();
    if (host) {
        // Apply at exactly the same command boundary as clients. Preserve a pending disk write.
        // Nothing was written since the snapshot was taken (at the resync request) or no game was running.
        bool dirty = g_globalStore.Dirty() || g_globalStore.Values() != global.Values();
        g_globalStore = std::move(global);
        if (dirty)
            g_globalStore.MarkDirty();
        g_saveStore = std::move(save);
    } else {
        g_mpGlobal = std::move(global);
        g_mpSave = std::move(save);
    }
    g_mpSyncedSession.store(session);
    Log(LogLevel::Info, "script_storage", "took the host's stores: %zu global and %zu save keys, %zu bytes", globalKeys,
        saveKeys, bytes);
}

// The snapshot that completed while the session could not be found, once it can be.
void MpApplyUnapplied() {
    if (!g_mpUnapplied.pending.load())
        return;
    uint8_t* session = MpSession();
    bool host = false;
    int type = 0;
    if (!MpReadSession(session, &host, &type))
        return;
    std::lock_guard<std::mutex> lock(g_storeLock);
    if (!g_mpUnapplied.pending.load())
        return;
    g_mpUnapplied.pending.store(false);
    MpApplySnapshot(session, host, g_mpUnapplied.global, g_mpUnapplied.save, g_mpUnapplied.bytes);
}

// Whether script may use the stores now: always in single player, and in multiplayer once this
// machine has executed the snapshot of the session it is in, including the host.
bool MpStoreReady(MpRole* role) {
    *role = MpRoleNow();
    if (*role == MpRole::Single)
        return true;
    MpApplyUnapplied();
    const uint8_t* synced = g_mpSyncedSession.load();
    return synced && MpSession() == synced;
}

// Posts one chat command with `text` to every player. No C++ objects here, for __try.
bool MpPostText(uint8_t* session, const char* text) {
    game::String s = game::Borrow(text);
    __try {
        auto* command = reinterpret_cast<void*(__fastcall*)(size_t)>(g_mpStore.operatorNew)(mpstore::kChatCommandSize);
        if (!command)
            return false;
        using CtorFn = void*(__fastcall*)(void*, const game::String*, int, int, int, bool, int);
        reinterpret_cast<CtorFn>(g_mpStore.chatCtor)(command, &s, 0, 0, 0, true, -1);
        bool posted = reinterpret_cast<bool(__fastcall*)(void*, void*)>(g_mpStore.post)(session, command);
        if (!posted) { // still ours: the scalar deleting destructor, the vtable's first slot
            using DeleteFn = void*(__fastcall*)(void*, unsigned);
            (*reinterpret_cast<DeleteFn**>(command))[0](command, 1);
        }
        return posted;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

pdxstore::Store& StoreFor(pdxstore::Which which);

// The host sends both stores to every player, answering resync request `answers`. Main thread.
bool MpPostSnapshot(uint8_t* session, const char* when, uint64_t answers, bool newGame = false) {
    std::string snapshot;
    size_t globalKeys = 0, saveKeys = 0;
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        StoreFor(pdxstore::Which::Global); // reads the file the first time
        snapshot = mpstore::MakeSnapshot(g_globalStore, newGame ? pdxstore::Store{} : g_saveStore, answers);
        globalKeys = g_globalStore.Count();
        saveKeys = newGame ? 0 : g_saveStore.Count();
    }
    if (snapshot.size() > mpstore::kMaxSnapshot) {
        Log(LogLevel::Warn, "script_storage",
            "the stores are %zu bytes, more than the %zu sent in multiplayer; posting storage disable to all players",
            snapshot.size(), mpstore::kMaxSnapshot);
        return MpPostText(session, mpstore::kDisabled);
    }
    std::vector<std::string> messages = mpstore::MakeMessages(snapshot);
    for (const std::string& m : messages) {
        if (!MpPostText(session, m.c_str())) {
            Log(LogLevel::Error, "script_storage", "the session refused a store command (%s)", when);
            return false;
        }
    }
    Log(LogLevel::Info, "script_storage",
        "sent the stores to the players (%s): %zu global and %zu save keys, %zu bytes in %zu chat commands", when,
        globalKeys, saveKeys, snapshot.size(), messages.size());
    return true;
}

// Only the host gets here (CGameSetup::Start), when the lobby starts the game.
void __fastcall MpStartSyncHook(void* setup) {
    uint8_t* session = MpSession();
    bool host = false;
    int type = 0;
    MpReadSession(session, &host, &type);
    if (session && host && type == mpstore::kLobbyHostSession) {
        // Answers no request: the latest one posted, so that no machine finds it older than one it saw.
        if (!MpPostSnapshot(session, "the lobby starts the game", g_mpResyncPosted, true)) {
            Log(LogLevel::Error, "script_storage", "game start blocked; retry after fixing the session");
            return;
        }
    } else {
        Log(LogLevel::Warn, "script_storage", "a game starts and the stores are not sent (session type %d, host %d)",
            type, host ? 1 : 0);
    }
    g_origStartSync(setup);
}

// The host starts to send its game state to a player: a hot join, or a loaded save.
bool MpIsSaveTransfer(const void* handler, const uint8_t* session) {
    __try {
        auto* p = static_cast<const uint8_t*>(handler);
        auto* name = reinterpret_cast<const game::String*>(p + g_mpStore.transferName);
        return *reinterpret_cast<uint8_t* const*>(p + g_mpStore.transferSession) == session &&
               name->size == sizeof("HotJoinData") - 1 &&
               memcmp(name->data(), "HotJoinData", sizeof("HotJoinData") - 1) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game is running: only ask for a snapshot here (mpstore::kResync). MpTakeMessage takes it
// where the request executes, so script that runs before then keeps its writes.
void __fastcall MpStartSendHook(void* handler, void* blob, bool reset) {
    uint8_t* session = MpSession();
    bool host = false;
    int type = 0;
    MpReadSession(session, &host, &type);
    if (session && host && StoreMultiplayer() && MpIsSaveTransfer(handler, session) &&
        !MpPostText(session, mpstore::ResyncMessage(++g_mpResyncPosted).c_str())) {
        Log(LogLevel::Error, "script_storage",
            "the session refused a store command (a player receives the game); "
            "transfer blocked, retry after fixing the session");
        return;
    }
    g_origStartSend(handler, blob, reset);
}

void __fastcall MpSessionInitHook(void* session, void* server, int basicType, int type) {
    if (!g_mpStoreOn)
        return g_origSessionInit(session, server, basicType, type);
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        g_mpSyncedSession.store(nullptr);
        g_mpAssembler.Reset();
        g_mpGlobal = pdxstore::Store{};
        g_mpSave = pdxstore::Store{};
        g_recording = StoreRecording{};
        g_storeRuns.clear();
        g_mpResyncSeen = 0; // g_mpResyncPosted keeps rising: a new session's ids stay above 0
        g_mpRetryPending = false;
        g_mpUnapplied.pending.store(false);
    }
    g_origSessionInit(session, server, basicType, type);
}

// A chat text that begins with mpstore::kPrefix: a piece of the host's stores. Every machine takes
// it even if it cannot find the session now, so that readiness, the latest request and the pieces
// stay the same everywhere; only the host's snapshot post and opening storage need the session.
void MpTakeMessage(const std::string& text) {
    uint8_t* session = MpSession();
    bool host = false;
    int type = 0;
    bool known = MpReadSession(session, &host, &type);
    std::string snapshot, error;
    mpstore::Assembler::Result result = mpstore::Assembler::Result::NotOurs;
    uint64_t request = 0;
    bool resync = mpstore::ParseResync(text, &request);
    {
        std::lock_guard<std::mutex> lock(g_storeLock);
        // The ordered command, not the local send, revokes readiness on every peer.
        g_mpSyncedSession.store(nullptr);
        g_mpUnapplied.pending.store(false);
        if (text == mpstore::kDisabled) {
            g_mpAssembler.Reset();
            g_mpRetryPending = false;
            Log(LogLevel::Warn, "script_storage",
                "host disabled storage for this multiplayer game (snapshot too large)");
            return;
        }
        if (resync) {
            g_mpAssembler.Reset();
            if (request > g_mpResyncSeen)
                g_mpResyncSeen = request;
            // An earlier pending snapshot is superseded: this request gets its own. Without the
            // session, whether this machine is the host is not known; MpRetrySnapshot posts the
            // snapshot once the session is found if it is, and drops it if not.
            g_mpRetryPending = !known;
            if (!known) {
                g_mpRetryAnswers = request;
                g_mpRetryDelay = 1000;
                g_mpRetryAt = GetTickCount64() + g_mpRetryDelay;
            }
        } else {
            result = g_mpAssembler.Feed(text, &snapshot, &error);
        }
    }
    if (resync) {
        // Every machine is at this boundary with the same stores and refuses storage from here until
        // the snapshot's last piece executes, so the snapshot taken now loses no write. Posting while
        // a command executes is what the AI does during the tick command.
        if (!known) {
            StoreProblem("multiplayer: a resync request executed while the session could not be found; "
                         "if this is the host, its stores are posted from the next frames");
        } else if (host && !MpPostSnapshot(session, "a player receives the game", request)) {
            {
                std::lock_guard<std::mutex> lock(g_storeLock);
                g_mpRetryPending = true;
                g_mpRetryAnswers = request;
                g_mpRetryDelay = 1000;
                g_mpRetryAt = GetTickCount64() + g_mpRetryDelay;
            }
            StoreProblem("multiplayer: the session refused the host's stores; storage is off until they are sent, "
                         "which is retried from the next frames");
        }
        return;
    }
    if (result == mpstore::Assembler::Result::Bad) {
        StoreProblem("multiplayer: %s; the host's stores were not taken", error.c_str());
        return;
    }
    if (result != mpstore::Assembler::Result::Complete)
        return;
    pdxstore::Store global, save;
    uint64_t answers = 0;
    if (!mpstore::ReadSnapshot(snapshot, global, save, error, &answers)) {
        StoreProblem("multiplayer: the host's stores: %s; storage is off in this game", error.c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(g_storeLock);
    if (answers < g_mpResyncSeen) {
        // A later request executed after this one: its snapshot is still to come, and storage
        // stays closed until then, on every machine alike.
        Log(LogLevel::Info, "script_storage",
            "a snapshot for request %llu arrived after request %llu; waiting for its snapshot",
            static_cast<unsigned long long>(answers), static_cast<unsigned long long>(g_mpResyncSeen));
        return;
    }
    if (!known) {
        g_mpUnapplied.global = std::move(global);
        g_mpUnapplied.save = std::move(save);
        g_mpUnapplied.bytes = snapshot.size();
        g_mpUnapplied.pending.store(true);
        Log(LogLevel::Warn, "script_storage",
            "the host's stores arrived while the session could not be found; "
            "they are taken when it is found again");
        return;
    }
    MpApplySnapshot(session, host, global, save, snapshot.size());
}

// The command's text, or -1. No C++ objects here, for __try.
int32_t MpCopyChatText(const void* command, char* out, int32_t size) {
    __try {
        auto* text = reinterpret_cast<const game::String*>(static_cast<const uint8_t*>(command) + g_mpStore.chatText);
        return CopyGameString(text, out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool MpIsHostCommand(const void* command) {
    __try {
        return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(command) + g_mpStore.commandSender) ==
               g_mpStore.initialHostId;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// CChatCommand::Execute, on every machine, in the order the session gives commands.
void __fastcall MpChatExecuteHook(void* command) {
    char text[mpstore::kMaxMessage + 2];
    int32_t length = MpCopyChatText(command, text, sizeof(text));
    if (length < 0 || !mpstore::IsMessage(text, strlen(text)))
        return g_origChatExecute(command);
    if (!MpIsHostCommand(command))
        return; // an ordinary client's chat must never replace the host's stores
    if (length > mpstore::kMaxMessage) {
        MpTakeMessage(std::string(mpstore::kPrefix) + "invalid-length");
        return;
    }
    MpTakeMessage(std::string(text, static_cast<size_t>(length)));
}
