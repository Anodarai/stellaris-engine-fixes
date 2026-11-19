// Statistics for development builds. `build.bat dev` defines SEF_DEV_STATS: the counters and timers
// below are real, and the DLL logs its 30-second summaries. Without it (the release build) they are
// empty types whose calls compile to nothing, no summary is logged and no summary thread starts.
//
// Use these only for numbers that feed the log. A count the code acts on (a verify mode's mismatch
// limit, say) stays an ordinary variable.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace devstats {
#ifdef SEF_DEV_STATS
inline constexpr bool kOn = true;

using Counter = std::atomic<uint64_t>; // any thread
using MainCounter = uint64_t;          // one thread only

// QPC ticks, for Timer::AddSince and for code that times a span itself.
inline LONGLONG Ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Calls, total time and longest call; any thread. Take() reads them and starts again from zero,
// Total() reads them and keeps counting.
struct Timer {
    std::atomic<uint64_t> calls{0};
    std::atomic<LONGLONG> ticks{0}, maxTicks{0};
    void Add(LONGLONG t) {
        calls.fetch_add(1, std::memory_order_relaxed);
        ticks.fetch_add(t, std::memory_order_relaxed);
        LONGLONG seen = maxTicks.load(std::memory_order_relaxed);
        while (t > seen && !maxTicks.compare_exchange_weak(seen, t, std::memory_order_relaxed)) {
        }
    }
    void AddSince(LONGLONG start) { Add(Ticks() - start); }
    struct Taken {
        unsigned long long calls;
        double ms, maxUs;
        double AvgUs() const { return calls ? 1e3 * ms / calls : 0.0; }
    };
    Taken Take() { return Convert(calls.exchange(0), ticks.exchange(0), maxTicks.exchange(0)); }
    Taken Total() const { return Convert(calls.load(), ticks.load(), maxTicks.load()); }

  private:
    static Taken Convert(uint64_t calls, LONGLONG ticks, LONGLONG maxTicks) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return {calls, 1e3 * ticks / f.QuadPart, 1e6 * maxTicks / f.QuadPart};
    }
};

// Times its scope. Not for functions with __try: use Ticks() and AddSince there.
struct Scope {
    Timer& timer;
    LONGLONG start = Ticks();
    explicit Scope(Timer& t) : timer(t) {}
    ~Scope() { timer.AddSince(start); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

#else
inline constexpr bool kOn = false;

struct Counter {
    constexpr Counter(uint64_t = 0) {}
    uint64_t fetch_add(uint64_t, std::memory_order = std::memory_order_seq_cst) { return 0; }
    uint64_t load(std::memory_order = std::memory_order_seq_cst) const { return 0; }
    uint64_t exchange(uint64_t, std::memory_order = std::memory_order_seq_cst) { return 0; }
    bool compare_exchange_weak(uint64_t&, uint64_t, std::memory_order = std::memory_order_seq_cst) { return true; }
    uint64_t operator++() { return 0; }
    uint64_t operator++(int) { return 0; }
    uint64_t operator+=(uint64_t) { return 0; }
    operator uint64_t() const { return 0; }
};
using MainCounter = Counter;

inline LONGLONG Ticks() {
    return 0;
}

struct Timer {
    void Add(LONGLONG) {}
    void AddSince(LONGLONG) {}
};

struct Scope {
    explicit Scope(Timer&) {}
    ~Scope() {} // user-provided, so an unused Scope is not an "unreferenced local variable"
};
#endif
} // namespace devstats
