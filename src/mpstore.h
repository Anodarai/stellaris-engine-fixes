// Script storage in multiplayer: the host sends its stores to every player (re/multiplayer.md,
// re/script_storage.md).
//
// Each machine has its own sefdata\ files, so in multiplayer script that read them would get other
// answers on each machine and the game would go out of sync. Instead the host's two stores travel
// to every player as one snapshot, and from then on each machine changes its copy with the same
// script in the same tick. Only the host's copy is its real stores; only the host writes files.
//
// The snapshot travels as chat commands (CChatCommand), which the game already sends to every
// player through the ordered queue (CCommand's targeted-asynchronous byte +0x14 is zero), and
// also to a player who is still hot joining (CChatCommand::OkDuringHotJoin). MustSync is false;
// that virtual is not what selects the queue. Each command's text starts with kPrefix; a hook on
// CChatCommand::Execute takes those and the chat never shows them. The host posts a snapshot
//   - in CGameSetup::StartSyncPlayers, which only the host runs, when the lobby starts the game,
//     before the random seed and the galaxy settings, so before any machine creates the game;
//   - when it executes the kResync it posts in CLargeFileHandler::StartSend, as it starts to send
//     its game state to a player (a hot join, and a loaded save, which reaches the other players
//     the same way). A snapshot taken in StartSend itself would miss the script that runs before
//     its commands execute, and their arrival would undo those writes on every machine.
//
// This header has the parts that know nothing about the game (the snapshot's text, its messages
// and their assembly; test/mpstore_test.cpp) and where the game's functions are. dllmain.cpp,
// "Script storage in multiplayer", does the game's side.
#pragma once

#include <cstdint>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "locate.h"
#include "pdxstore.h"

namespace mpstore {

constexpr char kPrefix[] = "@sef-store-1:";
constexpr char kDisabled[] = "@sef-store-1:disabled";
// Posted by the host when a running game goes to a player. Where it executes, every machine stops
// storage, and the host takes its snapshot and posts it: taken at that ordered boundary, the
// snapshot holds every write made before it, and nothing is written until its last piece arrives.
constexpr char kResync[] = "@sef-store-1:resync";
constexpr size_t kChunkBytes = 2400;            // of the snapshot in one chat command, before base64
constexpr size_t kMaxSnapshot = 1024 * 1024;    // a larger one is not sent: storage stays off in that game
constexpr size_t kMaxMessage = 4096;            // a longer chat text is not ours

// ---- Text helpers ----

inline uint32_t Hash(const std::string& s) {  // FNV-1a
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

inline std::string Base64Encode(const std::string& in) {
    static const char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        if (i + 2 < in.size()) v |= static_cast<unsigned char>(in[i + 2]);
        out += kDigits[v >> 18];
        out += kDigits[(v >> 12) & 63];
        out += i + 1 < in.size() ? kDigits[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kDigits[v & 63] : '=';
    }
    return out;
}

inline bool Base64Decode(const std::string& in, std::string* out) {
    out->clear();
    if (in.size() % 4) return false;
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (size_t i = 0; i < in.size(); i += 4) {
        bool last = i + 4 == in.size();
        int pad = 0;
        uint32_t v = 0;
        for (size_t k = 0; k < 4; ++k) {
            char c = in[i + k];
            if (c == '=') {
                if (!last || k < 2) return false;
                ++pad;
                v <<= 6;
                continue;
            }
            int d = digit(c);
            if (d < 0 || pad) return false;  // no digit after a '='
            v = (v << 6) | static_cast<uint32_t>(d);
        }
        // Padding must not hide nonzero bits (reject noncanonical encodings).
        if ((pad == 2 && (v & 0xFFFF)) || (pad == 1 && (v & 0xFF))) return false;
        *out += static_cast<char>(v >> 16);
        if (pad < 2) *out += static_cast<char>((v >> 8) & 0xFF);
        if (pad < 1) *out += static_cast<char>(v & 0xFF);
    }
    return true;
}

// ---- The snapshot: both stores as one text ----
// "sef-snapshot-1 <bytes of the global store> <bytes of the save store>\n", then the two stores
// as pdxstore::Serialize writes them.

inline std::string MakeSnapshot(const pdxstore::Store& global, const pdxstore::Store& save) {
    std::string g = pdxstore::Serialize(global), s = pdxstore::Serialize(save);
    return "sef-snapshot-1 " + std::to_string(g.size()) + " " + std::to_string(s.size()) + "\n" + g + s;
}

// False and `error` when the text isn't a snapshot; the stores are then empty.
inline bool ReadSnapshot(const std::string& text, pdxstore::Store& global, pdxstore::Store& save, std::string& error) {
    global.Clear();
    save.Clear();
    global.SetClean();
    save.SetClean();
    size_t eol = text.find('\n');
    error.clear();
    constexpr char heading[] = "sef-snapshot-1 ";
    size_t g = 0, s = 0;
    size_t split = text.find(' ', sizeof(heading) - 1);
    auto number = [&](size_t begin, size_t end, size_t& value) {
        if (begin >= end || end > text.size()) return false;
        auto r = std::from_chars(text.data() + begin, text.data() + end, value);
        return r.ec == std::errc{} && r.ptr == text.data() + end;
    };
    if (text.size() > kMaxSnapshot || eol == std::string::npos || eol > 64 ||
        text.compare(0, sizeof(heading) - 1, heading) != 0 || split >= eol ||
        !number(sizeof(heading) - 1, split, g) || !number(split + 1, eol, s)) {
        error = "no snapshot heading";
        return false;
    }
    size_t body = text.size() - eol - 1;
    if (g > body || s != body - g) {
        error = "the sizes in the heading don't match the text";
        return false;
    }
    std::string why;
    pdxstore::Store newGlobal, newSave;
    if (!pdxstore::Deserialize(text.substr(eol + 1, g), newGlobal, why)) {
        error = "the global store: " + why;
        return false;
    }
    if (!pdxstore::Deserialize(text.substr(eol + 1 + g), newSave, why)) {
        error = "the save store: " + why;
        return false;
    }
    global = std::move(newGlobal);
    save = std::move(newSave);
    return true;
}

// ---- Messages: the snapshot cut into chat texts ----
// "@sef-store-1:<hash of the snapshot, 8 hex digits>:<index>:<count>:<base64 of this piece>"

inline std::vector<std::string> MakeMessages(const std::string& snapshot) {
    std::vector<std::string> out;
    if (snapshot.size() > kMaxSnapshot) return out;
    size_t count = snapshot.empty() ? 1 : (snapshot.size() + kChunkBytes - 1) / kChunkBytes;
    const uint32_t hash = Hash(snapshot);
    char head[64];
    for (size_t i = 0; i < count; ++i) {
        sprintf_s(head, "%s%08x:%zu:%zu:", kPrefix, hash, i, count);
        out.push_back(head + Base64Encode(snapshot.substr(i * kChunkBytes, kChunkBytes)));
    }
    return out;
}

inline bool IsMessage(const char* text, size_t size) {
    return size >= sizeof(kPrefix) - 1 && memcmp(text, kPrefix, sizeof(kPrefix) - 1) == 0;
}

// Puts the messages of one snapshot together again. They arrive in order; anything else drops
// what was collected.
class Assembler {
public:
    enum class Result {
        NotOurs,   // not a message: the chat's own text
        Partial,   // taken; more to come
        Complete,  // taken; *snapshot is the whole text
        Bad,       // taken and dropped; *error says why
    };

    Result Feed(const std::string& message, std::string* snapshot, std::string* error) {
        if (!IsMessage(message.data(), message.size())) return Result::NotOurs;
        auto bad = [&](const char* why) {
            Reset();
            *error = why;
            return Result::Bad;
        };
        if (message.size() > kMaxMessage || message.find('\0') != std::string::npos) return bad("invalid message length or NUL");
        const char* p = message.data() + sizeof(kPrefix) - 1;
        const char* end = message.data() + message.size();
        unsigned hash = 0;
        size_t index = 0, count = 0;
        if (end - p < 9 || p[8] != ':') return bad("a message has no hash");
        auto h = std::from_chars(p, p + 8, hash, 16);
        if (h.ec != std::errc{} || h.ptr != p + 8) return bad("a message has a bad hash");
        auto i = std::from_chars(p + 9, end, index);
        if (i.ec != std::errc{} || i.ptr == end || *i.ptr != ':') return bad("a message has no index");
        auto n = std::from_chars(i.ptr + 1, end, count);
        if (n.ec != std::errc{} || n.ptr == end || *n.ptr != ':') {
            return bad("a message has no heading");
        }
        if (count == 0 || index >= count || count > kMaxSnapshot / kChunkBytes + 1) return bad("a message's numbers are wrong");
        std::string piece;
        if (!Base64Decode(std::string(n.ptr + 1, end), &piece) || piece.size() > kChunkBytes ||
            (index + 1 < count && piece.size() != kChunkBytes)) return bad("a message's data is damaged");
        if (index == 0) {
            Reset();
            hash_ = hash;
            count_ = static_cast<size_t>(count);
        } else if (hash != hash_ || count != count_ || index != next_) {
            return bad("a message is out of order");
        }
        if (piece.size() > kMaxSnapshot - text_.size()) return bad("the snapshot is too large");
        text_ += piece;
        next_ = static_cast<size_t>(index) + 1;
        if (next_ < count_) return Result::Partial;
        std::string whole;
        whole.swap(text_);
        uint32_t expected = hash_;
        Reset();
        if (Hash(whole) != expected) return bad("the snapshot's hash is wrong");
        snapshot->swap(whole);
        return Result::Complete;
    }

    void Reset() {
        text_.clear();
        hash_ = 0;
        count_ = next_ = 0;
    }

private:
    std::string text_;
    uint32_t hash_ = 0;
    size_t count_ = 0, next_ = 0;
};

// ---- In the exe (the same in the Steam and GOG builds, 4.5.2 and 4.5.3) ----

struct Locations {
    uint8_t* chatCtor = nullptr;        // CChatCommand::CChatCommand(this, const CString& text, int, int, int, bool, int)
    uintptr_t* chatExecuteSlot = nullptr;  // CChatCommand's vtable slot of Execute (hooked)
    uint8_t* chatExecute = nullptr;     // CChatCommand::Execute(this)
    size_t chatText = 0;                // CChatCommand: its text, a CString
    uint8_t* post = nullptr;            // bool CSession::Post(session, CCommand*)
    size_t hostFlag = 0;                // CSession: a byte, nonzero on the host
    uint8_t* startSend = nullptr;       // CLargeFileHandler::StartSend(this, CBlob*, bool) (hooked)
    uint8_t* startSync = nullptr;       // CGameSetup::StartSyncPlayers(this) (hooked)
    uint8_t** idler = nullptr;          // g_CurrentIdler
    size_t idlerApp = 0;                // idler: the application
    size_t appSessionSlot = 0;          // the application's vtable offset of AccessSession()
    size_t sessionType = 0;             // CSession: ESessionType, an int
    uint8_t* operatorNew = nullptr;     // operator new(size_t)
    uint8_t* sessionInit = nullptr;     // CSession::Init; resets storage even if a session address is reused
    size_t sessionId = 0, commandSender = 0;
    uint32_t initialHostId = 0;
    size_t transferName = 0, transferSession = 0;
};
constexpr size_t kChatCommandSize = 0x68;  // as CChat::OnMessage allocates it
constexpr int kLobbyHostSession = 6;       // the session type StartSyncPlayers turns into 9

// CChatCommand's constructor: the CCommand fields, +67 lea rax,[vtable], the text copied to +0x20
// (+140 call CString::assign), then the three ints, the int and the bool at +0x50..+0x60.
constexpr char kChatCtor[] =
    "48 89 5C 24 10 48 89 74 24 18 48 89 4C 24 08 57 48 83 EC 20 41 8B F9 41 8B F0 48 8B D9 49 C7 C0 FF FF FF FF "
    "44 89 41 08 45 33 C9 44 89 49 0C C7 41 10 00 00 FF FF 66 44 89 49 14 44 88 49 16 44 89 49 18 48 8D 05 ?? ?? ?? ?? "
    "48 89 01 48 83 C1 20 48 83 C2 10 48 83 7A 18 10 72 03 48 8B 12 44 89 09 4C 89 49 08 4C 89 49 10 4C 89 49 20 "
    "48 C7 41 28 0F 00 00 00 C6 41 10 00 66 0F 1F 44 00 00 49 FF C0 42 80 3C 02 00 75 F6 E8 ?? ?? ?? ?? "
    "89 73 50 89 7B 54 8B 44 24 50 89 43 58 8B 44 24 60 89 43 5C 0F B6 44 24 58 88 43 60";
// CChatCommand::Execute: +4 mov r11,[g_pCurrentChatBase]  +19 lea rdx,[rcx+text]; the ints and
// the bool passed on to the chat's vtable +0x68.
constexpr char kChatExecute[] =
    "48 83 EC 48 4C 8B 1D ?? ?? ?? ?? 4D 85 DB 74 2C 8B 41 5C 48 8D 51 20 44 8B 49 54 44 8B 41 50 4D 8B 13 89 44 24 30 "
    "0F B6 41 60 88 44 24 28 8B 41 58 49 8B CB 89 44 24 20 41 FF 52 68 48 83 C4 48 C3";
// CSession::Post: +28 cmp byte [rcx+host],0; a client's command is checked first, the host's is not.
constexpr char kSessionPost[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 83 79 60 00 48 8B DA 48 8B F9 74 ?? 48 85 D2 74 ?? 80 B9 ?? ?? ?? ?? 00 75 ?? "
    "80 B9 ?? ?? ?? ?? 00 75 ?? 48 8B 02 48 8B CA FF 50 ?? 84 C0 75 ?? 48 8B 03 48 8B CB FF 50 ?? 84 C0 74 ??";
// CLargeFileHandler::StartSend: its first 16 bytes (a mov to the stack and seven pushes) are moved by the hook.
constexpr char kStartSend[] =
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 41 0F B6 D8 4C 8B EA 48 8B F1 "
    "33 C0 8B F8 89 85 ?? ?? ?? ?? 49 C7 C7 FF FF FF FF 4C 89 B9 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 28 "
    "F2 0F 11 86";
constexpr size_t kStartSendStolenBytes = 16;
// CGameSetup::StartSyncPlayers: its first 15 bytes (three `mov [rsp+N], reg`) are moved by the hook.
//   +69 mov rcx,[g_CurrentIdler]  +84 mov rcx,[rcx+app]  +94 call [rax+AccessSession]  +105 cmp
//   dword [rsi+type],6  +111 mov dword [rsi+type],9  +118 mov ecx,0x28  +123 call operator new
constexpr char kStartSync[] =
    "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8B E9 48 8B 0D ?? ?? ?? ?? "
    "48 8B 01 FF 50 38 8B 1D ?? ?? ?? ?? 8B 3D ?? ?? ?? ?? 83 3D ?? ?? ?? ?? FF 0F 4F 1D ?? ?? ?? ?? "
    "48 8B 0D ?? ?? ?? ?? 45 33 F6 48 85 C9 74 12 48 8B 89 ?? ?? ?? ?? 48 8B 01 FF 50 ?? 48 8B F0 EB 03 49 8B F6 "
    "83 7E 58 06 75 07 C7 46 58 09 00 00 00 B9 28 00 00 00 E8";
constexpr size_t kStartSyncStolenBytes = 15;
constexpr char kSessionInit[] =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 20 48 8D 99 ?? ?? ?? ?? "
    "45 8B F1 83 7B 14 64 41 8B F0 48 8B EA 48 8B F9";
constexpr size_t kSessionInitStolenBytes = 14;

inline bool Locate(const locate::Image& img, Locations& out, std::string& error) {
    uint8_t* ctor = locate::FindOne(img, "CChatCommand::CChatCommand", kChatCtor, error);
    if (!ctor) return false;
    uint8_t* execute = locate::FindOne(img, "CChatCommand::Execute", kChatExecute, error);
    if (!execute) return false;
    uint8_t* post = locate::FindOne(img, "CSession::Post", kSessionPost, error);
    if (!post) return false;
    uint8_t* send = locate::FindOne(img, "CLargeFileHandler::StartSend", kStartSend, error);
    if (!send) return false;
    uint8_t* sync = locate::FindOne(img, "CGameSetup::StartSyncPlayers", kStartSync, error);
    if (!sync) return false;
    uint8_t* init = locate::FindOne(img, "CSession::Init", kSessionInit, error);
    if (!init) return false;
    uint8_t* identity = locate::FindWithin(init, 0x120, "CSession::Init identity",
        "89 77 ?? 44 89 77 ?? C7 87 ?? ?? ?? ?? 01 00 00 00 C7 87 ?? ?? ?? ?? 02 00 00 00", error);
    uint8_t* sender = locate::FindWithin(post, 0x90, "CSession::Post sender",
        "8B 87 ?? ?? ?? ?? B9 FF 7F 00 00 89 43 ?? 48 8B D3", error);
    uint8_t* name = locate::FindWithin(send, 0x400, "CLargeFileHandler::StartSend name",
        "48 8D 4F 20 48 8D 96 ?? ?? ?? ?? 48 83 7A 18 10", error);
    uint8_t* transfer = locate::FindWithin(send, 0x100, "CLargeFileHandler::StartSend session",
        "84 DB 74 ?? 48 8B 4E ?? E8", error);
    if (!identity || !sender || !name || !transfer) return false;
    if (locate::Disp32(identity + 9) != locate::Disp32(sender + 2) ||
        locate::Disp32(name + 7) < 0x10 || identity[6] != sync[107]) {
        error = "multiplayer session identity or transfer layout disagrees";
        return false;
    }

    // The constructor's vtable must have Execute in one of its first slots. The image may be
    // mapped somewhere else than it was linked for, so its pointers are taken relative to
    // pointerBase.
    auto* vtable = reinterpret_cast<uintptr_t*>(locate::RelTarget(ctor + 70, ctor + 74));
    uintptr_t want = static_cast<uintptr_t>(execute - img.base) + img.pointerBase;
    uintptr_t* slot = nullptr;
    for (size_t i = 1; i < 16 && !slot; ++i) {
        if (vtable[i] == want) slot = vtable + i;
    }
    if (!slot) {
        error = "CChatCommand: Execute is not in the constructor's vtable";
        return false;
    }
    if (locate::Disp32(post + 30) <= 0 || locate::Disp32(post + 30) > 0x10000) {
        error = "CSession::Post: no host flag";
        return false;
    }
    out.chatCtor = ctor;
    out.chatExecuteSlot = slot;
    out.chatExecute = execute;
    out.chatText = execute[22];
    out.post = post;
    out.hostFlag = static_cast<size_t>(locate::Disp32(post + 30));
    out.startSend = send;
    out.startSync = sync;
    out.idler = reinterpret_cast<uint8_t**>(locate::RelTarget(sync + 72, sync + 76));
    out.idlerApp = static_cast<size_t>(locate::Disp32(sync + 87));
    out.appSessionSlot = sync[96];
    out.sessionType = sync[107];
    out.operatorNew = locate::RelTarget(sync + 124, sync + 128);
    out.sessionInit = init;
    out.sessionId = static_cast<size_t>(locate::Disp32(sender + 2));
    out.commandSender = sender[13];
    out.initialHostId = static_cast<uint32_t>(locate::Disp32(identity + 13));
    out.transferName = static_cast<size_t>(locate::Disp32(name + 7)) - 0x10; // CString's buffer
    out.transferSession = transfer[7];
    return true;
}

}  // namespace mpstore
