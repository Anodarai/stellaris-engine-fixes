// Run as build\alloctest\alloc_host.exe (next to alloc_testdll.dll and the stellaris-engine-fixes.ini that
// build.bat copies from test\alloc_test.ini). Built with the static CRT, like stellaris.exe, so
// alloc_testdll.dll finds and replaces the same five CRT heap functions before this program's
// CRT starts. Checks that everything this program allocates comes from mimalloc and behaves as
// the CRT's heap did.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" __declspec(dllimport) const char* AllocTestLog();
extern "C" __declspec(dllimport) bool AllocTestInstalled();
extern "C" __declspec(dllimport) bool AllocTestOwned(const void* p);
extern "C" __declspec(dllimport) long AllocTestPurgeDelay();
extern "C" __declspec(dllimport) unsigned long long AllocTestForeign();
extern "C" __declspec(dllimport) const char* AllocTestSummary();

static int g_failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            ++g_failures;                                          \
            printf("FAIL line %d: %s\n", __LINE__, #cond);         \
        }                                                          \
    } while (0)

static std::vector<std::string> g_static = {"allocated", "before", "main"};  // during CRT start

int main() {
    printf("--- alloc_testdll log\n%s---\n", AllocTestLog());
    CHECK(AllocTestInstalled());
    CHECK(AllocTestOwned(g_static.data()));
    CHECK(AllocTestPurgeDelay() == 12345);  // from stellaris-engine-fixes.ini
    CHECK(GetEnvironmentVariableW(L"MIMALLOC_PURGE_DELAY", nullptr, 0) == 0);  // removed again

    // malloc, calloc, realloc, free, _msize
    void* p = malloc(100);
    CHECK(p && AllocTestOwned(p));
    CHECK(_msize(p) >= 100);
    memset(p, 0xAB, 100);
    p = realloc(p, 100000);
    CHECK(p && AllocTestOwned(p) && static_cast<unsigned char*>(p)[99] == 0xAB);
    CHECK(realloc(p, 0) == nullptr);  // frees, as the CRT's realloc does
    void* z = malloc(0);
    CHECK(z != nullptr);
    free(z);
    auto c = static_cast<unsigned char*>(calloc(1000, 7));
    bool zero = c != nullptr;
    for (int i = 0; zero && i < 7000; ++i) zero = c[i] == 0;
    CHECK(zero && AllocTestOwned(c));
    free(c);
    CHECK(calloc(SIZE_MAX / 2, 4) == nullptr);  // overflow
    free(nullptr);
    void* r = realloc(nullptr, 64);
    CHECK(r && AllocTestOwned(r));
    free(r);

    // The CRT functions built on those five
    auto* s = _strdup("strdup");
    CHECK(AllocTestOwned(s) && strcmp(s, "strdup") == 0);
    free(s);
    auto* rc = static_cast<int*>(_recalloc(nullptr, 10, sizeof(int)));
    CHECK(rc && AllocTestOwned(rc));
    rc = static_cast<int*>(_recalloc(rc, 1000, sizeof(int)));
    CHECK(rc && rc[999] == 0);
    free(rc);
    void* a = _aligned_malloc(1000, 4096);
    CHECK(a && (reinterpret_cast<uintptr_t>(a) & 4095) == 0);
    _aligned_free(a);
    auto big = std::make_unique<char[]>(64 << 20);
    CHECK(AllocTestOwned(big.get()));
    big.reset();
    std::string grow;
    for (int i = 0; i < 100000; ++i) grow += 'x';
    CHECK(AllocTestOwned(grow.data()));

    // Blocks from the process heap (as other modules allocate them) still go back to it.
    unsigned long long foreignBefore = AllocTestForeign();
    void* h = HeapAlloc(GetProcessHeap(), 0, 50);
    CHECK(!AllocTestOwned(h));
    CHECK(_msize(h) == 50);
    h = realloc(h, 5000);
    CHECK(h && !AllocTestOwned(h) && HeapSize(GetProcessHeap(), 0, h) == 5000);
    free(h);
    CHECK(AllocTestForeign() == foreignBefore + 3);

    // Many threads, freeing each other's blocks, then exiting.
    std::atomic<int> threadFailures{0};
    std::vector<std::atomic<void*>> shared(4096);
    for (int round = 0; round < 3; ++round) {
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                uint32_t x = 2463534242u + t;
                for (int i = 0; i < 200000; ++i) {
                    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
                    size_t size = (x % 7 == 0) ? (x % 300000) : (x % 512);
                    void* q = (x & 1) ? malloc(size) : calloc(1, size);
                    if (!q || !AllocTestOwned(q)) ++threadFailures;
                    if (q && size) static_cast<char*>(q)[size - 1] = 1;
                    void* old = shared[x % shared.size()].exchange(q);
                    if (x & 2) old = realloc(old, size + 16);
                    free(old);
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    for (auto& q : shared) free(q.exchange(nullptr));
    CHECK(threadFailures == 0);

    printf("%s\n", AllocTestSummary());
    printf(g_failures ? "%d FAILURES\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
