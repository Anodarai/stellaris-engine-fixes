// Multiplayer: the settings that change the game's results become part of the game's checksum.
//
// The game hashes its files into a checksum (CApplication::CalculateGameFilesChecksum, once at
// start-up); the version text ends in its first four digits, and a multiplayer host only accepts
// clients whose checksum equals its own (CChecksum::operator==, as with mods). Fixes that change
// what the simulation or the AI does must be the same for every player, so a text naming each such
// setting that is on goes into that hash, right before the game finishes it. Players whose settings
// differ then see different checksums and cannot join each other. With none of those settings on
// nothing is added, and the checksum is the game's own.
//
// The text is built from the ini: every key of [numeric_fixes] and [ai_fixes] with a value other
// than 0, sorted, except the keys known to change the display only. A key this file doesn't know
// is counted, so a new fix is covered without a change here.
//
// In the exe (the same in the Steam and GOG builds, 4.5.2 and 4.5.3):
//   CChecksum::SetupVersionObject(files, checksum) feeds every file, then the version name, to the
//   MD5 at checksum+0x74 (MD5::update), and ends in a jump to CChecksum::Finalize(checksum), which
//   finishes both MD5s and stores their digests. Finalize is hooked; the hook calls MD5::update
//   with the text and goes on to the game's function.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "locate.h"

namespace mpchecksum {

// Raise when a fix listed in the text starts to give other results than before for the same
// setting, so that players with the older DLL get another checksum.
constexpr char kRules[] = "sef-rules-1";

// Settings of the two sections that change the display only.
constexpr const char* kDisplayOnly[] = {"numeric_fixes.military_power_display"};

struct Setting {
    std::string section, key, value;
};

inline void AddScriptStorage(bool enabled, bool multiplayer, std::vector<Setting>& out) {
    if (enabled && multiplayer) out.push_back({"script_storage", "multiplayer", "1"});
}

// The entries of one ini section as GetPrivateProfileSectionA returns them: "key=value\0...\0\0".
inline void AddSection(const char* section, const char* entries, std::vector<Setting>& out) {
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    for (const char* p = entries; p && *p; p += strlen(p) + 1) {
        std::string line(p);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        // Windows leaves out ';' lines but returns '#' ones; neither is a setting anyone reads,
        // so a "#stockpile_arithmetic=1" left as a note must not change the checksum.
        if (key.empty() || key[0] == '#' || key[0] == ';') continue;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(tolower(c)); });
        out.push_back({section, key, trim(line.substr(eq + 1))});
    }
}

// The text for the settings, or "" when none of them changes the game's results. A value counts
// as the number the installers read (GetPrivateProfileInt: leading digits, 0 otherwise). Of two
// entries with one key the first counts, as it does for Windows.
inline std::string Fingerprint(const std::vector<Setting>& settings) {
    std::vector<std::string> on;
    std::vector<std::string> seen;
    for (const Setting& s : settings) {
        std::string name = s.section + "." + s.key;
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
        seen.push_back(name);
        bool display = false;
        for (const char* d : kDisplayOnly) display = display || name == d;
        if (display) continue;
        long value = strtol(s.value.c_str(), nullptr, 10);
        if (value == 0) continue;
        on.push_back(name + "=" + std::to_string(value));
    }
    if (on.empty()) return std::string();
    std::sort(on.begin(), on.end());
    std::string text = kRules;
    for (const std::string& s : on) text += ";" + s;
    return text;
}

struct Locations {
    uint8_t* finalize = nullptr;   // CChecksum::Finalize(checksum) (hooked)
    uint8_t* md5Update = nullptr;  // MD5::update(md5, bytes, uint32 count)
    size_t md5 = 0;                // CChecksum: the MD5 the files go into
};

// CChecksum::Finalize: its first 15 bytes (three `mov [rsp+N], reg`) are moved by the hook.
//   +34 mov rsi,rcx  +37 add rcx,8  +41 call MD5::finalize  +46 lea rcx,[rsi+md5]  +50 call
constexpr char kFinalize[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 48 8B F1 "
    "48 83 C1 08 E8 ?? ?? ?? ?? 48 8D 4E ?? E8";
constexpr size_t kFinalizeStolenBytes = 15;
// The end of CChecksum::SetupVersionObject, the version name:
//   +0 lea rcx,[r15+md5]  +4 mov r8d,[r15+name size]  +11 mov rdx,rbx  +14 call MD5::update
//   +19 mov rcx,rbx  +22 call free  +27 mov rcx,r15, the registers restored, jmp Finalize
constexpr char kVersionName[] = "49 8D 4F ?? 45 8B 87 ?? ?? ?? ?? 48 8B D3 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 49 8B CF";
constexpr size_t kVersionNameSearch = 48;  // from +30 to the jump

inline bool Locate(const locate::Image& img, Locations& out, std::string& error) {
    uint8_t* finalize = locate::FindOne(img, "CChecksum::Finalize", kFinalize, error);
    if (!finalize) return false;
    uint8_t* name = locate::FindOne(img, "CChecksum::SetupVersionObject's version name", kVersionName, error);
    if (!name) return false;
    bool jumps = false;
    for (size_t at = 30; at < 30 + kVersionNameSearch && !jumps; ++at) {
        jumps = name[at] == 0xE9 && locate::RelTarget(name + at + 1, name + at + 5) == finalize;
    }
    if (!jumps) {
        error = "CChecksum::SetupVersionObject doesn't end in a jump to CChecksum::Finalize";
        return false;
    }
    if (name[3] != finalize[49]) {
        error = "CChecksum: SetupVersionObject and Finalize disagree about the files' MD5";
        return false;
    }
    out.finalize = finalize;
    out.md5Update = locate::RelTarget(name + 15, name + 19);
    out.md5 = name[3];
    return true;
}

}  // namespace mpchecksum
