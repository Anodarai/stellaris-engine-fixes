#include "../src/interface/starbase/starbaseautoupgrade.h"
#include <cstdio>
#include <cstdlib>
using namespace starbaseautoupgrade;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "auto-upgrade check failed: %s\n", what);
        std::exit(1);
    }
}
bool Same(const std::vector<uint32_t>& a, std::initializer_list<uint32_t> b) {
    return a == std::vector<uint32_t>(b);
}
int main() {
    Schedule s;
    Check(s.Enable(7, 1, 1000) && s.Enabled(7), "enable");
    Check(Same(s.TakeDue(1000), {7}), "first run on the day enabled");
    Check(s.Find(7)->nextDay == 1030, "next run a month later");
    Check(s.TakeDue(1000).empty() && s.TakeDue(1029).empty(), "not again within the month");
    Check(Same(s.TakeDue(1030), {7}) && s.Find(7)->nextDay == 1060, "same day next month");

    // A date that jumps several months runs once and keeps the day of the month.
    Check(Same(s.TakeDue(1145), {7}) && s.Find(7)->nextDay == 1150, "missed months run once");
    Check(s.TakeDue(1149).empty() && Same(s.TakeDue(1150), {7}), "anchor kept after a jump");

    // Each base keeps its own anchor; ids come back in order.
    Check(s.Enable(3, 1, 1160), "second base");
    Check(Same(s.TakeDue(1180), {3, 7}), "both due");
    Check(s.Find(3)->nextDay == 1190 && s.Find(7)->nextDay == 1210, "separate anchors");

    // Turning it off and on again restarts from the new day.
    Check(s.Disable(7) && !s.Enabled(7) && !s.Disable(7), "disable");
    Check(s.Enable(7, 1, 1195) && Same(s.TakeDue(1195), {3, 7}), "re-enable runs that day");
    Check(s.Find(7)->nextDay == 1225 && s.Find(3)->nextDay == 1220, "re-enable anchor");
    Check(!s.Enable(9, 1, -1), "no date");

    // Round trip and transactional parsing.
    const auto text = s.Serialize();
    Schedule t;
    Check(t.Deserialize(text) && t.bases.size() == 2 && t.Find(3)->nextDay == 1220 && t.Find(7)->owner == 1 &&
              t.Serialize() == text,
          "round trip");
    for (const char* bad : {"", "SEF_AUTOUPGRADE_2 0", "SEF_AUTOUPGRADE_1 1 5 1", "SEF_AUTOUPGRADE_1 1 5 1 -3",
                            "SEF_AUTOUPGRADE_1 2 5 1 3 5 1 4", "SEF_AUTOUPGRADE_1 0 junk", "SEF_AUTOUPGRADE_1 99999"}) {
        Check(!t.Deserialize(bad), bad);
        Check(t.Serialize() == text, "failed parse leaves schedule");
    }
    Check(t.Deserialize("SEF_AUTOUPGRADE_1 0") && t.bases.empty(), "empty schedule");

    // A limit takes the first due bases; the rest stay due and keep their day of the month.
    Schedule many;
    for (uint32_t i = 1; i <= 5; ++i)
        Check(many.Enable(i, 1, 100), "enable many");
    Check(Same(many.TakeDue(100, 2), {1, 2}), "limit takes the first");
    Check(Same(many.TakeDue(100, 2), {3, 4}) && Same(many.TakeDue(101, 2), {5}), "rest on later passes");
    Check(many.Find(5)->nextDay == 130 && many.Find(1)->nextDay == 130, "late base keeps its day");
    Check(many.TakeDue(129, 2).empty() && many.TakeDue(130, 0).empty(), "nothing early; zero takes none");

    Schedule full;
    for (uint32_t i = 0; i < kMaxBases; ++i)
        Check(full.Enable(i, 1, 0), "fill");
    Check(!full.Enable(kMaxBases, 1, 0) && full.Enable(0, 1, 5), "limit, but existing base re-enables");

    // Tooltip: the reason it can't be used wins; then off; then on with the next date and last run.
    const Entry on{1, 1030};
    Check(TooltipText("Not your starbase", &on, "2200.02.11", LastRun::Queued) ==
              "Auto-upgrade defense platforms\nNot available: Not your starbase",
          "tooltip unavailable");
    Check(TooltipText(nullptr, nullptr, "", LastRun::None).rfind("Auto-upgrade defense platforms: off\n", 0) == 0,
          "tooltip off");
    const auto queued = TooltipText(nullptr, &on, "2200.02.11", LastRun::Queued);
    Check(queued.find("on\nNext run: 2200.02.11\nLast run: upgrades queued\nClick to turn off.") != std::string::npos,
          "tooltip on");
    Check(TooltipText(nullptr, &on, "x", LastRun::Refused).find("Last run: nothing queued") != std::string::npos &&
              TooltipText(nullptr, &on, "x", LastRun::None).find("Last run: not this session") != std::string::npos,
          "tooltip last run");
    std::printf("PASS\n");
    return 0;
}
