#include "../src/interface/starbase/starbasetemplates_editor.h"
#include <cstdio>
#include <cstdlib>
using namespace starbasetemplates;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "starbase editor check failed: %s\n", what);
        std::exit(1);
    }
}
int main() {
    Check(Prettify("trading_hub") == "Trading hub", "prettify");
    Check(LevelName("starbase_level_star_fortress") == "Star fortress", "level name");
    Check(LevelName("modded_level") == "Modded level", "level without the prefix");

    // The vanilla chain, with its listed slots.
    std::vector<LevelInfo> levels{{"starbase_level_outpost", 0, 0},
                                  {"starbase_level_starport", 2, 1},
                                  {"starbase_level_starhold", 4, 2},
                                  {"starbase_level_starfortress", 6, 3},
                                  {"starbase_level_citadel", 6, 4}};
    std::vector<Choice> modules{{"shipyard", "Shipyard", kTypeAll},
                                {"anchorage", "", kTypeAll},
                                {"trading_hub", "Trade Hub", kTypeStarbase},
                                {"orbital_ring_habitation", "Habitation Module", kTypeOrbitalRing}};
    std::vector<Choice> buildings{{"crew_quarters", "Crew Quarters", kTypeAll},
                                  {"ring_minerals_hub", "Mineral Hub", kTypeOrbitalRing}};

    auto draft = NewDraft(levels);
    Check(draft.layout.level == "starbase_level_citadel" && draft.name.empty(), "a new draft plans the last level");
    Check(SlotCount(draft, Kind::Module, levels) == 6 && SlotCount(draft, Kind::Building, levels) == 4,
          "citadel slots");
    Check(DraftProblem(draft) == nullptr, "an empty citadel can be saved");
    Check(DraftProblem(Draft{}) != nullptr, "a draft needs a level");

    // A vanilla citadel is exactly one page of the short panel: the level, 6 modules, 4 buildings.
    auto view = BuildEditor(draft, levels, modules, buildings, 0, 11);
    Check(view.rows.size() == 11 && view.pages == 1, "citadel rows");
    Check(view.rows[0].kind == EditorRowKind::Level && view.rows[0].label == "Level: Citadel", "level row");
    Check(view.rows[1].kind == EditorRowKind::Module && view.rows[1].slot == 0 && view.rows[1].label == "M1: -",
          "first module row");
    Check(view.rows[7].kind == EditorRowKind::Building && view.rows[7].slot == 0 && view.rows[7].label == "B1: -",
          "first building row");
    Check(view.pageLabel == "6 modules, 4 buildings", "editor page label");

    Check(SetSlot(draft, Kind::Module, 0, "shipyard") && SetSlot(draft, Kind::Module, 2, "anchorage"), "set modules");
    Check(SetSlot(draft, Kind::Building, 1, "gone_mod_building"), "set building");
    Check(draft.layout.modules == std::vector<std::string>({"shipyard", "", "anchorage"}), "gaps are kept");
    view = BuildEditor(draft, levels, modules, buildings, 0, 11);
    Check(view.rows[1].label == "M1: Shipyard" && view.rows[2].label == "M2: -", "names from the game");
    Check(view.rows[3].label == "M3: Anchorage", "an unnamed definition shows its key");
    Check(view.rows[8].label == "B2: Gone mod building (missing)", "a key without a definition");
    Check(SetSlot(draft, Kind::Module, 2, "") && draft.layout.modules.size() == 1, "emptying the last slot trims");
    Check(SetSlot(draft, Kind::Module, 9, "") && draft.layout.modules.size() == 1, "emptying an empty slot");
    Check(!SetSlot(draft, Kind::Module, kEditorMaxSlots, "shipyard") && !SetSlot(draft, Kind::Upgrade, 0, "x"),
          "slot limits");

    // Mods: more slots than the level lists. Asked for with "+ slot", marked with '+'.
    Check(AddSlot(draft, Kind::Module, levels) && SlotCount(draft, Kind::Module, levels) == 7, "one more module slot");
    view = BuildEditor(draft, levels, modules, buildings, 0, 11);
    Check(view.pages == 2 && view.pageLabel == "7 modules, 4 buildings - 1/2", "a second page");
    Check(view.rows[7].label == "M7+: -", "extra slot mark");
    Check(SetSlot(draft, Kind::Module, 6, "trading_hub"), "fill the extra slot");
    // Lowering the level keeps what is filled, drops what was only asked for.
    Check(AddSlot(draft, Kind::Building, levels), "one more building slot");
    Check(StepLevel(draft, levels, -1) && draft.layout.level == "starbase_level_starfortress", "level down");
    Check(SlotCount(draft, Kind::Module, levels) == 7 && SlotCount(draft, Kind::Building, levels) == 3,
          "slots after level down");
    Check(StepLevel(draft, levels, 1) && !StepLevel(draft, levels, 1), "the chain's end");
    for (int i = 0; i < 8; ++i)
        StepLevel(draft, levels, -1);
    Check(draft.layout.level == "starbase_level_outpost" && !StepLevel(draft, levels, -1), "the chain's start");
    view = BuildEditor(draft, levels, modules, buildings, 0, 11);
    Check(view.rows[1].label == "M1+: Shipyard", "an outpost lists no slots");

    // Capacity a real starbase showed raises its level's count; the rest keep theirs.
    CapacityMemory memory;
    memory.Observe("starbase_level_citadel", 8, 5);
    memory.Observe("starbase_level_citadel", 7, 6);
    memory.Observe("starbase_level_starport", 1, 0);
    memory.Observe("starbase_level_starhold", 999, 2);
    auto raised = levels;
    memory.Apply(raised);
    Check(raised[4].modules == 8 && raised[4].buildings == 6, "observed capacity");
    Check(raised[1].modules == 2 && raised[2].modules == 4, "capacity never lowers and ignores nonsense");
    auto modded = NewDraft(raised);
    Check(SlotCount(modded, Kind::Module, raised) == 8, "a modded citadel");
    int added = 0;
    while (AddSlot(modded, Kind::Module, raised))
        ++added;
    Check(added == kEditorMaxSlots - 8 && SlotCount(modded, Kind::Module, raised) == kEditorMaxSlots, "slot cap");

    // A stored template on a level this game doesn't have (a removed mod).
    auto old = EditDraft("Old", true, Layout{"starbase_level_gone", {"shipyard", "", ""}, {}});
    Check(old.name == "Old" && old.global && old.layout.modules.size() == 1, "edit trims trailing empties");
    view = BuildEditor(old, levels, modules, buildings, 0, 11);
    Check(view.rows[0].label == "Level: Gone (not in this game)" && view.rows.size() == 2, "unknown level");
    Check(StepLevel(old, levels, 1) && old.layout.level == "starbase_level_citadel", "stepping off an unknown level");
    const auto stored = DraftLayout(old);
    Check(stored.level == "starbase_level_citadel" && stored.modules == std::vector<std::string>({"shipyard"}),
          "stored layout");
    Check(LayoutSummary(stored) == "Citadel: 1 module, 0 buildings", "summary");

    // The picker: nothing first, then by name; other station kinds hidden unless asked for.
    auto picker = BuildPicker(modules, kStationStarbase, "", 0, true, 11);
    Check(picker.rows.size() == 4 && picker.hidden == 1 && picker.shown == 3, "picker filter");
    Check(picker.rows[0].key.empty() && picker.rows[0].label == "> (empty)", "empty entry is the current one");
    Check(picker.rows[1].key == "anchorage" && picker.rows[2].key == "shipyard" && picker.rows[3].key == "trading_hub",
          "sorted by name");
    Check(picker.pageLabel == "3 listed", "picker label");
    picker = BuildPicker(modules, -1, "shipyard", 0, true, 11);
    Check(picker.rows.size() == 5 && picker.hidden == 0 && picker.rows[3].selected &&
              picker.rows[3].label == "> Shipyard",
          "all kinds");
    picker = BuildPicker(modules, kStationStarbase, "orbital_ring_habitation", 0, true, 11);
    Check(picker.rows.size() == 5 && picker.hidden == 0, "the current entry is never hidden");
    picker = BuildPicker(modules, kStationStarbase, "gone_module", 0, true, 11);
    Check(picker.rows.size() == 5 && picker.rows[2].key == "gone_module" &&
              picker.rows[2].label == "> Gone module (missing)",
          "missing current entry stays");
    std::vector<Choice> many;
    for (int i = 0; i < 30; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "m%02d", i);
        many.push_back({key, "", kTypeAll});
    }
    picker = BuildPicker(many, kStationStarbase, "m25", 0, true, 11);
    Check(picker.pages == 3 && picker.page == 2 && picker.rows[4].selected, "picker follows the current entry");
    picker = BuildPicker(many, kStationStarbase, "m25", 7, false, 11);
    Check(picker.page == 2 && picker.pageLabel == "30 listed - 3/3", "picker page clamps");

    // Each kind lists its own components; "all" ones too, except for waystations.
    const Choice all{"shipyard", "", kTypeAll}, star{"trading_hub", "", kTypeStarbase},
        ringOnly{"ring_x", "", kTypeOrbitalRing}, way{"waystation_energy_module", "", kTypeWaystation},
        ark{"arkship_armor", "", kTypeArkship}, unknown{"mod_x", "", -1};
    Check(ChoiceFits(all, kStationStarbase) && ChoiceFits(star, kStationStarbase) &&
              !ChoiceFits(ringOnly, kStationStarbase) && !ChoiceFits(way, kStationStarbase) &&
              !ChoiceFits(ark, kStationStarbase),
          "starbase choices");
    Check(ChoiceFits(all, kStationOrbitalRing) && ChoiceFits(ringOnly, kStationOrbitalRing) &&
              !ChoiceFits(star, kStationOrbitalRing),
          "orbital ring choices");
    Check(ChoiceFits(all, kStationCitadel) && ChoiceFits(star, kStationCitadel) && !ChoiceFits(way, kStationCitadel),
          "citadel choices");
    Check(ChoiceFits(way, kStationWaystation) && !ChoiceFits(all, kStationWaystation) &&
              !ChoiceFits(star, kStationWaystation),
          "waystation choices");
    for (int kind = -1; kind < kStationKinds; ++kind)
        Check(ChoiceFits(unknown, kind) && (kind >= 0 || ChoiceFits(ark, kind)), "unknown type or no kind");
    picker = BuildPicker(modules, kStationOrbitalRing, "", 0, true, 11);
    Check(picker.shown == 3 && picker.hidden == 1 && picker.rows[2].key == "orbital_ring_habitation",
          "orbital ring picker");
    picker = BuildPicker(modules, kStationWaystation, "", 0, true, 11);
    Check(picker.shown == 0 && picker.hidden == 4 && picker.rows.size() == 1, "waystation picker");

    // Kinds: a template belongs to the chain that has its level.
    KindChains chains;
    chains[kStationStarbase] = {"starbase_level_outpost", "starbase_level_starport", "starbase_level_citadel",
                                "mod_level_6"};
    chains[kStationOrbitalRing] = {"starbase_level_orbital_ring_tier_1", "starbase_level_orbital_ring_tier_2"};
    chains[kStationCitadel] = {"starbase_level_deep_space_citadel_1"};
    chains[kStationWaystation] = {"starbase_level_waystation_1"};
    Check(KindOfLevel(chains, "mod_level_6") == kStationStarbase &&
              KindOfLevel(chains, "starbase_level_orbital_ring_tier_2") == kStationOrbitalRing,
          "kind of level");
    Check(KindOfLevel(chains, "starbase_level_deep_space_citadel_1") == kStationCitadel &&
              KindOfLevel(chains, "starbase_level_waystation_1") == kStationWaystation,
          "citadel and waystation levels");
    Check(KindOfLevel(chains, "starbase_level_gone") == -1 && KindOfLevel(chains, "") == -1 &&
              KindOfLevel(KindChains{}, "starbase_level_outpost") == -1,
          "no kind");
    const std::map<std::string, Layout> library{{"Bastion", {"starbase_level_citadel", {}, {}}},
                                                {"Ring", {"starbase_level_orbital_ring_tier_2", {}, {}}},
                                                {"Way", {"starbase_level_waystation_1", {}, {}}},
                                                {"Old", {"starbase_level_gone", {}, {}}}};
    auto listed = KindLibrary(library, chains, kStationStarbase);
    Check(listed.size() == 2 && listed.count("Bastion") && listed.count("Old"),
          "starbase templates, and those of no kind");
    listed = KindLibrary(library, chains, kStationWaystation);
    Check(listed.size() == 2 && listed.count("Way") && listed.count("Old"), "waystation templates");
    Check(KindLibrary(library, chains, kStationCitadel).size() == 1 &&
              KindLibrary(library, KindChains{}, kStationCitadel).size() == 4,
          "citadel templates; unread chains list all");
    for (int kind = 0; kind < kStationKinds; ++kind)
        Check(kStationKindInfo[kind].name[0] && kStationKindInfo[kind].title[0] && kStationKindInfo[kind].firstLevel[0],
              "kind info");

    // starbase_type's values are learned from vanilla definitions, whatever the numbering.
    const std::vector<std::string> keys{
        "shipyard", "anchorage", "orbital_ring_habitation", "waystation_energy_module", "arkship_armor", "mod_module"};
    auto map = LearnTypes(keys, {4, 4, 1, 2, 3, 0});
    Check(map.trusted && map.Of(4) == kTypeAll && map.Of(1) == kTypeOrbitalRing && map.Of(2) == kTypeWaystation &&
              map.Of(3) == kTypeArkship && map.Of(0) == kTypeStarbase && map.Of(5) == -1,
          "the expected numbering");
    Check(map.note == "starbase=0 orbital_ring=1 waystation=2 arkship=3 all=4", "learned values in the log");
    map = LearnTypes(keys, {4, 4, 0, 3, 1, 2});
    Check(map.trusted && map.Of(0) == kTypeOrbitalRing && map.Of(3) == kTypeWaystation && map.Of(1) == kTypeArkship &&
              map.Of(2) == kTypeStarbase && map.Of(4) == kTypeAll,
          "another numbering");
    map = LearnTypes({"shipyard", "orbital_ring_habitation", "mod_a"}, {4, 1, 0});
    Check(map.trusted && map.Of(4) == kTypeAll && map.Of(1) == kTypeOrbitalRing && map.Of(0) == -1 && map.Of(2) == -1,
          "starbase not inferred without all three");
    // A mod made the orbital ring habitation "all": the shipyard keeps that value, the ring hub teaches orbital ring.
    map = LearnTypes({"shipyard", "orbital_ring_habitation", "ring_minerals_hub"}, {4, 4, 1});
    Check(map.trusted && map.Of(4) == kTypeAll && map.Of(1) == kTypeOrbitalRing, "a changed vanilla definition");
    Check(!LearnTypes(keys, {4, 4, 1, 2, 3, 77}).trusted && !LearnTypes(keys, {4, 4, 1, 2, 3, -1}).trusted,
          "a value outside the enum");
    Check(!LearnTypes({"mod_a", "mod_b"}, {5, 0}).trusted && !LearnTypes({}, {}).trusted &&
              !LearnTypes({"shipyard"}, {4}).trusted,
          "too few examples");
    Check(!LearnTypes(keys, {4, 4, 1}).trusted && LearnTypes(keys, {4, 4, 1}).Of(4) == -1, "sizes differ");

    // Same name, different definitions: told apart by kind, or by key when kinds don't.
    const std::vector<Choice> twins{{"starbase_battery", "Battery", kTypeAll},
                                    {"orbital_ring_battery", "Battery", kTypeOrbitalRing},
                                    {"arkship_battery", "Battery", kTypeArkship},
                                    {"hangar_a", "Hangar", -1},
                                    {"hangar_b", "Hangar", -1},
                                    {"solo", "Solo", kTypeAll}};
    picker = BuildPicker(twins, -1, "", 0, false, 11, 60);
    Check(picker.rows.size() == 7 && picker.rows[1].label == "Battery (arkship)" &&
              picker.rows[2].label == "Battery (orbital ring)" && picker.rows[3].label == "Battery (any station)",
          "twins by kind");
    Check(picker.rows[4].label == "Hangar (hangar_a)" && picker.rows[5].label == "Hangar (hangar_b)" &&
              picker.rows[6].label == "Solo",
          "twins by key");
    picker = BuildPicker(twins, kStationOrbitalRing, "", 0, false, 11, 60);
    Check(picker.rows[1].label == "Battery (orbital ring)" && picker.rows[2].label == "Battery (any station)",
          "twins left after the filter");
    picker = BuildPicker(twins, kStationStarbase, "", 0, false, 11, 60);
    Check(picker.rows[1].label == "Battery", "no twin left: the plain name");

    // A share code opens as a new draft of its level's kind, suggesting the code's name.
    KindChains shareChains;
    shareChains[kStationStarbase] = {"starbase_level_outpost", "starbase_level_citadel"};
    shareChains[kStationOrbitalRing] = {"starbase_level_orbital_ring_tier_1"};
    SharedTemplate shared{"Ring plan", {"starbase_level_orbital_ring_tier_1", {"orbital_ring_habitation", ""}, {}}};
    auto imported = ImportDraft(shared, shareChains, kStationStarbase);
    Check(imported.kind == kStationOrbitalRing && imported.name.empty() && imported.suggested == "Ring plan" &&
              imported.layout.modules.size() == 1 && DraftShareName(imported) == "Ring plan",
          "imported draft");
    shared.layout.level = "gone_mod_level";
    Check(ImportDraft(shared, shareChains, kStationWaystation).kind == kStationWaystation, "unknown level kind");
    imported = EditDraft("Saved", true, Layout{"starbase_level_citadel", {}, {}});
    imported.suggested = "ignored";
    Check(DraftShareName(imported) == "Saved", "an edited entry keeps its name");
    const Layout partly{"starbase_level_citadel", {"shipyard", "", "gone_module"}, {"crew_quarters", "gone_building"}};
    Check(MissingParts(partly, levels, modules, buildings) == 2, "missing parts");
    Check(MissingParts(Layout{"gone_level", {}, {}}, levels, modules, buildings) == 1, "missing level");
    std::puts("starbasetemplates_editor_test: passed");
}
