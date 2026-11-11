// Execute the original and patched rounding instructions, extracted from each supported exe.
// Usage: resourceoverflow_test.exe <stellaris.exe> [more builds...]
#include <cstdio>
#include <random>
#include "../src/resourceoverflow.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok && ++failures <= 20) printf("FAIL %s\n", what);
}

using RoundFn = int64_t (*)(int64_t);

// Windows x64 wrapper: save RBX, copy the first argument into RBX (the game's amount),
// run its position-independent arithmetic, put the result in RAX, restore RBX and return.
uint8_t* MakeRound(const uint8_t* block, bool situation) {
    auto p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!p) return nullptr;
    const uint8_t prefix[] = {0x53, 0x48, 0x8b, 0xd9}; // push rbx; mov rbx,rcx
    memcpy(p, prefix, sizeof(prefix));
    size_t n = sizeof(prefix);
    if (situation) {
        memcpy(p + n, block, 36); n += 36; // through imul r9,rax,100000
        const uint8_t result[] = {0x49, 0x8b, 0xc1}; // mov rax,r9
        memcpy(p + n, result, sizeof(result)); n += sizeof(result);
    } else {
        memcpy(p + n, block + 58, 27); n += 27; // through add rdx,rax
        // Skip resource-table addressing, which is not part of the rounding arithmetic.
        memcpy(p + n, block + resourceoverflow::kStockpileNarrow, 10); n += 10;
        const uint8_t result[] = {0x48, 0x8b, 0xc1}; // mov rax,rcx
        memcpy(p + n, result, sizeof(result)); n += sizeof(result);
    }
    p[n++] = 0x5b; p[n++] = 0xc3; // pop rbx; ret
    DWORD old;
    if (!VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(GetCurrentProcess(), p, n)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    return p;
}

void CheckArithmetic(const uint8_t* original, const uint8_t* patched, bool situation) {
    uint8_t* oldCode = MakeRound(original, situation);
    uint8_t* newCode = MakeRound(patched, situation);
    Check(oldCode && newCode, "allocate executable rounding snippets");
    if (!oldCode || !newCode) {
        if (oldCode) VirtualFree(oldCode, 0, MEM_RELEASE);
        if (newCode) VirtualFree(newCode, 0, MEM_RELEASE);
        return;
    }
    auto oldRound = reinterpret_cast<RoundFn>(oldCode);
    auto newRound = reinterpret_cast<RoundFn>(newCode);
    auto sample = [&](int64_t raw) {
        // Division then multiplication cannot overflow: truncation reduces the magnitude.
        int64_t units = raw / 100000;
        int64_t expected = units * 100000;
        uint32_t low = static_cast<uint32_t>(units);
        int32_t narrowed;
        memcpy(&narrowed, &low, sizeof(low)); // reproduce x64 narrowing without signed overflow
        int64_t oldExpected = static_cast<int64_t>(narrowed) * 100000;
        Check(oldRound(raw) == oldExpected, "original machine instructions match int32 truncation");
        Check(newRound(raw) == expected, "patched machine instructions match int64 truncation");
        if (units >= INT32_MIN && units <= INT32_MAX)
            Check(newRound(raw) == oldRound(raw), "ordinary-scale result unchanged");
    };
    const int64_t values[] = {
        INT64_MIN, INT64_MIN + 1, INT64_MAX - 1, INT64_MAX,
        -2147483649LL * 100000, -2147483648LL * 100000 - 99999,
        -2147483648LL * 100000, -2147483648LL * 100000 + 1,
        2147483647LL * 100000, 2147483648LL * 100000 - 1,
        2147483648LL * 100000, 2147483648LL * 100000 + 99999,
        4294967296LL * 100000, -4294967296LL * 100000,
        -100001, -100000, -99999, -1, 0, 1, 99999, 100000, 100001
    };
    for (int64_t value : values) sample(value);
    std::mt19937_64 rng(3);
    for (int i = 0; i < 100000; ++i) {
        uint64_t bits = rng();
        int64_t raw;
        memcpy(&raw, &bits, sizeof(raw));
        sample(raw);
        sample(raw % (2147483647LL * 100000));
    }
    const int64_t boundary = 2147483648LL * 100000;
    Check(oldRound(boundary) < 0 && newRound(boundary) == boundary, "positive overflow reproduction");
    Check(oldRound(2 * boundary) == 0 && newRound(2 * boundary) == 2 * boundary, "zero-wrap reproduction");
    printf("  %s: 2,147,483,648 units: original %lld, patched %lld; 200,023 cases\n",
           situation ? "situation" : "stockpile", oldRound(boundary) / 100000, newRound(boundary) / 100000);
    VirtualFree(oldCode, 0, MEM_RELEASE);
    VirtualFree(newCode, 0, MEM_RELEASE);
}

void CheckBuild(const char* path) {
    HMODULE mod = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    Check(mod != nullptr, "map executable as image resource");
    if (!mod) { printf("  %s: Windows error %lu\n", path, GetLastError()); return; }
    auto base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod) & ~uintptr_t(3));
    locate::Image img = locate::Describe(base);
    std::string error;
    uint8_t* at = resourceoverflow::Locate(img, error);
    Check(at != nullptr, "unique complete AddResource pattern");
    if (!at) { printf("  %s: %s\n", path, error.c_str()); FreeLibrary(mod); return; }
    printf("%s: rounding block +%llX\n", path, static_cast<unsigned long long>(at - base));
    uint8_t patched[resourceoverflow::kSize];
    Check(resourceoverflow::Prepare(at, patched), "prepare both patches");
    for (size_t i = 0; i < sizeof(patched); ++i) {
        bool changed = i == resourceoverflow::kSituationNarrow || i == resourceoverflow::kSituationNarrow + 1 ||
                       i == resourceoverflow::kStockpileNarrow + 1;
        Check((patched[i] != at[i]) == changed, "only the three intended bytes change");
    }
    CheckArithmetic(at, patched, false);
    CheckArithmetic(at, patched, true);

    uint8_t duplicate[resourceoverflow::kSize * 2];
    memcpy(duplicate, at, resourceoverflow::kSize);
    memcpy(duplicate + resourceoverflow::kSize, at, resourceoverflow::kSize);
    locate::Image fake;
    fake.code.push_back({duplicate, duplicate + sizeof(duplicate)});
    Check(resourceoverflow::Locate(fake, error) == nullptr, "ambiguous pattern rejected");
    fake.code[0].end = duplicate + resourceoverflow::kSize;
    Check(resourceoverflow::Locate(fake, error) == duplicate, "single synthetic pattern accepted");
    duplicate[resourceoverflow::kStockpileNarrow + 1] ^= 1;
    Check(resourceoverflow::Locate(fake, error) == nullptr, "changed stockpile instruction rejected");
    Check(!resourceoverflow::Prepare(duplicate, patched), "prepare rejects changed instructions");
    FreeLibrary(mod);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: resourceoverflow_test <stellaris.exe> [more builds...]\n"); return 2; }
    for (int i = 1; i < argc; ++i) CheckBuild(argv[i]);
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
