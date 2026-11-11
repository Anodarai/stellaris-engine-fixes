// The round count of CAIEconomicPlan::CalculateIncomeTarget's scaling loop, worked out instead of
// run (dllmain.cpp, "The AI's income targets"; re/ai.md section 6).
//
// The game's loop, with total = t0 before the first round:
//   for (rounds = 0;; ++rounds) {
//       for each resource i: if ((target[i] >= 0 || total[i] > 0) && target[i] < total[i]) leave;
//       if (no resources || the last round added nothing) leave;
//       total += r;  // the same r every round
//   }
#pragma once

#include <intrin.h>
#include <stdint.h>

namespace income {

// Check each intermediate before evaluating signed arithmetic. Refusing a shortcut leaves
// the game's loop in charge; it must never manufacture a delta from wrapped observations.
inline bool Difference(int64_t a, int64_t b, int64_t& out) {
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) return false;
    out = a - b;
    return true;
}

inline bool Total(int64_t initial, int64_t increment, int64_t rounds, int64_t& out) {
    int64_t high;
    int64_t low = _mul128(increment, rounds, &high);
    if (high != (low >> 63)) return false;
    if ((low > 0 && initial > INT64_MAX - low) || (low < 0 && initial < INT64_MIN - low)) return false;
    out = initial + low;
    return true;
}

// The rounds the game's loop reaches, given that `done` rounds ran without it leaving, or -1 if
// no resource's total ever passes its target, or the count doesn't fit the game's int32 counter,
// or a total would overflow on the way.
inline int64_t Rounds(const int64_t* t0, const int64_t* r, const int64_t* target, int n, int64_t done) {
    if (done < 0 || done > INT32_MAX || n <= 0) return -1;
    uint64_t k = UINT64_MAX;
    for (int i = 0; i < n; ++i) {
        int64_t now;
        if (!Total(t0[i], r[i], done, now)) return -1;
        if (r[i] <= 0) continue;  // a total that doesn't grow never starts to pass its target
        // The check holds once total > target (target >= 0), or total > 0 (target < 0).
        int64_t threshold = target[i] < 0 ? 0 : target[i];
        if (now > threshold) return -1;     // the game would have left already
        uint64_t quotient = (static_cast<uint64_t>(threshold) - static_cast<uint64_t>(t0[i])) /
                            static_cast<uint64_t>(r[i]);
        // +1 may itself wrap for the full signed range. Such a resource cannot be the
        // stopping condition within the game's counter, but a different resource can.
        if (quotient >= INT32_MAX) continue;
        uint64_t ki = quotient + 1;
        if (ki < k) k = ki;
    }
    if (k == UINT64_MAX || k <= static_cast<uint64_t>(done) || k > INT32_MAX) return -1;
    for (int i = 0; i < n; ++i) {
        int64_t sum;
        if (!Total(t0[i], r[i], static_cast<int64_t>(k), sum)) return -1;
    }
    return static_cast<int64_t>(k);
}

}  // namespace income
