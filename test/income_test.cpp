// Checks income::Rounds against the game's loop run round by round (src/incometarget.h).
#include <cstdio>
#include <random>

#include "../src/incometarget.h"

namespace {

// The game's loop, from `done` rounds on; -1 if it runs past `limit`.
int64_t GameRounds(const int64_t* t0, const int64_t* r, const int64_t* target, int n, int64_t done, int64_t limit,
                   bool addsSomething) {
    int64_t total[8];
    for (int i = 0; i < n; ++i) total[i] = t0[i] + done * r[i];
    for (int64_t rounds = done;; ++rounds) {
        for (int i = 0; i < n; ++i) {
            if ((target[i] >= 0 || total[i] > 0) && target[i] < total[i]) return rounds;
        }
        if (n == 0 || !addsSomething) return rounds;
        if (rounds >= limit) return -1;
        for (int i = 0; i < n; ++i) total[i] += r[i];
    }
}

int g_failures = 0;

void Check(const char* what, const int64_t* t0, const int64_t* r, const int64_t* target, int n, int64_t done) {
    int64_t game = GameRounds(t0, r, target, n, done, 200000, true);
    int64_t ours = income::Rounds(t0, r, target, n, done);
    // Ours may give up (-1) where the game runs long; when it answers, it must be the game's answer.
    if (ours >= 0 && ours != game && (game >= 0 || ours < 200000)) {
        if (++g_failures <= 10) printf("FAIL %s: game %lld, ours %lld\n", what, (long long)game, (long long)ours);
    }
    if (ours < 0 && game >= 0 && game > done) {
        // Only acceptable when the game would have left before `done` rounds ran (not this loop).
        int64_t fromStart = GameRounds(t0, r, target, n, 0, 200000, true);
        if (fromStart > done || fromStart < 0) {
            if (++g_failures <= 10) printf("FAIL %s: game %lld, ours gave up\n", what, (long long)game);
        }
    }
}

}  // namespace

int main() {
    auto require = [](bool ok, const char* what) {
        if (!ok) { printf("FAIL %s\n", what); ++g_failures; }
    };
    {
        int64_t value = 17;
        require(!income::Difference(INT64_MAX, -1, value) && value == 17, "positive delta overflow");
        require(!income::Difference(INT64_MIN, 1, value) && value == 17, "negative delta overflow");
        require(income::Difference(INT64_MIN, INT64_MIN, value) && value == 0, "equal minimum delta");
        require(income::Difference(-1, INT64_MIN, value) && value == INT64_MAX, "maximum valid delta");
        require(income::Total(INT64_MAX - 6, 2, 3, value) && value == INT64_MAX, "maximum valid total");
        require(income::Total(INT64_MIN + 6, -2, 3, value) && value == INT64_MIN, "minimum valid total");
        value = 17;
        require(!income::Total(INT64_MAX - 5, 2, 3, value) && value == 17, "positive total overflow");
        require(!income::Total(INT64_MIN + 5, -2, 3, value) && value == 17, "negative total overflow");
        require(!income::Total(0, INT64_MAX, 2, value) && value == 17, "product overflow");
        require(!income::Total(0, INT64_MIN, -1, value) && value == 17, "minimum times negative one");
    }
    {
        // The first threshold distance is UINT64_MAX: its quotient + 1 used to wrap
        // to zero and discard a valid stopping condition from the other resource.
        int64_t t0[] = {INT64_MIN, 0}, r[] = {1, 1}, target[] = {INT64_MAX, 2};
        require(income::Rounds(t0, r, target, 2, 1) == 3, "full-width distance with another stopping resource");
        require(income::Rounds(t0, r, target, 1, 1) == -1, "counter cannot reach full-width distance");
        require(income::Rounds(t0, r, target, 2, -1) == -1, "invalid observed round count");
        t0[0] = INT64_MIN + 1; r[0] = -1;
        require(income::Rounds(t0, r, target, 2, 2) == -1, "observed negative resource already overflowed");
        t0[0] = INT64_MAX - 1; r[0] = 1;
        require(income::Rounds(t0, r, target, 2, 2) == -1, "observed positive resource already overflowed");
    }
    // The case seen in the game: one resource grows by 500000 a round towards 6.37e14.
    {
        int64_t t0[3] = {1000000, 0, 323971963500000 - 647943923LL * 500000};
        int64_t r[3] = {0, 0, 500000};
        int64_t target[3] = {68018196482409, 0, 637025288226593};
        int64_t k = income::Rounds(t0, r, target, 3, 1);
        int64_t want = (637025288226593 - t0[2]) / 500000 + 1;
        printf("seen case: %lld rounds (want %lld)\n", (long long)k, (long long)want);
        if (k != want) ++g_failures;
    }
    std::mt19937_64 rng(12345);
    auto pick = [&](int64_t lo, int64_t hi) { return std::uniform_int_distribution<int64_t>(lo, hi)(rng); };
    for (int t = 0; t < 200000; ++t) {
        int n = static_cast<int>(pick(1, 6));
        int64_t t0[8], r[8], target[8];
        for (int i = 0; i < n; ++i) {
            t0[i] = pick(-50, 50) * (pick(0, 1) ? 1 : 1000);
            r[i] = pick(0, 3) == 0 ? 0 : pick(-20, 40);
            target[i] = pick(-200, 5000);
        }
        int64_t done = pick(1, 3);
        // Only states the game reaches: it didn't leave in the first `done` rounds.
        int64_t first = GameRounds(t0, r, target, n, 0, done, true);
        if (first >= 0 && first < done) continue;
        Check("random", t0, r, target, n, done);
    }
    // Overflow: a total that would wrap must give up, not answer.
    {
        int64_t t0[2] = {0, INT64_MAX - 10};
        int64_t r[2] = {1, 5};
        int64_t target[2] = {1000, INT64_MAX};
        if (income::Rounds(t0, r, target, 2, 1) != -1) {
            printf("FAIL overflow: answered\n");
            ++g_failures;
        }
    }
    // Nothing grows: give up (the game would loop until its total wraps).
    {
        int64_t t0[2] = {0, 0};
        int64_t r[2] = {0, -3};
        int64_t target[2] = {10, 10};
        if (income::Rounds(t0, r, target, 2, 1) != -1) {
            printf("FAIL no growth: answered\n");
            ++g_failures;
        }
    }
    printf(g_failures ? "FAILED (%d)\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
