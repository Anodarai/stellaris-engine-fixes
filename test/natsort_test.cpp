// Unit tests for natsort.h.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/natsort.h"

static int g_failures = 0;

static void ExpectOrder(std::vector<std::string> expected) {
    std::vector<std::string> v = expected;
    std::reverse(v.begin(), v.end());
    std::stable_sort(v.begin(), v.end(),
                     [](const std::string& a, const std::string& b) { return natsort::Compare(a, b) < 0; });
    if (v != expected) {
        ++g_failures;
        printf("FAIL order:\n");
        for (auto& s : v) printf("  got  %s\n", s.c_str());
        for (auto& s : expected) printf("  want %s\n", s.c_str());
    }
}

static void ExpectCmp(const char* a, const char* b, int sign) {
    int c = natsort::Compare(a, b);
    int got = (c > 0) - (c < 0);
    if (got != sign) {
        ++g_failures;
        printf("FAIL Compare(\"%s\", \"%s\") = %d, want %d\n", a, b, got, sign);
    }
}

static void ExpectStrip(const char* in, const char* want) {
    std::string got = natsort::StripMarkup(in);
    if (got != want) {
        ++g_failures;
        printf("FAIL StripMarkup(\"%s\") = \"%s\", want \"%s\"\n", in, got.c_str(), want);
    }
}

int main() {
    // Numbers compare by value.
    ExpectOrder({"1 Doomstack", "2 Defense", "10 Patrol", "100 Reserve"});
    ExpectOrder({"Fleet 2", "Fleet 9", "Fleet 10", "Fleet 11", "Fleet 100"});
    ExpectOrder({"1st GDF Fleet", "2nd GDF Fleet", "10th GDF Fleet"});
    ExpectCmp("Fleet 007", "Fleet 7", 0);
    ExpectCmp("Fleet 007", "Fleet 8", -1);
    ExpectCmp("a10b", "a9c", 1);
    ExpectCmp("x", "x0", -1);

    // Case-insensitive, otherwise byte-wise.
    ExpectCmp("attack moon", "Attack Moon", 0);
    ExpectOrder({"Attack Moon", "Behemoth Planetcraft", "Deathclaw", "Deathclaw II", "Deathclaw III"});

    // Names from the logging run, in the expected natural order.
    ExpectOrder({"1st GDF Fleet", "Attack Moon", "Behemoth Planetcraft", "Coldsquall Voidwings", "Deathclaw",
                 "Deathclaw II", "Deathclaw III", "Final Flight", "Galesurge Voidwings", "Irass", "Izta",
                 "Last Breath", "Last Breath II", "Mira", "Othvomia", "Shadelurk Voidwings", "Siplim", "Skat",
                 "Terminal Egress", "Windstorm Voidwings"});

    // Markup is ignored.
    ExpectStrip("\x11TDeathclaw\x11!", "Deathclaw");
    ExpectStrip("\xC2\xA7YGold\xC2\xA7! Fleet", "Gold Fleet");
    ExpectStrip("\xC2\xA3military_power\xC2\xA3 Strike  Group ", "Strike Group");
    ExpectStrip("Caf\xC3\xA7", "Caf\xC3\xA7");  // UTF-8 bytes that merely contain A7 survive
    ExpectStrip("  ", "");
    ExpectStrip("\xE2\x80\x8BGrand Archive", "Grand Archive");  // as logged in the game
    ExpectStrip("a\xE2\x80\x8C\xE2\x80\x8D\xE2\x81\xA0\xEF\xBB\xBF" "b", "ab");
    ExpectStrip("\xE2\x80\x94 dash", "\xE2\x80\x94 dash");  // other E2 80 xx characters survive
    ExpectOrder({"Dyson Sphere", "E.H.O.F. Ultima", natsort::StripMarkup("\xE2\x80\x8BGrand Archive"), "HRAE-MC"});

    // Strict weak ordering over a mixed set: irreflexive, and equivalence/less are transitive.
    std::vector<std::string> s = {"", "0", "00", "1", "01", "9", "10", "a", "A", "a0", "a00", "a1", "a01",
                                  "a9", "a10", "a10b", "a9c", "ab", "a b", "Fleet 2", "fleet 02", "Fleet 10",
                                  "1st", "01st", "z", "Z9", "\xC3\xA7", "~"};
    auto less = [](const std::string& a, const std::string& b) { return natsort::Compare(a, b) < 0; };
    auto equiv = [&](const std::string& a, const std::string& b) { return !less(a, b) && !less(b, a); };
    for (auto& a : s) {
        if (less(a, a)) { ++g_failures; printf("FAIL irreflexive: %s\n", a.c_str()); }
        for (auto& b : s) {
            if (natsort::Compare(a, b) != -natsort::Compare(b, a) &&
                ((natsort::Compare(a, b) > 0) != (natsort::Compare(b, a) < 0))) {
                ++g_failures;
                printf("FAIL antisymmetric: %s / %s\n", a.c_str(), b.c_str());
            }
            for (auto& c : s) {
                if (less(a, b) && less(b, c) && !less(a, c)) {
                    ++g_failures;
                    printf("FAIL transitive <: %s %s %s\n", a.c_str(), b.c_str(), c.c_str());
                }
                if (equiv(a, b) && equiv(b, c) && !equiv(a, c)) {
                    ++g_failures;
                    printf("FAIL transitive ~: %s %s %s\n", a.c_str(), b.c_str(), c.c_str());
                }
            }
        }
    }

    printf(g_failures ? "%d FAILURES\n" : "all natsort tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
