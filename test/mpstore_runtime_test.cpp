// Exercise the real DLL callbacks with fake game objects and an ordered command queue.
// No Stellaris code runs and no sefdata files are read or written.
#include "../src/dllmain.cpp"
#include <new>

namespace {
int failures = 0, chatCalls = 0, sendCalls = 0, initCalls = 0, deletedCommands = 0;
bool acceptPosts = true;
alignas(8) uint8_t session[0xb00]{}, idler[0x200]{}, app[8]{};
uint8_t* currentIdler = idler;
std::vector<std::string> posted;
void Check(bool ok, const char* name) {
    if (!ok) { printf("FAIL: %s\n", name); ++failures; }
}
uint8_t* __fastcall AccessSession(uint8_t*) { return session; }
void __fastcall OriginalChat(void*) { ++chatCalls; }
void __fastcall OriginalSend(void*, void*, bool) { ++sendCalls; }
void __fastcall OriginalInit(void*, void*, int, int) { ++initCalls; }
struct FakeCommand { void* vtable; std::string text; };
void* __fastcall DeleteCommand(void* p, unsigned) {
    static_cast<FakeCommand*>(p)->~FakeCommand();
    free(p);
    ++deletedCommands;
    return nullptr;
}
void* __fastcall NewCommand(size_t n) { return malloc(n); }
void* __fastcall ConstructCommand(void* p, const game::String* text, int, int, int, bool, int) {
    static void* table[] = {reinterpret_cast<void*>(&DeleteCommand)};
    return new(p) FakeCommand{table, std::string(text->data(), text->size)};
}
bool __fastcall PostCommand(void*, void* p) {
    if (!acceptPosts) return false;
    posted.push_back(static_cast<FakeCommand*>(p)->text);
    DeleteCommand(p, 1);
    return true;
}
void Deliver(const std::string& text, uint32_t sender = 1) {
    alignas(8) uint8_t command[0x68]{};
    memcpy(command + g_mpStore.commandSender, &sender, sizeof(sender));
    // BorrowString uses strlen; explicitly restore size for embedded NUL regression cases.
    game::String value = BorrowString(text.c_str());
    value.size = text.size();
    memcpy(command + g_mpStore.chatText, &value, sizeof(value));
    MpChatExecuteHook(command);
}
bool Ready() { MpRole role; return MpStoreReady(&role); }
}

int main() {
    g_mpStoreOn = g_storeOn = true;
    g_globalLoaded = true;
    g_mp.session = &currentIdler;
    g_mp.flag = 0x180;
    idler[g_mp.flag] = 1;
    g_mpStore.idler = &currentIdler;
    g_mpStore.idlerApp = 0x140;
    g_mpStore.appSessionSlot = 0;
    void* vtable[] = {reinterpret_cast<void*>(&AccessSession)};
    *reinterpret_cast<void***>(app) = vtable;
    *reinterpret_cast<uint8_t**>(idler + g_mpStore.idlerApp) = app;
    g_mpStore.hostFlag = 0x9a8;
    g_mpStore.sessionType = 0x58;
    g_mpStore.sessionId = 0x94;
    g_mpStore.commandSender = 8;
    g_mpStore.initialHostId = 1;
    g_mpStore.chatText = 0x20;
    g_mpStore.transferName = 0x80;
    g_mpStore.transferSession = 0x70;
    g_mpStore.operatorNew = reinterpret_cast<uint8_t*>(&NewCommand);
    g_mpStore.chatCtor = reinterpret_cast<uint8_t*>(&ConstructCommand);
    g_mpStore.post = reinterpret_cast<uint8_t*>(&PostCommand);
    g_origChatExecute = &OriginalChat;
    g_origStartSend = &OriginalSend;
    g_origSessionInit = &OriginalInit;
    session[g_mpStore.hostFlag] = 1;
    g_globalStore.Set("host", "42");
    g_saveStore.Set("saved", "5");
    Check(MpPostSnapshot(session, "test", 0) && !Ready(), "posting does not make the host ready");
    const auto snapshot = posted;
    for (const auto& m : snapshot) Deliver(m);
    Check(Ready() && g_globalStore.Dirty() && g_saveStore.Get("saved"), "host echo activates storage and keeps pending writes");
    Deliver("ordinary chat");
    Check(chatCalls == 1, "ordinary chat reaches the original handler");
    Deliver(mpstore::kDisabled, 2);
    Check(Ready(), "client cannot disable or replace storage");

    // The same delivery on a client changes only its in-memory copies.
    session[g_mpStore.hostFlag] = 0;
    g_globalStore.Clear();
    g_globalStore.Set("local", "9");
    g_saveStore.Clear();
    g_saveStore.Set("localSave", "8");
    for (const auto& m : snapshot) Deliver(m);
    Check(Ready() && g_mpGlobal.Get("host") && g_mpSave.Get("saved") && g_globalStore.Get("local") &&
          g_saveStore.Get("localSave"), "client snapshot does not overwrite local stores");
    StoreLoading("save games/not-a-real-save.sav");
    Check(g_mpSave.Get("saved") != nullptr, "client load preserves the received save store");
    Deliver(snapshot[0] + std::string("\0bad", 4));
    Check(!Ready(), "NUL inside a reserved message revokes readiness");
    for (const auto& m : snapshot) Deliver(m);
    pdxstore::Store big, empty;
    big.Set("big", std::string(6000, 'x'));
    const auto chunks = mpstore::MakeMessages(mpstore::MakeSnapshot(big, empty));
    Deliver(chunks[0]);
    Check(!Ready(), "first chunk revokes previous readiness");
    Deliver(chunks[2]);
    Check(!Ready(), "missing chunk never activates storage");
    for (const auto& m : chunks) Deliver(m);
    Check(Ready() && g_mpGlobal.Get("big"), "new complete transfer recovers after malformed traffic");
    g_recording.active = true;
    g_storeRuns.push_back({pdxstore::Which::Global, "old"});
    MpSessionInitHook(session, nullptr, 0, 0);
    Check(initCalls == 1 && !Ready() && g_mpGlobal.Empty() && g_mpSave.Empty() && !g_recording.active && g_storeRuns.empty(),
          "reusing a session address clears copies, readiness, recordings and queued effects");

    session[g_mpStore.hostFlag] = 1;
    for (const auto& m : snapshot) Deliver(m);
    alignas(8) uint8_t transfer[0xd0]{};
    *reinterpret_cast<uint8_t**>(transfer + g_mpStore.transferSession) = session;
    game::String name = BorrowString("OtherData");
    memcpy(transfer + g_mpStore.transferName, &name, sizeof(name));
    posted.clear();
    MpStartSendHook(transfer, nullptr, true);
    Check(sendCalls == 1 && posted.empty(), "unrelated file transfer does not resnapshot storage");
    name = BorrowString("HotJoinData");
    memcpy(transfer + g_mpStore.transferName, &name, sizeof(name));
    MpStartSendHook(transfer, nullptr, true);
    Check(sendCalls == 2 && posted.size() == 1 && posted[0] == mpstore::ResyncMessage(1) && Ready(),
          "save transfer posts a resync request before forwarding, readiness unchanged locally");
    // Script on the host writes before the request executes; the snapshot must keep it.
    g_globalStore.Set("beforeResync", "7");
    const auto request = posted;
    posted.clear();
    Deliver(request[0]);
    Check(!Ready() && !posted.empty() && posted[0].rfind(mpstore::kPrefix, 0) == 0,
          "the host takes and posts its snapshot where the request executes");
    const auto resnapshot = posted;
    for (const auto& m : resnapshot) Deliver(m);
    Check(Ready() && g_globalStore.Get("beforeResync"), "a write made before the request survives the snapshot");
    session[g_mpStore.hostFlag] = 0;
    posted.clear();
    Deliver(request[0]);
    Check(!Ready() && posted.empty(), "a client stops storage at the request and posts nothing");
    for (const auto& m : resnapshot) Deliver(m);
    Check(Ready() && g_mpGlobal.Get("beforeResync"), "a client takes the snapshot posted at the request");
    session[g_mpStore.hostFlag] = 1;
    for (const auto& m : resnapshot) Deliver(m);

    // Two joins in flight: both requests execute before either snapshot. The first snapshot must not
    // reopen storage, or a write made then would be undone by the second, taken before it.
    posted.clear();
    MpStartSendHook(transfer, nullptr, true);
    MpStartSendHook(transfer, nullptr, true);
    const auto requests = posted;  // requests 2 and 3
    Check(requests.size() == 2 && requests[0] == mpstore::ResyncMessage(2) && requests[1] == mpstore::ResyncMessage(3),
          "each request has the next id");
    posted.clear();
    Deliver(requests[0]);
    const auto first = posted;
    posted.clear();
    Deliver(requests[1]);
    const auto second = posted;
    posted.clear();
    for (const auto& m : first) Deliver(m);
    Check(!Ready(), "the earlier request's snapshot does not reopen storage once a later request executed");
    for (const auto& m : second) Deliver(m);
    Check(Ready(), "the latest request's snapshot reopens storage");
    // A joiner that saw only the later request agrees: closed after the first snapshot, open after the second.
    session[g_mpStore.hostFlag] = 0;
    MpSessionInitHook(session, nullptr, 0, 0);
    Deliver(requests[1]);
    for (const auto& m : first) Deliver(m);
    Check(!Ready(), "a joiner that missed the earlier request stays closed for its snapshot too");
    for (const auto& m : second) Deliver(m);
    Check(Ready() && g_mpGlobal.Get("beforeResync"), "and opens with the latest snapshot");
    session[g_mpStore.hostFlag] = 1;
    MpSessionInitHook(session, nullptr, 0, 0);
    for (const auto& m : second) Deliver(m);

    // The session refuses the snapshot where the request executes: the host posts it again later.
    posted.clear();
    MpStartSendHook(transfer, nullptr, true);
    const auto retryRequest = posted;  // request 4
    posted.clear();
    acceptPosts = false;
    Deliver(retryRequest[0]);
    Check(!Ready() && g_mpRetryPending && g_mpRetryAnswers == 4, "a refused snapshot is kept to post again");
    acceptPosts = true;
    MpRetrySnapshot();
    Check(posted.empty() && g_mpRetryPending, "not before its delay");
    // AccessSession may be unavailable for a frame even though the same session resumes.
    currentIdler = nullptr;
    g_mpRetryAt = 0;
    MpRetrySnapshot();
    Check(posted.empty() && g_mpRetryPending && g_mpRetryAnswers == 4,
          "a missing session lookup keeps the pending snapshot");
    currentIdler = idler;
    g_mpRetryAt = 0;
    MpRetrySnapshot();
    Check(!posted.empty() && !g_mpRetryPending, "posted again from the frame once due");
    for (const auto& m : posted) Deliver(m);
    Check(Ready() && g_globalStore.Get("beforeResync"), "the reposted snapshot reopens storage");

    // The session lookup fails while store messages execute (the idler has no application here, so
    // the game still counts as multiplayer). Every machine takes them in order anyway; only posting
    // the host's snapshot and opening storage wait for the session.
    auto loseSession = [] { *reinterpret_cast<uint8_t**>(idler + g_mpStore.idlerApp) = nullptr; };
    auto findSession = [] { *reinterpret_cast<uint8_t**>(idler + g_mpStore.idlerApp) = app; };
    posted.clear();
    MpStartSendHook(transfer, nullptr, true);
    const auto lostRequest = posted;  // request 5
    Check(lostRequest.size() == 1 && lostRequest[0] == mpstore::ResyncMessage(5), "request 5 posted");
    posted.clear();
    loseSession();
    Deliver(lostRequest[0]);
    Check(!Ready() && posted.empty() && g_mpResyncSeen == 5 && g_mpRetryPending && g_mpRetryAnswers == 5,
          "a request executed without the session closes storage and keeps the snapshot to post");
    g_mpRetryAt = 0;
    MpRetrySnapshot();
    Check(posted.empty() && g_mpRetryPending, "the retry waits for the session too");
    findSession();
    g_mpRetryAt = 0;
    MpRetrySnapshot();
    Check(!posted.empty() && !g_mpRetryPending, "the host posts the snapshot once the session is found");
    const auto lostSnapshot = posted;
    posted.clear();
    loseSession();
    for (const auto& m : lostSnapshot) Deliver(m);
    Check(!Ready() && g_mpUnapplied.pending.load(), "a snapshot completed without the session is kept");
    findSession();
    Check(Ready() && !g_mpUnapplied.pending.load() && g_globalStore.Get("beforeResync"),
          "the host opens storage with it once the session is found");
    session[g_mpStore.hostFlag] = 0;
    loseSession();
    Deliver(lostRequest[0]);
    Check(!Ready() && g_mpRetryPending, "a client without the session cannot tell it is not the host");
    findSession();
    g_mpRetryAt = 0;
    MpRetrySnapshot();
    Check(posted.empty() && !g_mpRetryPending, "and drops the retry once the session shows it");
    g_mpGlobal.Clear();
    loseSession();
    for (const auto& m : lostSnapshot) Deliver(m);
    findSession();
    Check(Ready() && g_mpGlobal.Get("beforeResync"), "a client opens storage with the snapshot it kept");
    loseSession();
    for (const auto& m : lostSnapshot) Deliver(m);
    Deliver(chunks[0]);  // another transfer's first piece revokes readiness on every machine
    findSession();
    Check(!Ready() && !g_mpUnapplied.pending.load(), "a later store message drops the kept snapshot");
    session[g_mpStore.hostFlag] = 1;
    for (const auto& m : lostSnapshot) Deliver(m);
    Check(Ready(), "the host is ready again");

    posted.clear();
    acceptPosts = false;
    int before = deletedCommands, sendsBefore = sendCalls;
    MpStartSendHook(transfer, nullptr, true);
    Check(sendCalls == sendsBefore && deletedCommands == before + 1 && Ready(),
          "refused command is freed, transfer blocked, readiness not changed locally");
    acceptPosts = true;
    g_globalStore.Set("huge", std::string(mpstore::kMaxSnapshot, 'x'));
    posted.clear();
    Check(MpPostSnapshot(session, "oversize", g_mpResyncPosted) && posted.size() == 1 && posted[0] == mpstore::kDisabled && Ready(),
          "oversize snapshot posts an ordered disable without locally changing readiness");
    Deliver(posted[0]);
    Check(!Ready(), "host and clients disable at the command boundary");
    g_globalStore.Clear();
    g_saveStore.Set("previousGame", "1");
    posted.clear();
    MpPostSnapshot(session, "new game", g_mpResyncPosted, true);
    for (const auto& m : posted) Deliver(m);
    Check(Ready() && g_saveStore.Empty(), "new game snapshot does not carry the previous save store");
    printf(failures ? "mpstore_runtime_test: %d FAILED\n" : "mpstore_runtime_test: all passed\n", failures);
    return failures ? 1 : 0;
}
