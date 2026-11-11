// Actual x64 instructions from the four game binaries, compared with checked integer math.
#include <cstdio>
#include <random>
#include "../src/fixedpointoverflow.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok && ++failures <= 20) printf("FAIL %s\n", what);
}
void* Executable(const std::vector<uint8_t>& code) {
    void* p = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return nullptr;
    memcpy(p, code.data(), code.size());
    DWORD old;
    if (!VirtualProtect(p, code.size(), PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), p, code.size())) {
        VirtualFree(p, 0, MEM_RELEASE); return nullptr;
    }
    return p;
}
std::vector<uint8_t> InlineWrapper(const uint8_t* block) {
    std::vector<uint8_t> code = {
        0x53,0x48,0x8b,0xda, // push rbx; mov rbx,rdx (output for live-register checks)
        0x4c,0x8b,0xc1,      // mov r8,rcx
        0x49,0xba,0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,
        0x49,0xbb,0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01
    };
    code.insert(code.end(), block, block + fixedpointoverflow::kInlineSize);
    const uint8_t suffix[] = {0x4c,0x89,0x13, 0x4c,0x89,0x5b,0x08, 0x49,0x8b,0xc1, 0x5b,0xc3};
    code.insert(code.end(), std::begin(suffix), std::end(suffix));
    return code;
}

int64_t Reference(int64_t raw) {
    int64_t whole = raw / 32768, fraction = (raw % 32768) * 100000;
    fraction = (fraction + (fraction < 0 ? -16384 : 16384)) / 32768;
    if (whole > INT64_MAX / 100000) return INT64_MAX;
    if (whole < INT64_MIN / 100000) return INT64_MIN;
    int64_t result = whole * 100000;
    if (fraction > 0 && result > INT64_MAX - fraction) return INT64_MAX;
    if (fraction < 0 && result < INT64_MIN - fraction) return INT64_MIN;
    return result + fraction;
}

void Test(const char* path) {
    HMODULE module = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    Check(module != nullptr, "map executable");
    if (!module) return;
    auto base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module) & ~uintptr_t(3));
    auto img = locate::Describe(base);
    fixedpointoverflow::Locations loc{};
    std::string error;
    if (!fixedpointoverflow::Locate(img, loc, error)) {
        Check(false, error.c_str()); FreeLibrary(module); return;
    }
    printf("%s: conversion +%llX, fleet weight +%llX, tech weight +%llX\n", path,
           static_cast<unsigned long long>(loc.function-base),
           static_cast<unsigned long long>(loc.fleetWeight-base),
           static_cast<unsigned long long>(loc.techWeight-base));
    Check(locate::MatchAt(loc.fleetWeight, locate::Parse(fixedpointoverflow::kInline)), "fleet block exact");
    Check(locate::MatchAt(loc.techWeight, locate::Parse(fixedpointoverflow::kInline)), "tech block exact");
    {
        std::vector<uint8_t> bytes(1024,0xcc);
        memcpy(bytes.data(),loc.function,fixedpointoverflow::kFunctionSize);
        memcpy(bytes.data()+128,loc.fleetWeight-19,locate::Parse(fixedpointoverflow::kFleetWeight).size());
        memcpy(bytes.data()+512,loc.techWeight-18,locate::Parse(fixedpointoverflow::kTechWeight).size());
        locate::Image fake;
        fake.code.push_back({bytes.data(),bytes.data()+bytes.size()});
        fixedpointoverflow::Locations found{};
        Check(fixedpointoverflow::Locate(fake,found,error),"synthetic conversion patterns");
        bytes[512+14] ^= 4;
        Check(!fixedpointoverflow::Locate(fake,found,error),"conversion field relationship checked");
        bytes[512+14] ^= 4;
        bytes.insert(bytes.end(),loc.function,loc.function+fixedpointoverflow::kFunctionSize);
        fake.code[0] = {bytes.data(),bytes.data()+bytes.size()};
        Check(!fixedpointoverflow::Locate(fake,found,error),"duplicate conversion function rejected");
    }
    auto functionPatch = fixedpointoverflow::FunctionPatch();
    auto inlinePatch = fixedpointoverflow::InlinePatch();
    void* oldFn = Executable({loc.function, loc.function + fixedpointoverflow::kFunctionSize});
    void* newFn = Executable({functionPatch.begin(), functionPatch.end()});
    void* oldInline = Executable(InlineWrapper(loc.fleetWeight));
    void* oldTech = Executable(InlineWrapper(loc.techWeight));
    void* newInline = Executable(InlineWrapper(inlinePatch.data()));
    Check(oldFn && newFn && oldInline && oldTech && newInline, "allocate code");
    if (oldFn && newFn && oldInline && oldTech && newInline) {
        using Fn = int64_t* (*)(int64_t*, const int64_t*);
        using Inline = int64_t (*)(int64_t, uint64_t*);
        auto sample = [&](int64_t raw) {
            int64_t want = Reference(raw), oldValue = 0, value = 0;
            Check(reinterpret_cast<Fn>(newFn)(&value, &raw) == &value && value == want, "function result and ABI");
            uint64_t live[2]{};
            Check(reinterpret_cast<Inline>(newInline)(raw, live) == want, "inline result");
            Check(live[0] == 0xfedcba9876543210ULL && live[1] == 0x0123456789abcdefULL, "live registers preserved");
            int64_t whole = raw / 32768;
            if (whole >= INT32_MIN && whole <= INT32_MAX) {
                reinterpret_cast<Fn>(oldFn)(&oldValue, &raw);
                Check(value == oldValue, "ordinary function output unchanged");
                Check(reinterpret_cast<Inline>(oldInline)(raw, live) == value, "ordinary fleet conversion unchanged");
                Check(reinterpret_cast<Inline>(oldTech)(raw, live) == value, "ordinary tech conversion unchanged");
            }
        };
        const int64_t values[] = {INT64_MIN, INT64_MIN+1, INT64_MAX-1, INT64_MAX,
            -2147483649LL*32768, -2147483648LL*32768-32767, -2147483648LL*32768,
            2147483648LL*32768-1, 2147483648LL*32768, 4294967296LL*32768,
            -32769,-32768,-32767,-512,-1,0,1,512,32767,32768,32769};
        for (auto raw : values) sample(raw);
        // Every fractional remainder, including both signs and all halfway cases.
        for (int64_t raw = -32768; raw <= 32768; ++raw) sample(raw);
        // Decimal saturation boundary: sample raw values on either side of it.
        const int64_t edge = (INT64_MAX / 100000) * 32768 + ((INT64_MAX % 100000) * 32768) / 100000;
        for (int64_t d = -8; d <= 8; ++d) { sample(edge+d); sample(-edge+d); }
        std::mt19937_64 rng(3003);
        for (int i = 0; i < 100000; ++i) {
            uint64_t bits = rng(); int64_t raw; memcpy(&raw, &bits, 8);
            sample(raw); sample(raw % (2147483647LL * 32768));
        }
        int64_t input = 2147483648LL * 32768, oldValue, newValue;
        reinterpret_cast<Fn>(oldFn)(&oldValue, &input);
        reinterpret_cast<Fn>(newFn)(&newValue, &input);
        Check(oldValue < 0 && newValue == 2147483648LL * 100000, "positive-to-negative reproduction");
        printf("  2^31 units: original raw %lld, patched raw %lld; 265,592 inputs\n", oldValue, newValue);
        input = -512;
        Check(reinterpret_cast<Fn>(newFn)(&input, &input) == &input && input == -1563, "aliased input/output and negative tie");
    }
    for (void* p : {oldFn, newFn, oldInline, oldTech, newInline}) if (p) VirtualFree(p, 0, MEM_RELEASE);
    FreeLibrary(module);
}
} // namespace
int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: fixedpointoverflow_test <exe> [more builds...]\n"); return 2; }
    for (int i = 1; i < argc; ++i) Test(argv[i]);
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
