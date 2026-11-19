// The hook that puts the result-changing settings into the game's checksum (mpchecksum.h,
// re/multiplayer.md). Included by dllmain.cpp inside its anonymous namespace, after every feature's
// runtime, since it only reads the ini.

// ---- Multiplayer: result-changing settings in the game's checksum (mpchecksum.h) ----

mpchecksum::Locations g_mpChecksum;
std::string g_mpText; // names the settings that are on; never empty once the hook is in
using ChecksumFinalizeFn = void(__fastcall*)(uint8_t* checksum);
using Md5UpdateFn = void(__fastcall*)(uint8_t* md5, const unsigned char* bytes, uint32_t count);
ChecksumFinalizeFn g_origChecksumFinalize = nullptr;

// Before the game finishes a checksum of its files: the settings' text goes in after the files.
void __fastcall ChecksumFinalizeHook(uint8_t* checksum) {
    reinterpret_cast<Md5UpdateFn>(g_mpChecksum.md5Update)(checksum + g_mpChecksum.md5,
                                                          reinterpret_cast<const unsigned char*>(g_mpText.data()),
                                                          static_cast<uint32_t>(g_mpText.size()));
    Log(LogLevel::Info, "multiplayer", "the game's checksum now includes %s", g_mpText.c_str());
    g_origChecksumFinalize(checksum);
}

void InstallMpChecksum(HMODULE self, const locate::Image& img) {
    std::string ini = IniPath(self);
    if (ini.empty())
        return; // without an ini every result-changing setting is off
    std::vector<mpchecksum::Setting> settings;
    std::vector<char> entries(32768);
    for (const char* section : {"numeric_fixes", "ai_fixes"}) {
        DWORD n = GetPrivateProfileSectionA(section, entries.data(), static_cast<DWORD>(entries.size()), ini.c_str());
        if (n >= entries.size() - 2) {
            Log(LogLevel::Error, "multiplayer", "[%s] is too long to read; its settings are not in the game's checksum",
                section);
            continue;
        }
        mpchecksum::AddSection(section, entries.data(), settings);
    }
    // Storage in multiplayer only works when every player has it: the host's stores are sent as chat commands.
    mpchecksum::AddScriptStorage(GetPrivateProfileIntA("script_storage", "enabled", 1, ini.c_str()) != 0,
                                 GetPrivateProfileIntA("script_storage", "multiplayer", 0, ini.c_str()) != 0, settings);
    g_mpText = mpchecksum::Fingerprint(settings);
    if (g_mpText.empty()) {
        Log(LogLevel::Info, "multiplayer",
            "no setting that changes the game's results is on; the game's checksum is its own");
        return;
    }
    if (GetPrivateProfileIntA("multiplayer", "settings_in_checksum", 1, ini.c_str()) == 0) {
        Log(LogLevel::Warn, "multiplayer",
            "settings_in_checksum=0; the game's checksum is its own although these are on: %s. "
            "Players whose settings differ can join each other and may go out of sync",
            g_mpText.c_str());
        return;
    }
    std::string error;
    if (!mpchecksum::Locate(img, g_mpChecksum, error)) {
        Log(LogLevel::Error, "multiplayer", "%s; the game's checksum is its own although these are on: %s",
            error.c_str(), g_mpText.c_str());
        return;
    }
    auto tramp = InstallJumpHook(g_mpChecksum.finalize, mpchecksum::kFinalizeStolenBytes,
                                 reinterpret_cast<void*>(&ChecksumFinalizeHook));
    if (!tramp) {
        Log(LogLevel::Error, "multiplayer",
            "failed to hook CChecksum::Finalize; the game's checksum is its own although these are on: %s",
            g_mpText.c_str());
        return;
    }
    g_origChecksumFinalize = reinterpret_cast<ChecksumFinalizeFn>(tramp);
    Log(LogLevel::Info, "multiplayer",
        "Finalize +%llX, MD5::update +%llX, MD5 +0x%zx; the game's checksum will include the settings that are on",
        Rva(g_mpChecksum.finalize), Rva(g_mpChecksum.md5Update), g_mpChecksum.md5);
}
