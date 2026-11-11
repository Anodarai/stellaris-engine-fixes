// Tests mpchecksum.h: the text made from the settings, and, given a stellaris.exe, the checksum
// functions it locates in it (mapped as an image; no code runs).
// Usage: mpchecksum_test.exe [<path to stellaris.exe> [--expect-known]]
//   --expect-known  also compare with the addresses of the Steam 4.5.2 exe (SHA-256 400DF27C...7A83)
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../src/mpchecksum.h"

static int g_failures = 0;

static void Check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

static std::string Text(const char* numeric, size_t numericSize, const char* ai, size_t aiSize) {
    // The sections as GetPrivateProfileSectionA gives them: entries end in NUL, the list in another.
    std::string n(numeric, numericSize), a(ai, aiSize);
    n.push_back('\0');
    a.push_back('\0');
    std::vector<mpchecksum::Setting> settings;
    mpchecksum::AddSection("numeric_fixes", n.c_str(), settings);
    mpchecksum::AddSection("ai_fixes", a.c_str(), settings);
    return mpchecksum::Fingerprint(settings);
}
#define FP(numeric, ai) Text(numeric, sizeof(numeric) - 1, ai, sizeof(ai) - 1)

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Nothing on: no text, so the game's checksum stays its own.
    Check(FP("", "").empty(), "empty sections give no text");
    Check(FP("resource_amount_rounding=0\0stockpile_arithmetic=0\0", "build_plan_keep=0\0colonizer_idle_days=0\0").empty(),
          "settings at 0 give no text");
    Check(FP("military_power_display=1\0", "").empty(), "a display-only setting gives no text");
    Check(FP("stockpile_arithmetic=off\0", "").empty(), "a value that reads as 0 gives no text");
    // GetPrivateProfileSectionA returns '#' lines (and leaves out ';' ones): notes are not settings.
    Check(FP("#stockpile_arithmetic=1\0 # resource_multiplication = 1 \0; fleet_power_arithmetic=1\0",
             "#colonizer_idle_days=180\0").empty(), "commented-out settings give no text");

    // What is on, sorted, with its value.
    Check(FP("stockpile_arithmetic=1\0resource_amount_rounding=1\0military_power_display=1\0",
               "colonizer_idle_days=180\0build_plan_keep=0\0") ==
              "sef-rules-1;ai_fixes.colonizer_idle_days=180;numeric_fixes.resource_amount_rounding=1;"
              "numeric_fixes.stockpile_arithmetic=1",
          "settings that are on are listed in order");
    Check(FP("stockpile_arithmetic=1\0", "") == FP(" Stockpile_Arithmetic = 1 \0", ""), "spaces and case don't matter");
    Check(FP("", "colonizer_idle_days=180\0") != FP("", "colonizer_idle_days=90\0"), "the value is part of the text");
    Check(FP("stockpile_arithmetic=1\0", "") != FP("resource_multiplication=1\0", ""), "the key is part of the text");
    Check(FP("some_new_fix=1\0", "") == "sef-rules-1;numeric_fixes.some_new_fix=1", "an unknown key counts");
    // Windows reads the first of two entries with one key.
    Check(FP("stockpile_arithmetic=0\0stockpile_arithmetic=1\0", "").empty(), "a repeated key: the first entry counts");
    Check(FP("stockpile_arithmetic=1\0stockpile_arithmetic=0\0", "") == "sef-rules-1;numeric_fixes.stockpile_arithmetic=1",
          "a repeated key: the first entry counts (on)");
    std::vector<mpchecksum::Setting> storage;
    mpchecksum::AddScriptStorage(false, true, storage);
    mpchecksum::AddScriptStorage(true, false, storage);
    Check(mpchecksum::Fingerprint(storage).empty(), "disabled multiplayer storage does not change the checksum");
    mpchecksum::AddScriptStorage(true, true, storage);
    Check(mpchecksum::Fingerprint(storage) == "sef-rules-1;script_storage.multiplayer=1",
          "enabled multiplayer storage changes the checksum");

    if (argc > 1) {
        bool expectKnown = argc > 2 && strcmp(argv[2], "--expect-known") == 0;
        HMODULE mod = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!mod) {
            printf("FAIL: cannot map %s (error %lu)\n", argv[1], GetLastError());
            return 1;
        }
        auto base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod) & ~uintptr_t(3));
        locate::Image img = locate::Describe(base);
        mpchecksum::Locations loc;
        std::string error;
        if (!mpchecksum::Locate(img, loc, error)) {
            printf("FAIL: %s\n", error.c_str());
            return 1;
        }
        auto expect = [&](const char* what, unsigned long long got, unsigned long long want) {
            bool good = !expectKnown || got == want;
            printf("  %-12s 0x%llX%s\n", what, got, good ? "" : "  MISMATCH");
            if (!good) {
                printf("    want 0x%llX\n", want);
                ++g_failures;
            }
        };
        expect("finalize", static_cast<unsigned long long>(loc.finalize - base), 0x1b35b20);
        expect("md5Update", static_cast<unsigned long long>(loc.md5Update - base), 0x1b554b0);
        expect("md5", loc.md5, 0x74);
        // The bytes the hook moves are the three register saves.
        Check(memcmp(loc.finalize, "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x48\x89\x7C\x24\x18", mpchecksum::kFinalizeStolenBytes) == 0,
              "Finalize starts with three register saves");
    }

    printf(g_failures ? "%d FAILED\n" : "mpchecksum_test: all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
