#include "../src/interface/starbase/starbasetemplates_panel.h"
#include <cstdio>
#include <cstdlib>
using namespace starbasetemplates;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "starbase panel check failed: %s\n", what);
        std::exit(1);
    }
}
int main() {
    std::map<std::string, Layout> library, local;
    auto empty = BuildPage(library, local, "", 3, false);
    Check(empty.rows.empty() && empty.pages == 1 && empty.page == 0 && empty.selected.empty(), "empty library");
    Check(empty.pageLabel == "No templates saved", "empty label");

    for (int i = 0; i < 27; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "T%02d", i);
        library[name] = Layout{"starbase_level_citadel", {}, {}};
    }
    local["T05"] = library["T05"];
    auto first = BuildPage(library, local, "missing", 0, false);
    Check(first.selected == "T00" && first.rows.size() == 12 && first.pages == 3, "first page");
    Check(first.rows[0].selected && first.rows[0].label == "> [G] T00", "selected global label");
    Check(first.rows[5].label == "[S] T05", "local label");
    Check(first.pageLabel == "Page 1/3 - 27 templates", "page label");

    auto last = BuildPage(library, local, "T00", 9, false);
    Check(last.page == 2 && last.rows.size() == 3 && last.rows[2].name == "T26", "page clamps to last");
    auto followed = BuildPage(library, local, "T13", 0, true);
    Check(followed.page == 1 && followed.rows[1].selected, "follow selection");
    auto kept = BuildPage(library, local, "T13", 0, false);
    Check(kept.page == 0, "no follow keeps page");

    library.erase("T05");
    Controller c;
    c.library = library;
    Check(!c.Remove("T05") && c.Remove("T06") && !c.library.count("T06"), "remove");

    Check(Ellipsize("short", 10) == "short", "short text");
    Check(Ellipsize("abcdefghij", 8) == "abcde...", "ascii cut");
    // U+00E9 is two bytes; a cut inside it must back off to the code point start.
    const std::string accented = "ab\xC3\xA9"
                                 "cdef";
    Check(Ellipsize(accented, 6) == "ab...", "utf-8 cut");

    Check(PlanStatus(nullptr) == "No plan on this starbase", "no plan");
    Job job;
    job.reason = "Waiting for resources or construction requirements";
    Check(PlanStatus(&job).rfind("Active: Waiting", 0) == 0 && PlanStatus(&job).size() <= 46, "active status");
    job.paused = true;
    job.reason.clear();
    Check(PlanStatus(&job) == "Plan paused", "paused status");
    auto ui = BuildPage(library, local, "T00", 0, false, 23, 50);
    Check(ui.pages == 2 && ui.rows.size() == 23, "taller panel pages");
    // Vanilla short and tall side views, UI Overhaul Dynamic's, and nonsense sizes.
    const struct {
        int w, h, rows, slotRows, width, height;
    } sizes[] = {{360, 559, 10, 8, 360, 559},
                 {360, 753, 17, 14, 360, 753},
                 {450, 871, 21, 17, 450, 871},
                 {0, 0, 10, 8, 360, 559},
                 {99999, 50, 10, 8, 360, 559}};
    for (const auto& s : sizes) {
        const auto g = MakeGeometry(s.w, s.h);
        Check(g.rows == s.rows && g.slotRows == s.slotRows && g.width == s.width && g.height == s.height,
              "geometry rows");
        const auto lastSlot = PanelLayout(kPanelRow0 + g.slotRows - 1, g, true);
        Check(lastSlot.h == kPanelSlotRowHeight && lastSlot.y + lastSlot.h < PanelLayout(kPanelPrev, g).y &&
                  PanelLayout(kPanelRow0, g, true).y == PanelLayout(kPanelRow0, g).y &&
                  PanelLayout(kPanelSave, g, true).h == kPanelButtonHeight,
              "taller editor rows above pager");
        for (int i = 0; i < kPanelRow0 + g.rows; ++i) {
            const auto r = PanelLayout(i, g);
            Check(r.x >= 0 && r.w > 0 && r.x + r.w <= g.width && r.y >= kPanelTop && r.y + r.h <= g.height - 8,
                  "layout inside side view");
        }
        const auto lastRow = PanelLayout(kPanelRow0 + g.rows - 1, g);
        Check(lastRow.y + lastRow.h < PanelLayout(kPanelPrev, g).y, "rows above pager");
        Check(PanelLayout(kPanelSave, g).x + PanelLayout(kPanelSave, g).w < PanelLayout(kPanelSaveGlobal, g).x &&
                  PanelLayout(kPanelSaveGlobal, g).x + PanelLayout(kPanelSaveGlobal, g).w == g.width - kPanelMargin,
              "save buttons side by side");
        Check(PanelLayout(kPanelNew, g).y > PanelLayout(kPanelSave, g).y + kPanelButtonHeight &&
                  PanelLayout(kPanelNew, g).y + kPanelButtonHeight < PanelLayout(kPanelRow0, g).y &&
                  PanelLayout(kPanelEdit, g).x == PanelLayout(kPanelSaveGlobal, g).x,
              "new and edit row");
        Check(PanelLayout(kPanelCopyCode, g).y > PanelLayout(kPanelNew, g).y + kPanelButtonHeight &&
                  PanelLayout(kPanelCopyCode, g).y + kPanelButtonHeight < PanelLayout(kPanelRow0, g).y &&
                  PanelLayout(kPanelCopyCode, g).x == PanelLayout(kPanelNew, g).x &&
                  PanelLayout(kPanelPasteCode, g).x == PanelLayout(kPanelEdit, g).x &&
                  PanelLayout(kPanelPasteCode, g).y == PanelLayout(kPanelCopyCode, g).y,
              "share code row");
    }
    Check(MakeGeometry(450, 871).rowText > MakeGeometry(360, 559).rowText, "wider rows fit more text");
    std::puts("starbasetemplates_panel_test: passed");
}
