// Tests mpstore.h: the snapshot of the two stores, the chat messages it travels in and their
// assembly, and, given a stellaris.exe, the game functions it locates (mapped as an image; no code
// runs).
// Usage: mpstore_test.exe [<path to stellaris.exe> [--expect-known]]
//   --expect-known  also compare with the addresses of the Steam 4.5.2 exe (SHA-256 400DF27C...7A83)
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../src/mpstore.h"

static int g_failures = 0;

static void Check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

using mpstore::Assembler;

// Sends `snapshot` through messages and an assembler; true when it arrives unchanged.
static bool RoundTrip(const std::string& snapshot, size_t* messages) {
    std::vector<std::string> m = mpstore::MakeMessages(snapshot);
    *messages = m.size();
    Assembler a;
    std::string out, error;
    for (size_t i = 0; i < m.size(); ++i) {
        if (m[i].size() > mpstore::kMaxMessage || strlen(m[i].c_str()) != m[i].size()) return false;
        Assembler::Result r = a.Feed(m[i], &out, &error);
        if (r != (i + 1 == m.size() ? Assembler::Result::Complete : Assembler::Result::Partial)) return false;
    }
    return out == snapshot;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Base64.
    std::string decoded;
    Check(mpstore::Base64Encode("") == "" && mpstore::Base64Encode("f") == "Zg==" && mpstore::Base64Encode("fo") == "Zm8=" &&
              mpstore::Base64Encode("foo") == "Zm9v" && mpstore::Base64Encode("foobar") == "Zm9vYmFy",
          "base64 encodes the standard examples");
    Check(mpstore::Base64Decode("Zm9vYmE=", &decoded) && decoded == "fooba", "base64 decodes padding");
    Check(!mpstore::Base64Decode("Zm9", &decoded) && !mpstore::Base64Decode("Zm9!", &decoded) &&
              !mpstore::Base64Decode("Zg==Zm9v", &decoded) && !mpstore::Base64Decode("Z=g=", &decoded),
          "base64 refuses damaged text");
    std::string bytes;
    for (int i = 0; i < 256; ++i) bytes += static_cast<char>(i);
    Check(mpstore::Base64Decode(mpstore::Base64Encode(bytes), &decoded) && decoded == bytes, "base64 keeps every byte");
    Check(!mpstore::Base64Decode("Zh==", &decoded) && !mpstore::Base64Decode("Zm9=", &decoded),
          "base64 refuses nonzero padding bits");

    // The snapshot.
    pdxstore::Store global, save, g2, s2;
    std::string error;
    Check(mpstore::ReadSnapshot(mpstore::MakeSnapshot(global, save), g2, s2, error) && g2.Empty() && s2.Empty(),
          "two empty stores survive");
    global.Set("giga.preset", "3");
    global.Set("script", "set_global_flag = a\ncountry_event = { id = \"x.1\" }\n");
    global.Set("odd", std::string("back\\slash \"quote\" \r\n caf\xC3\xA9"));
    save.Set("turns", "-0.5");
    Check(mpstore::ReadSnapshot(mpstore::MakeSnapshot(global, save), g2, s2, error) && g2.Values() == global.Values() &&
              s2.Values() == save.Values() && !g2.Dirty() && !s2.Dirty(),
          "both stores survive, and arrive clean");
    Check(mpstore::ReadSnapshot(mpstore::MakeSnapshot(save, global), g2, s2, error) && g2.Values() == save.Values() &&
              s2.Values() == global.Values(),
          "the stores are not mixed up");
    std::string snapshot = mpstore::MakeSnapshot(global, save);
    Check(!mpstore::ReadSnapshot("", g2, s2, error) && !mpstore::ReadSnapshot("hello\n", g2, s2, error) &&
              !mpstore::ReadSnapshot(snapshot.substr(0, snapshot.size() - 1), g2, s2, error) &&
              !mpstore::ReadSnapshot(snapshot + "x", g2, s2, error) && g2.Empty() && s2.Empty(),
          "a text that is not a snapshot is refused and leaves the stores empty");
    Check(!mpstore::ReadSnapshot("sef-snapshot-2 0 5 0\nx = 1\n", g2, s2, error) && g2.Empty(),
          "a damaged store is refused");
    uint64_t answers = 0;
    Check(mpstore::ReadSnapshot(mpstore::MakeSnapshot(global, save, 18446744073709551615ull), g2, s2, error, &answers) &&
              answers == 18446744073709551615ull && g2.Values() == global.Values(),
          "the snapshot names the resync request it answers");
    Check(!mpstore::ReadSnapshot("sef-snapshot-1 0 0\n", g2, s2, error) && !mpstore::ReadSnapshot("sef-snapshot-2 0 0\n", g2, s2, error) &&
              !mpstore::ReadSnapshot("sef-snapshot-2 x 0 0\n", g2, s2, error),
          "a snapshot of the previous version or without its request id is refused");
    uint64_t id = 0;
    Check(mpstore::ParseResync(mpstore::ResyncMessage(42), &id) && id == 42 &&
              !mpstore::ParseResync(std::string(mpstore::kResyncPrefix), &id) &&
              !mpstore::ParseResync(std::string(mpstore::kResyncPrefix) + "4x", &id) &&
              !mpstore::ParseResync(std::string(mpstore::kResyncPrefix) + "-1", &id) &&
              !mpstore::ParseResync("@sef-store-1:resync", &id),
          "resync requests carry a plain decimal id");

    // Messages.
    size_t count = 0;
    Check(RoundTrip(snapshot, &count) && count == 1, "a small snapshot is one message");
    Check(RoundTrip("", &count) && count == 1, "an empty text is one message");
    std::string big;
    for (int i = 0; big.size() < 3 * mpstore::kChunkBytes + 17; ++i) big += static_cast<char>(i * 7 + 1);
    Check(RoundTrip(big, &count) && count == 4, "a large text is cut and put together again");
    Check(RoundTrip(std::string(mpstore::kChunkBytes, 'a'), &count) && count == 1, "a text of exactly one piece");
    Check(RoundTrip(std::string(mpstore::kMaxSnapshot, 'z'), &count), "the largest snapshot survives");
    Check(mpstore::MakeMessages(std::string(mpstore::kMaxSnapshot + 1, 'z')).empty(), "oversize snapshot is not sent");
    Check(!mpstore::ReadSnapshot("sef-snapshot-2 0 +0 0\n", g2, s2, error) &&
              !mpstore::ReadSnapshot("sef-snapshot-2 0 18446744073709551616 0\n", g2, s2, error) &&
              !mpstore::ReadSnapshot("sef-snapshot-2 18446744073709551616 0 0\n", g2, s2, error),
          "snapshot sizes and ids reject signs and overflow");

    std::vector<std::string> m = mpstore::MakeMessages(big);
    Assembler a;
    std::string out;
    Check(a.Feed("hello", &out, &error) == Assembler::Result::NotOurs && a.Feed("", &out, &error) == Assembler::Result::NotOurs &&
              a.Feed("@sef", &out, &error) == Assembler::Result::NotOurs,
          "the chat's own text is left alone");
    Check(a.Feed(std::string(mpstore::kPrefix) + "typed by a player", &out, &error) == Assembler::Result::Bad,
          "text with the prefix and nothing else is taken and dropped");
    Check(a.Feed(m[1], &out, &error) == Assembler::Result::Bad, "a message without its beginning is dropped");
    Check(a.Feed(m[0], &out, &error) == Assembler::Result::Partial && a.Feed(m[2], &out, &error) == Assembler::Result::Bad,
          "a skipped message drops the snapshot");
    Check(a.Feed(m[3], &out, &error) == Assembler::Result::Bad, "and what follows it");
    // A second snapshot interrupts the first: the second arrives whole.
    std::vector<std::string> m2 = mpstore::MakeMessages(snapshot);
    Check(a.Feed(m[0], &out, &error) == Assembler::Result::Partial && a.Feed(m2[0], &out, &error) == Assembler::Result::Complete &&
              out == snapshot,
          "a new snapshot replaces an unfinished one");
    std::string damaged = m2[0];
    damaged[damaged.size() - 5] = damaged[damaged.size() - 5] == 'A' ? 'B' : 'A';
    Check(a.Feed(damaged, &out, &error) == Assembler::Result::Bad, "changed data fails the hash");
    Check(a.Feed(m2[0] + std::string(mpstore::kMaxMessage, 'A'), &out, &error) == Assembler::Result::Bad,
          "a message that is too long is dropped");
    Check(a.Feed(m2[0], &out, &error) == Assembler::Result::Complete && out == snapshot, "and the assembler works afterwards");
    for (const char* bad : {"@sef-store-2:1:0:1:", "@sef-store-2:00000000:+0:1:",
                           "@sef-store-2:00000000:0:18446744073709551617:",
                           "@sef-store-2:00000000:0:0:", "@sef-store-2:00000000:0:2:YQ=="}) {
        Check(a.Feed(bad, &out, &error) == Assembler::Result::Bad, "malformed numeric heading or short chunk refused");
    }
    Check(a.Feed(m2[0] + std::string("\0ignored", 8), &out, &error) == Assembler::Result::Bad,
          "embedded NUL cannot hide trailing bytes");
    auto over = mpstore::MakeMessages(std::string(mpstore::kMaxSnapshot, 'z'));
    // Last piece may not take the cumulative size past the limit, even with a legal count.
    over.back() += "AAAA";
    for (size_t i = 0; i + 1 < over.size(); ++i) a.Feed(over[i], &out, &error);
    Check(a.Feed(over.back(), &out, &error) == Assembler::Result::Bad, "assembly enforces the total byte limit");

    if (argc > 1) {
        bool expectKnown = argc > 2 && strcmp(argv[2], "--expect-known") == 0;
        HMODULE mod = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!mod) {
            printf("FAIL: cannot map %s (error %lu)\n", argv[1], GetLastError());
            return 1;
        }
        auto base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod) & ~uintptr_t(3));
        locate::Image img = locate::Describe(base);
        mpstore::Locations loc;
        if (!mpstore::Locate(img, loc, error)) {
            printf("FAIL: %s\n", error.c_str());
            return 1;
        }
        auto expect = [&](const char* what, unsigned long long got, unsigned long long want) {
            bool good = !expectKnown || got == want;
            printf("  %-16s 0x%llX%s\n", what, got, good ? "" : "  MISMATCH");
            if (!good) {
                printf("    want 0x%llX\n", want);
                ++g_failures;
            }
        };
        auto rva = [&](const void* p) { return static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - base); };
        expect("chatCtor", rva(loc.chatCtor), 0x1a9ce20);
        expect("chatExecute", rva(loc.chatExecute), 0x1a9d280);
        expect("chatExecuteSlot", rva(loc.chatExecuteSlot), 0x255fdc8 + 9 * 8);
        expect("post", rva(loc.post), 0x1a59590);
        expect("startSend", rva(loc.startSend), 0x1a5d020);
        expect("startSync", rva(loc.startSync), 0xb29800);
        expect("idler", rva(loc.idler), 0x3114e70);
        expect("operatorNew", rva(loc.operatorNew), 0x2021868);
        expect("sessionInit", rva(loc.sessionInit), 0x1a58a80);
        Check(loc.sessionId == 0x94 && loc.commandSender == 8 && loc.initialHostId == 1,
              "Post stamps the session identity verified in Init");
        Check(loc.transferName == 0x80 && loc.transferSession == 0x70, "file transfer name and session offsets");
        Check(memcmp(loc.sessionInit, "\x48\x89\x5C\x24\x10\x48\x89\x6C\x24\x18\x56\x57\x41\x56",
                     mpstore::kSessionInitStolenBytes) == 0, "session init stolen bytes are position independent");
        // The same in every build so far; a change means the game's classes changed.
        Check(loc.chatText == 0x20, "CChatCommand: the text is at +0x20");
        Check(loc.hostFlag == 0x9a8, "CSession: the host flag is at +0x9a8");
        Check(loc.idlerApp == 0x140 && loc.appSessionSlot == 0x70 && loc.sessionType == 0x58,
              "the session is the idler's +0x140, vtable +0x70; its type at +0x58");
        // The bytes the hooks move.
        Check(memcmp(loc.startSync, "\x48\x89\x5C\x24\x08\x48\x89\x6C\x24\x18\x48\x89\x74\x24\x20", mpstore::kStartSyncStolenBytes) == 0,
              "StartSyncPlayers starts with three register saves");
        Check(memcmp(loc.startSend, "\x48\x89\x5C\x24\x10\x55\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57", mpstore::kStartSendStolenBytes) == 0,
              "StartSend starts with a register save and seven pushes");
        // The chat command the DLL allocates must hold every field the constructor writes.
        Check(mpstore::kChatCommandSize >= 0x61, "the chat command's size covers its last field");
    }

    printf(g_failures ? "%d FAILED\n" : "mpstore_test: all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
