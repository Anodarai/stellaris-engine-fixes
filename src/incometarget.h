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

// The rounds the game's loop reaches, given that `done` rounds ran without it leaving, or -1 if
// no resource's total ever passes its target, or the count doesn't fit the game's int32 counter,
// or a total would overflow on the way.
inline int64_t Rounds(const int64_t* t0, const int64_t* r, const int64_t* target, int n, int64_t done) {
    uint64_t k = UINT64_MAX;
    for (int i = 0; i < n; ++i) {
        if (r[i] <= 0) continue;  // a total that doesn't grow never starts to pass its target
        // The check holds once total > target (target >= 0), or total > 0 (target < 0).
        int64_t threshold = target[i] < 0 ? 0 : target[i];
        int64_t now = t0[i] + done * r[i];  // fits: the game got there
        if (now > threshold) return -1;     // the game would have left already
        uint64_t ki = (static_cast<uint64_t>(threshold) - static_cast<uint64_t>(t0[i])) / static_cast<uint64_t>(r[i]) + 1;
        if (ki < k) k = ki;
    }
    if (k == UINT64_MAX || k <= static_cast<uint64_t>(done) || k > INT32_MAX) return -1;
    for (int i = 0; i < n; ++i) {
        int64_t high;
        int64_t low = _mul128(r[i], static_cast<int64_t>(k), &high);
        if (high != (low >> 63)) return -1;
        int64_t sum = static_cast<int64_t>(static_cast<uint64_t>(t0[i]) + static_cast<uint64_t>(low));
        if (((t0[i] ^ sum) & (low ^ sum)) < 0) return -1;
    }
    return static_cast<int64_t>(k);
}

}  // namespace income
