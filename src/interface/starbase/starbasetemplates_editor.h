// Pure model of the template editor: a draft layout whose level, modules and buildings are set
// directly, instead of copied from a built starbase. The DLL feeds it the game's own level chain
// and component definitions, so modded levels, slot counts and components appear as loaded.
// Nothing here touches game memory.
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <string>
#include <vector>
#include "starbasetemplates_panel.h"
#include "starbasetemplates_share.h"

namespace starbasetemplates {
inline constexpr int kEditorMaxSlots = 32;

// One level of the normal starbase chain, first (outpost) to last, with the slots a starbase of
// that level has: the level's own count, raised to what a real starbase of that level showed.
struct LevelInfo {
    std::string key;
    int modules = 0, buildings = 0;
};
// A module or building definition. `type` is its starbase_type (kType...), or -1 when unknown.
struct Choice {
    std::string key, name;
    int type = -1;
};

// The station kinds that have templates. Each is a chain of levels from `firstLevel`; a mod's
// further levels belong to the chain they are reached from.
enum StationKind { kStationStarbase, kStationOrbitalRing, kStationCitadel, kStationWaystation, kStationKinds };
// starbase_type values (EBuildingStarbaseType): what a definition says it is for. The game uses
// it only for technology unlock text; `potential` does the real restricting. Most vanilla
// definitions for normal starbases don't set it and are "all".
enum ComponentType { kTypeStarbase, kTypeOrbitalRing, kTypeWaystation, kTypeArkship, kTypeAll, kTypeOther };
struct StationKindInfo {
    const char* name;  // "Starbase"
    const char* title; // the panel's title for its templates
    const char* firstLevel;
    int type;      // the starbase_type its own components carry
    bool untagged; // whether "all" components are listed for it too
};
// Deep space citadels take normal starbase buildings. Waystations have their own modules, all
// tagged, and no buildings.
inline constexpr StationKindInfo kStationKindInfo[kStationKinds] = {
    {"Starbase", "Starbase templates", "starbase_level_outpost", kTypeStarbase, true},
    {"Orbital ring", "Orbital ring templates", "starbase_level_orbital_ring_tier_1", kTypeOrbitalRing, true},
    {"Deep space citadel", "Citadel templates", "starbase_level_deep_space_citadel_1", kTypeStarbase, true},
    {"Waystation", "Waystation templates", "starbase_level_waystation_1", kTypeWaystation, false}};
// Each kind's level keys, first to last, as the game has them loaded.
using KindChains = std::array<std::vector<std::string>, kStationKinds>;
// The kind whose chain has the level, or -1 (another station type, or a removed mod's level).
inline int KindOfLevel(const KindChains& chains, const std::string& level) {
    for (int kind = 0; kind < kStationKinds; ++kind)
        if (std::find(chains[kind].begin(), chains[kind].end(), level) != chains[kind].end())
            return kind;
    return -1;
}
// The templates listed for a kind: its own, and those whose level no chain has, so that they
// can still be looked at and deleted.
inline std::map<std::string, Layout> KindLibrary(const std::map<std::string, Layout>& library, const KindChains& chains,
                                                 int kind) {
    std::map<std::string, Layout> result;
    for (const auto& entry : library) {
        const int own = KindOfLevel(chains, entry.second.level);
        if (own < 0 || own == kind)
            result.insert(entry);
    }
    return result;
}
// Whether a definition is listed for a kind; kind -1 lists everything.
inline bool ChoiceFits(const Choice& c, int kind) {
    if (kind < 0 || kind >= kStationKinds || c.type < 0)
        return true;
    return c.type == kStationKindInfo[kind].type || (c.type == kTypeAll && kStationKindInfo[kind].untagged);
}

struct Draft {
    int kind = kStationStarbase; // whose levels and components it is made of
    std::string name;            // library entry being edited; empty for a new template
    std::string suggested;       // a new template's name to start the Save dialog with
    bool global = false;         // scope of that entry
    Layout layout;
    int moduleSlots = 0, buildingSlots = 0; // slots asked for with "+ slot", beyond the level's
};

// "starbase_level_star_fortress" -> "Star fortress"; "trading_hub" -> "Trading hub".
inline std::string Prettify(std::string key, const char* prefix = "") {
    const std::string p(prefix);
    if (!p.empty() && key.size() > p.size() && key.compare(0, p.size(), p) == 0)
        key.erase(0, p.size());
    std::replace(key.begin(), key.end(), '_', ' ');
    if (!key.empty())
        key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    return key;
}
inline std::string LevelName(const std::string& key) {
    return Prettify(key, "starbase_level_");
}

inline const Choice* FindChoice(const std::vector<Choice>& choices, const std::string& key) {
    for (const auto& c : choices)
        if (c.key == key)
            return &c;
    return nullptr;
}
// The game's name for a component, or its key made readable when the definition is gone.
inline std::string ChoiceName(const std::vector<Choice>& choices, const std::string& key) {
    if (key.empty())
        return "-";
    const auto* c = FindChoice(choices, key);
    if (!c)
        return Prettify(key) + " (missing)";
    return c->name.empty() ? Prettify(key) : c->name;
}

inline int LevelIndex(const std::vector<LevelInfo>& levels, const std::string& key) {
    for (size_t i = 0; i < levels.size(); ++i)
        if (levels[i].key == key)
            return static_cast<int>(i);
    return -1;
}

// Slots a real starbase showed, by level key. Capacity is a modifier, so a mod or a technology
// can give a starbase more slots than its level's definition lists.
struct CapacityMemory {
    std::map<std::string, std::pair<int, int>> seen;
    void Observe(const std::string& level, int modules, int buildings) {
        if (level.empty() || modules < 0 || buildings < 0 || modules > kEditorMaxSlots || buildings > kEditorMaxSlots)
            return;
        auto& entry = seen[level];
        entry.first = (std::max)(entry.first, modules);
        entry.second = (std::max)(entry.second, buildings);
    }
    void Apply(std::vector<LevelInfo>& levels) const {
        for (auto& level : levels) {
            const auto it = seen.find(level.key);
            if (it == seen.end())
                continue;
            level.modules = (std::max)(level.modules, it->second.first);
            level.buildings = (std::max)(level.buildings, it->second.second);
        }
    }
};

inline int FilledSlots(const std::vector<std::string>& slots) {
    int n = static_cast<int>(slots.size());
    while (n > 0 && slots[n - 1].empty())
        --n;
    return n;
}
// Slots of the draft's level, by its definition and what was seen; 0 for an unknown level.
inline int LevelSlots(const Draft& d, Kind kind, const std::vector<LevelInfo>& levels) {
    const int index = LevelIndex(levels, d.layout.level);
    if (index < 0)
        return 0;
    return std::clamp(kind == Kind::Module ? levels[index].modules : levels[index].buildings, 0, kEditorMaxSlots);
}
// Slots the editor shows: the level's, any asked for, and never fewer than are filled.
inline int SlotCount(const Draft& d, Kind kind, const std::vector<LevelInfo>& levels) {
    const auto& slots = kind == Kind::Module ? d.layout.modules : d.layout.buildings;
    const int asked = kind == Kind::Module ? d.moduleSlots : d.buildingSlots;
    return std::clamp((std::max)({LevelSlots(d, kind, levels), asked, FilledSlots(slots)}), 0, kEditorMaxSlots);
}
inline bool AddSlot(Draft& d, Kind kind, const std::vector<LevelInfo>& levels) {
    const int count = SlotCount(d, kind, levels);
    if (count >= kEditorMaxSlots)
        return false;
    (kind == Kind::Module ? d.moduleSlots : d.buildingSlots) = count + 1;
    return true;
}
inline bool SetSlot(Draft& d, Kind kind, int slot, const std::string& key) {
    if (kind == Kind::Upgrade || slot < 0 || slot >= kEditorMaxSlots || key.size() > 256)
        return false;
    auto& slots = kind == Kind::Module ? d.layout.modules : d.layout.buildings;
    if (static_cast<int>(slots.size()) <= slot) {
        if (key.empty())
            return true;
        slots.resize(static_cast<size_t>(slot) + 1);
    }
    slots[slot] = key;
    slots.resize(static_cast<size_t>(FilledSlots(slots)));
    return true;
}
// Moves the level along the chain, stopping at its ends. Slots asked for belong to the old
// level and are dropped; filled ones stay, shown as extra when the new level has fewer.
inline bool StepLevel(Draft& d, const std::vector<LevelInfo>& levels, int step) {
    if (levels.empty())
        return false;
    const int index = LevelIndex(levels, d.layout.level);
    const int last = static_cast<int>(levels.size()) - 1;
    const int next = index < 0 ? (step > 0 ? last : 0) : std::clamp(index + step, 0, last);
    if (next == index)
        return false;
    d.layout.level = levels[next].key;
    d.moduleSlots = d.buildingSlots = 0;
    return true;
}
inline Draft NewDraft(const std::vector<LevelInfo>& levels) {
    Draft d;
    if (!levels.empty())
        d.layout.level = levels.back().key; // plan for the fully upgraded base
    return d;
}
inline Draft EditDraft(const std::string& name, bool global, const Layout& layout) {
    Draft d;
    d.name = name;
    d.global = global;
    d.layout = layout;
    d.layout.modules.resize(static_cast<size_t>(FilledSlots(d.layout.modules)));
    d.layout.buildings.resize(static_cast<size_t>(FilledSlots(d.layout.buildings)));
    return d;
}
// A draft from a share code: a new template, which replaces nothing until it is saved, of the kind
// its level belongs to (`fallbackKind` when no chain has it: a mod's level that isn't loaded).
inline Draft ImportDraft(const SharedTemplate& shared, const KindChains& chains, int fallbackKind) {
    Draft d = EditDraft(std::string(), false, shared.layout);
    const int kind = KindOfLevel(chains, shared.layout.level);
    d.kind = kind >= 0 ? kind : fallbackKind;
    d.suggested = shared.name;
    return d;
}
// The level, modules and buildings of a layout that this game doesn't have: a mod that isn't
// loaded. `levels` is the draft's chain.
inline int MissingParts(const Layout& l, const std::vector<LevelInfo>& levels, const std::vector<Choice>& modules,
                        const std::vector<Choice>& buildings) {
    int missing = LevelIndex(levels, l.level) < 0;
    for (const auto& key : l.modules)
        missing += !key.empty() && !FindChoice(modules, key);
    for (const auto& key : l.buildings)
        missing += !key.empty() && !FindChoice(buildings, key);
    return missing;
}
// The name a draft's share code carries: its library entry's, or the suggested one.
inline std::string DraftShareName(const Draft& d) {
    return d.name.empty() ? d.suggested : d.name;
}

// The layout to store: empty slots between filled ones are kept, trailing ones dropped.
inline Layout DraftLayout(const Draft& d) {
    Layout l = d.layout;
    l.modules.resize(static_cast<size_t>(FilledSlots(l.modules)));
    l.buildings.resize(static_cast<size_t>(FilledSlots(l.buildings)));
    return l;
}
// nullptr when the draft can be saved.
inline const char* DraftProblem(const Draft& d) {
    if (d.layout.level.empty())
        return "Choose a level first";
    if (!Valid(d.layout))
        return "Template is too large";
    return nullptr;
}

enum class EditorRowKind { Level, Module, Building };
struct EditorRow {
    EditorRowKind kind = EditorRowKind::Level;
    int slot = 0;
    std::string label;
};
struct EditorView {
    std::vector<EditorRow> rows; // the visible page
    int page = 0, pages = 1;
    std::string pageLabel;
};
inline std::string SlotsText(int n, const char* what) {
    return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
}
// Rows in order: the level, every module slot, every building slot. A slot beyond the level's
// own count (it needs a modifier, or a later level) is marked with '+'.
inline EditorView BuildEditor(const Draft& d, const std::vector<LevelInfo>& levels, const std::vector<Choice>& modules,
                              const std::vector<Choice>& buildings, int page, int perPage = 12,
                              size_t labelBytes = 40) {
    std::vector<EditorRow> all;
    const bool known = LevelIndex(levels, d.layout.level) >= 0;
    all.push_back(
        {EditorRowKind::Level, 0,
         "Level: " + (d.layout.level.empty() ? std::string("-")
                                             : LevelName(d.layout.level) + (known ? "" : " (not in this game)"))});
    for (Kind kind : {Kind::Module, Kind::Building}) {
        const bool module = kind == Kind::Module;
        const auto& slots = module ? d.layout.modules : d.layout.buildings;
        const int own = LevelSlots(d, kind, levels), count = SlotCount(d, kind, levels);
        for (int i = 0; i < count; ++i) {
            const std::string key = i < static_cast<int>(slots.size()) ? slots[i] : std::string();
            all.push_back({module ? EditorRowKind::Module : EditorRowKind::Building, i,
                           std::string(module ? "M" : "B") + std::to_string(i + 1) + (i >= own ? "+" : "") + ": " +
                               ChoiceName(module ? modules : buildings, key)});
        }
    }
    EditorView view;
    perPage = (std::max)(1, perPage);
    const int count = static_cast<int>(all.size());
    view.pages = (std::max)(1, (count + perPage - 1) / perPage);
    view.page = std::clamp(page, 0, view.pages - 1);
    for (int i = view.page * perPage; i < count && i < (view.page + 1) * perPage; ++i) {
        all[i].label = Ellipsize(all[i].label, labelBytes);
        view.rows.push_back(std::move(all[i]));
    }
    view.pageLabel =
        SlotsText(SlotCount(d, Kind::Module, levels), "module") + ", " +
        SlotsText(SlotCount(d, Kind::Building, levels), "building") +
        (view.pages > 1 ? " - " + std::to_string(view.page + 1) + "/" + std::to_string(view.pages) : std::string());
    return view;
}

struct PickerRow {
    std::string key, label;
    bool selected = false;
};
struct PickerView {
    std::vector<PickerRow> rows; // the visible page; the first entry of page 0 empties the slot
    int page = 0, pages = 1, shown = 0, hidden = 0;
    std::string pageLabel;
};
// What can go in a slot: nothing, then the definitions by name. With a `kind` (not -1) the ones
// for other station kinds are left out, except the slot's current one. A current key without a
// definition (a removed mod) stays listed, so opening the picker never loses it.
inline PickerView BuildPicker(const std::vector<Choice>& choices, int kind, const std::string& current, int page,
                              bool follow, int perPage = 12, size_t labelBytes = 40) {
    std::vector<Choice> list;
    PickerView view;
    for (const auto& c : choices) {
        if (c.key.empty())
            continue;
        if (!ChoiceFits(c, kind) && c.key != current) {
            ++view.hidden;
            continue;
        }
        list.push_back({c.key, c.name.empty() ? Prettify(c.key) : c.name, c.type});
    }
    if (!current.empty() && !FindChoice(choices, current))
        list.push_back({current, ChoiceName(choices, current), -1});
    std::sort(list.begin(), list.end(), [](const Choice& a, const Choice& b) {
        auto lower = [](std::string s) {
            for (auto& ch : s)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        };
        const auto x = lower(a.name), y = lower(b.name);
        return x != y ? x < y : a.key < b.key;
    });
    // Entries that share a name (the game gives a module's starbase, orbital ring, waystation and
    // arkship variants one name) say which they are: by station kind when that tells them apart,
    // otherwise by key.
    for (size_t first = 0; first < list.size();) {
        size_t last = first + 1;
        while (last < list.size() && list[last].name == list[first].name)
            ++last;
        if (last - first > 1) {
            static const char* const tags[] = {"starbase", "orbital ring", "waystation", "arkship", "any station"};
            bool byType = true;
            for (size_t i = first; i < last && byType; ++i) {
                byType = list[i].type >= kTypeStarbase && list[i].type <= kTypeAll;
                for (size_t j = first; j < i && byType; ++j)
                    byType = list[j].type != list[i].type;
            }
            for (size_t i = first; i < last; ++i)
                list[i].name += " (" + (byType ? std::string(tags[list[i].type]) : list[i].key) + ")";
        }
        first = last;
    }
    list.insert(list.begin(), Choice{std::string(), "(empty)", -1});
    view.shown = static_cast<int>(list.size()) - 1;
    perPage = (std::max)(1, perPage);
    const int count = static_cast<int>(list.size());
    view.pages = (std::max)(1, (count + perPage - 1) / perPage);
    int index = 0;
    for (int i = 0; i < count; ++i)
        if (list[i].key == current) {
            index = i;
            break;
        }
    view.page = follow ? index / perPage : std::clamp(page, 0, view.pages - 1);
    for (int i = view.page * perPage; i < count && i < (view.page + 1) * perPage; ++i) {
        PickerRow row;
        row.key = list[i].key;
        row.selected = i == index;
        row.label = Ellipsize(std::string(row.selected ? "> " : "") + list[i].name, labelBytes);
        view.rows.push_back(std::move(row));
    }
    view.pageLabel =
        std::to_string(view.shown) + " listed" +
        (view.pages > 1 ? " - " + std::to_string(view.page + 1) + "/" + std::to_string(view.pages) : std::string());
    return view;
}

// What the values read as starbase_type mean. The enum's numbering is not known for certain, so
// it is learned from vanilla definitions whose script says what they are for. `Of` gives a
// ComponentType, or -1 for a value that wasn't learned (such a definition is listed for every kind).
struct TypeMap {
    std::array<int, 6> canon{-1, -1, -1, -1, -1, -1};
    bool trusted = false;
    std::string note; // for the log: what was learned, or why nothing is filtered
    int Of(int raw) const { return trusted && raw >= 0 && raw < 6 ? canon[raw] : -1; }
};
inline TypeMap LearnTypes(const std::vector<std::string>& keys, const std::vector<int>& types) {
    // In order of preference: a mod may have changed one, and then the earlier ones win.
    static constexpr struct {
        const char* key;
        int type;
    } known[] = {{"shipyard", kTypeAll},
                 {"crew_quarters", kTypeAll},
                 {"anchorage", kTypeAll},
                 {"orbital_ring_habitation", kTypeOrbitalRing},
                 {"ring_minerals_hub", kTypeOrbitalRing},
                 {"waystation_energy_module", kTypeWaystation},
                 {"arkship_engineering_bay", kTypeArkship},
                 {"arkship_armor", kTypeArkship}};
    static const char* const names[] = {"starbase", "orbital_ring", "waystation", "arkship", "all"};
    TypeMap map;
    if (keys.size() != types.size()) {
        map.note = "internal: sizes differ";
        return map;
    }
    for (size_t i = 0; i < keys.size(); ++i)
        if (types[i] < 0 || types[i] > 5) {
            map.note = "value " + std::to_string(types[i]) + " on " + keys[i] + " is outside the enum";
            return map;
        }
    int rawOf[5] = {-1, -1, -1, -1, -1};
    for (const auto& k : known)
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] != k.key)
                continue;
            const int raw = types[i];
            if (map.canon[raw] == -1 && rawOf[k.type] == -1) {
                map.canon[raw] = k.type;
                rawOf[k.type] = raw;
            } else if (map.canon[raw] != k.type)
                map.note += std::string(k.key) + "=" + std::to_string(raw) + " ignored; ";
            break;
        }
    const bool others = rawOf[kTypeOrbitalRing] >= 0 || rawOf[kTypeWaystation] >= 0 || rawOf[kTypeArkship] >= 0;
    if (rawOf[kTypeAll] < 0 || !others) {
        map.canon.fill(-1);
        map.note += "too few vanilla definitions to learn the values from";
        return map;
    }
    // No vanilla definition says "starbase". The game stores 0-3 for the four named types, so
    // with the other three known it is the one left.
    int free = -1, count = 0;
    for (int raw = 0; raw < 4; ++raw)
        if (map.canon[raw] == -1) {
            free = raw;
            ++count;
        }
    if (count == 1 && rawOf[kTypeOrbitalRing] >= 0 && rawOf[kTypeWaystation] >= 0 && rawOf[kTypeArkship] >= 0) {
        map.canon[free] = kTypeStarbase;
        rawOf[kTypeStarbase] = free;
    }
    map.trusted = true;
    for (int type = 0; type < 5; ++type)
        map.note += std::string(names[type]) + "=" +
                    (rawOf[type] < 0 ? std::string("?") : std::to_string(rawOf[type])) + (type < 4 ? " " : "");
    return map;
}

// One line about a layout, for the manager's status line.
inline std::string LayoutSummary(const Layout& l) {
    int modules = 0, buildings = 0;
    for (const auto& s : l.modules)
        modules += !s.empty();
    for (const auto& s : l.buildings)
        buildings += !s.empty();
    return LevelName(l.level) + ": " + SlotsText(modules, "module") + ", " + SlotsText(buildings, "building");
}
} // namespace starbasetemplates
