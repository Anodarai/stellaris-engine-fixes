// Template share codes (starbasetemplates_share.h): round trips, shorthand and full keys, damage.
#include <cstdio>
#include <cstdlib>
#include <set>
#include "../src/interface/starbase/starbasetemplates_share.h"

using namespace starbasetemplates;

static void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("starbasetemplates_share_test: FAILED %s\n", what);
        std::exit(1);
    }
}
static bool Same(const Layout& a, const Layout& b) {
    return a.level == b.level && a.modules == b.modules && a.buildings == b.buildings;
}

int main() {
    // The tables are the format: their size and order must not change, only grow at the end.
    // A pinned fingerprint of every key in order catches an edit that moves or drops one.
    std::string all;
    for (const auto& t : kShareTables) {
        std::set<std::string> seen;
        for (size_t i = 0; i < t.count; ++i) {
            Check(seen.insert(t.keys[i]).second, "table entries are unique");
            all += t.keys[i];
            all += '\n';
        }
        Check(t.count < 36 * 36, "two base-36 digits address every entry");
    }
    Check(kShareTables[kShareLevel].count == 41 && kShareTables[kShareModule].count == 130 &&
              kShareTables[kShareBuilding].count == 117,
          "table sizes (append new keys at the end and update these)");
    Check(ShareCheck(all) == "f151", "table fingerprint (never reorder or remove a key)");
    Check(std::string(kShareModules[0]) == "shipyard" && std::string(kShareBuildings[1]) == "crew_quarters" &&
              std::string(kShareLevels[4]) == "starbase_level_citadel",
          "common entries come first");

    // A vanilla citadel: every key is short.
    Layout citadel{"starbase_level_citadel",
                   {"shipyard", "shipyard", "anchorage", "", "starbase_hangar_bay"},
                   {"crew_quarters", "naval_logistics_office"}};
    const auto code = EncodeShare(citadel, "Defense hub");
    std::printf("vanilla: %s\n", code.c_str());
    Check(code.rfind("SB1:4:0,0,1,,", 0) == 0, "shorthand for base game keys");
    Check(code.find(":Defense+hub:") != std::string::npos, "name with a space");
    SharedTemplate back;
    std::string error;
    Check(DecodeShare(code, back, error) && Same(back.layout, citadel) && back.name == "Defense hub", "round trip");

    // A mod's keys are written in full; odd bytes are escaped; trailing empties are dropped.
    Layout modded{"giga_level_x", {"giga_ring_module", "", "shipyard", "", ""}, {"we:ird key%"}};
    const auto modCode = EncodeShare(modded, "\xC3\x9C" "n\xC3\xAF" "code name: 100%");
    std::printf("modded:  %s\n", modCode.c_str());
    Check(modCode.find(".giga_level_x") != std::string::npos && modCode.find(".giga_ring_module,,0") != std::string::npos,
          "full ids for mod keys");
    Check(modCode.find(' ') == std::string::npos, "no spaces in a code");
    Check(DecodeShare(modCode, back, error), "modded decodes");
    Check(back.layout.level == "giga_level_x" && back.layout.modules.size() == 3 &&
              back.layout.modules[0] == "giga_ring_module" && back.layout.modules[1].empty() &&
              back.layout.buildings == std::vector<std::string>{"we:ird key%"} && back.name == "\xC3\x9C" "n\xC3\xAF" "code name: 100%",
          "modded round trip");

    // Pasted from chat: wrapped, padded, lower-case prefix, upper-case check.
    std::string pasted = "  " + code.substr(0, 10) + "\r\n " + code.substr(10) + "\n";
    Check(DecodeShare(pasted, back, error) && Same(back.layout, citadel), "whitespace is ignored");
    std::string lowered = code;
    lowered[0] = 's';
    lowered[1] = 'b';
    for (size_t i = lowered.size() - 4; i < lowered.size(); ++i)
        lowered[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(lowered[i])));
    Check(DecodeShare(lowered, back, error), "prefix and check in any case");

    // No modules or buildings at all.
    Layout outpost{"starbase_level_outpost", {}, {}};
    const auto empty = EncodeShare(outpost, "");
    Check(empty == "SB1:0:::" + empty.substr(8) && DecodeShare(empty, back, error) && Same(back.layout, outpost) &&
              back.name.empty(),
          "empty lists and name");

    // Refused: damage, truncation, other formats, out-of-range indices, bad escapes.
    auto refuse = [&](const std::string& text, const char* what) {
        SharedTemplate t;
        std::string e;
        Check(!DecodeShare(text, t, e) && !e.empty(), what);
    };
    std::string typo = code;
    typo[7] = typo[7] == '0' ? '1' : '0';
    refuse(typo, "a changed character fails the check");
    refuse(code.substr(0, code.size() - 3), "a truncated code");
    refuse(code.substr(0, code.rfind(':')), "a code without its check");
    refuse("hello", "plain text");
    refuse("SB2:4::::" + ShareCheck("SB2:4:::"), "another version");
    auto withCheck = [](const std::string& body) { return body + ":" + ShareCheck(body); };
    Check(DecodeShare(withCheck("SB1:4:0:1:x"), back, error), "hand-made code");
    refuse(withCheck("SB1:zz:0:1:x"), "level index past the table");
    refuse(withCheck("SB1:4:abc:1:x"), "three-digit index");
    refuse(withCheck("SB1:4:.:1:x"), "empty full key");
    refuse(withCheck("SB1:4:.a%G0:1:x"), "bad escape");
    refuse(withCheck("SB1:4:.a%4:1:x"), "short escape");
    refuse(withCheck("SB1::0:1:x"), "no level");
    refuse(withCheck("SB1:4:0:1:x:y"), "too many fields");
    std::string many = "SB1:4:0";
    for (int i = 0; i < 32; ++i)
        many += ",0";
    refuse(withCheck(many + ":1:x"), "too many slots");
    refuse(std::string(kShareMaxText + 1, 'S'), "too long");
    Check(EncodeShare(Layout{}, "x").empty(), "no level, no code");
    Check(EncodeShare(citadel, std::string(kShareMaxName + 1, 'n')).empty(), "name too long");

    std::puts("starbasetemplates_share_test: passed");
    return 0;
}
