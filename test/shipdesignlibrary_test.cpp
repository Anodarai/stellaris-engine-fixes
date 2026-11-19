#include "../src/interface/ship_design/shipdesignlibrary.h"
#include <cstdio>
#include <cstdlib>
#include <set>
using namespace shipdesignlibrary;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "ship design library check failed: %s\n", what);
        std::exit(1);
    }
}

Design Cruiser() {
    Design d;
    d.name = "Line \"Cruiser\" Mk 2 \xC5\x81\xC3\xB3"
             "d\xC5\xBA"; // quotes, spaces and UTF-8
    d.size = "cruiser";
    d.role = "";
    d.sections = {
        {"bow",
         "CR_BOW_ARTILLERY",
         {{"LARGE_GUN_01", "LARGE_KINETIC_ARTILLERY_3"}, {"LARGE_GUN_02", "MOD_ONLY_LANCE"}}},
        {"mid", "CR_MID_HANGAR", {{"HANGAR_01", "STRIKE_CRAFT_2"}}},
        {"stern", "CR_STERN_TORPEDO", {}},
    };
    d.required = {{"power_core", "SHIP_REACTOR_FISSION"}, {"ftl_components", "SHIP_HYPER_DRIVE_2"}};
    return d;
}

int main() {
    // Escaping round-trips every byte, and empty strings.
    std::string all;
    for (int c = 1; c < 256; ++c)
        all += static_cast<char>(c);
    std::string back;
    Check(Unescape(Escape(all), back) && back == all, "escape round trip");
    Check(Escape("") == "%" && Unescape("%", back) && back.empty(), "empty token");
    Check(Escape("a b").find(' ') == std::string::npos, "no spaces in tokens");
    Check(!Unescape("%4", back) && !Unescape("%G0", back) && !Unescape("ab%", back), "bad escapes refused");

    // Library round trip.
    Library library;
    auto d = Cruiser();
    library[d.name] = d;
    Design small{"Picket", "corvette", "picket", {{"mid", "CO_MID_S2", {{"SMALL_GUN_01", "SMALL_LASER_1"}}}}, {}};
    library[small.name] = small;
    const auto text = Serialize(library);
    Library read;
    Check(Deserialize(text, read) && read.size() == 2, "deserialize");
    const auto& r = read.at(d.name);
    Check(r.size == "cruiser" && r.role.empty() && r.sections.size() == 3 && r.required.size() == 2, "fields");
    Check(r.sections[0].components[1].component == "MOD_ONLY_LANCE" && r.sections[2].components.empty(), "components");
    Check(read.at("Picket").role == "picket", "role kept");
    Check(Serialize(read) == text, "stable serialization");

    // Malformed input leaves the library untouched.
    Library keep = read;
    for (const std::string& bad :
         {std::string(""), std::string("SEF_SHIPDESIGNS_2 0"), text + "junk", text.substr(0, text.size() / 2),
          std::string("SEF_SHIPDESIGNS_1 1\ndesign % cruiser % 0 0\n"),
          std::string("SEF_SHIPDESIGNS_1 1\ndesign A cruiser % 999 0\n"),
          std::string("SEF_SHIPDESIGNS_1 2\ndesign A x % 0 0\ndesign A x % 0 0\n"),
          std::string("SEF_SHIPDESIGNS_1 -1\n")}) {
        Check(!Deserialize(bad, keep) && keep.size() == 2, "malformed refused");
    }
    Library empty;
    Check(Deserialize("SEF_SHIPDESIGNS_1 0\n", empty) && empty.empty(), "empty library");

    // Names.
    Check(ValidName("A") && !ValidName("") && !ValidName("   ") && !ValidName("a\nb") &&
              !ValidName(std::string(129, 'x')) && ValidName(std::string(128, 'x')),
          "name rules");

    // Loading: missing and locked parts are skipped and reported; a missing section takes its
    // components with it; everything else is placed.
    std::set<std::string> gone = {"CR_MID_HANGAR", "MOD_ONLY_LANCE", "SHIP_HYPER_DRIVE_2"};
    std::vector<std::string> placed;
    Placer placer;
    placer.section = [&](const Section& s) {
        if (gone.count(s.section))
            return Result{Placed::Missing, ""};
        placed.push_back(s.section);
        return Result{};
    };
    placer.component = [&](const Section&, const Component& c) {
        if (gone.count(c.component))
            return Result{Placed::Missing, ""};
        if (c.component == "LARGE_KINETIC_ARTILLERY_3")
            return Result{Placed::Locked, "Requires Mass Accelerators"};
        placed.push_back(c.component);
        return Result{};
    };
    placer.required = [&](const Required& q) {
        if (gone.count(q.component))
            return Result{Placed::Missing, ""};
        placed.push_back(q.component);
        return Result{};
    };
    const auto report = Apply(d, placer);
    Check(report.placed == 3 && report.skipped == 5, "placed and skipped counts");
    Check(placed == std::vector<std::string>({"CR_BOW_ARTILLERY", "CR_STERN_TORPEDO", "SHIP_REACTOR_FISSION"}),
          "placed parts");
    Check(report.problems.size() == 4, "one problem per refusal");
    Check(report.problems[0].find("Requires Mass Accelerators") != std::string::npos, "the game's reason is shown");
    Check(report.problems[1].find("MOD_ONLY_LANCE no longer exists") != std::string::npos, "missing component");
    Check(report.problems[2].find("Section CR_MID_HANGAR") != std::string::npos &&
              report.problems[2].find("1 components were skipped") != std::string::npos,
          "missing section");
    const auto message = ReportText(d.name, report, 2);
    Check(message.find("5 parts could not be added") != std::string::npos &&
              message.find("and 2 more") != std::string::npos,
          "message");

    // Unfit and missing-slot results.
    Check(Describe({Placed::NoSlot, ""}, "X", "s").find("no slot s") != std::string::npos, "no slot");
    Check(Describe({Placed::Unfit, ""}, "X", "s").find("does not fit") != std::string::npos, "unfit");

    // Paging: sorted, the selection followed, hull size shown.
    Library many;
    for (int i = 0; i < 25; ++i) {
        Design x{"Design " + std::string(i < 10 ? "0" : "") + std::to_string(i), "destroyer", "", {}, {}};
        many[x.name] = x;
    }
    auto page = BuildPage(many, "Design 13", 0, true, 10, 38);
    Check(page.page == 1 && page.pages == 3 && page.rows.size() == 10 && page.selected == "Design 13",
          "follow selection");
    Check(page.rows[3].selected && page.rows[3].label == "> [destroyer] Design 13", "selected row label");
    page = BuildPage(many, "missing", 7, false, 10, 38);
    Check(page.page == 2 && page.rows.size() == 5 && page.selected == "Design 00", "clamped page, default selection");
    Check(BuildPage(Library{}, "", 0, true, 10, 38).pageLabel == "No designs saved", "empty page");
    Check(Ellipsize("abcdefgh", 6) == "abc..." && Ellipsize("\xC5\x81\xC5\x81\xC5\x81", 5).size() <= 5, "ellipsize");

    // Layout: rows between the pager and Delete/Close, everything below the toggle.
    const auto g = MakeGeometry(445, 0, 642);
    Check(g.rows >= 4 && g.rows <= kMaxRows, "row count");
    const auto status = Layout(kStatus, g);
    Check(status.y + status.h <= 642 - 8, "fits the vanilla designer");
    Check(Layout(kRow0, g).y > Layout(kPrev, g).y && Layout(kDelete, g).y > Layout(kRow0 + g.rows - 1, g).y, "order");
    Check(MakeGeometry(445, 0, 100000).rows == MakeGeometry(445, 0, 642).rows, "odd heights fall back");
    std::puts("ship design library tests passed");
    return 0;
}
