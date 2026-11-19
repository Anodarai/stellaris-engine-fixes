#pragma once

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "uinavigation.h"

namespace uinavigation {

enum class View { Fleet, Combat, Megastructure, Diplomacy, Count };
enum class ShipClass : uint8_t { Military = 0, Construction = 1, Science = 3, Transport = 4, SpecialMilitary = 11 };

template <class T> T& Field(uint8_t* object, size_t offset) {
    return *reinterpret_cast<T*>(object + offset);
}

template <class Fn> Fn Method(uint8_t* object, size_t slot) {
    return reinterpret_cast<Fn>(Field<uintptr_t*>(object, 0)[slot / sizeof(uintptr_t)]);
}

template <class T> struct Items {
    T* data;
    int count;

    static Items Read(uint8_t* owner, size_t dataOffset, size_t countOffset) {
        const auto data = Field<T*>(owner, dataOffset);
        const auto count = Field<int>(owner, countOffset);
        if (!data || count < 0 || count > 0xffffff)
            return {};
        return {data, count};
    }
};

// game::Borrow with kind 1 for short text too, as the GUI calls here have always been given.
// Text over 15 bytes must be NUL-terminated and outlive the String.
inline game::String Text(std::string_view value) {
    game::String text = game::Borrow(value);
    text.kind = 1;
    return text;
}

inline uint32_t Adjacent(const std::vector<uint32_t>& ids, uint32_t current, bool forward) {
    if (ids.empty())
        return UINT32_MAX;
    auto it = std::find(ids.begin(), ids.end(), current);
    if (it == ids.end())
        return forward ? ids.front() : ids.back();
    if (forward) {
        ++it;
        return it == ids.end() ? ids.front() : *it;
    }
    return it == ids.begin() ? ids.back() : *std::prev(it);
}

using Resolve = uint8_t* (*)(uint32_t);
using SortFleets = void (*)(std::vector<uint32_t>&);
using SortMegas = bool (*)(const std::vector<uint8_t*>&, std::vector<uint8_t*>&);

struct Runtime {
    Locations locations;
    Resolve fleet = nullptr;
    Resolve mega = nullptr;
    size_t fleetId = 0;
    size_t megaId = 0;
    SortFleets sortMilitary = nullptr;
    SortMegas sortMegas = nullptr;
    bool disabled = false;
};

using PtrMethod = uint8_t* (*)(uint8_t*);
using BoolMethod = bool (*)(uint8_t*);
using PointerMethod = void (*)(uint8_t*, uint8_t*);
using CountryCheck = bool (*)(uint8_t*, uint8_t*);
using NamedLookup = uint8_t* (*)(uint8_t*, const game::String*);

inline void CloseDiplomacyFromButton(const ContactsReturn& locations, uint8_t* view) {
    auto* idler = *locations.idler;
    const bool fromContacts = Field<uint8_t>(view, locations.fromContacts) != 0;
    Method<void (*)(uint8_t*)>(view, 0x38)(view); // CDiplomacyView::Hide
    if (!fromContacts || !idler || *locations.idler != idler)
        return;

    auto* contacts = Field<uint8_t*>(idler, locations.contactsView);
    if (contacts && !Method<BoolMethod>(contacts, 0x70)(contacts)) {
        Method<void (*)(uint8_t*, bool)>(contacts, 0x40)(contacts, true); // CGalaxyView::SetShown
    }
}

inline void Select(Runtime& runtime, uint8_t* idler, uint8_t* selectable) {
    reinterpret_cast<PointerMethod>(runtime.locations.select)(idler, selectable);
}

inline bool FleetEligible(const Locations& locations, uint8_t* fleet, ShipClass category) {
    const auto type = Field<ShipClass>(fleet, locations.fleetClass);
    if (type != category && !(category == ShipClass::Military && type == ShipClass::SpecialMilitary))
        return false;
    if (!Method<BoolMethod>(fleet + 0x18, 0x20)(fleet + 0x18))
        return false;
    if (category == ShipClass::Transport)
        return reinterpret_cast<BoolMethod>(locations.isTransport)(fleet);
    return reinterpret_cast<bool (*)(uint8_t*, ShipClass)>(locations.fleetVisible)(fleet, type);
}

inline void CycleFleet(Runtime& runtime, uint8_t* idler, uint8_t* local, bool forward) {
    const auto& locations = runtime.locations;
    if (Field<int>(idler, locations.selectionCount) != 1)
        return;
    auto* current = reinterpret_cast<PtrMethod>(locations.selectedFleet)(idler);
    if (!current)
        return;
    const uint32_t currentId = Field<uint32_t>(current, runtime.fleetId);
    if (runtime.fleet(currentId) != current)
        return;
    auto category = Field<ShipClass>(current, locations.fleetClass);
    if (category == ShipClass::SpecialMilitary)
        category = ShipClass::Military;
    if (category != ShipClass::Military && category != ShipClass::Construction && category != ShipClass::Science &&
        category != ShipClass::Transport)
        return;

    const auto owned = Items<uint32_t>::Read(local, locations.fleetData, locations.fleetCount);
    std::vector<uint32_t> ids;
    for (int i = 0; i < owned.count; ++i) {
        const auto id = owned.data[i];
        auto* fleet = runtime.fleet(id);
        if (fleet && FleetEligible(locations, fleet, category))
            ids.push_back(id);
    }
    if (std::find(ids.begin(), ids.end(), currentId) == ids.end())
        return;
    if (category == ShipClass::Military && runtime.sortMilitary)
        runtime.sortMilitary(ids);
    const auto next = Adjacent(ids, currentId, forward);
    if (next == currentId)
        return;
    if (auto* destination = runtime.fleet(next))
        Select(runtime, idler, destination + 0x18);
}

inline bool MegaEligible(const Locations& locations, uint8_t* mega) {
    auto* type = Field<uint8_t*>(mega, locations.megaType);
    if (!type || !(Field<uint32_t>(type, locations.megaFlags) & (1u << 5)))
        return false;
    if (!Field<int>(type, locations.megaTriggerCount))
        return true;

    // COutlinerGroupMegastructure evaluates the trigger with a direct megastructure scope.
    alignas(16) uint8_t scope[0x170];
    const auto previous = *locations.guiScope;
    bool constructed = false;
    bool result = false;
    *locations.guiScope = 1;
    __try {
        reinterpret_cast<PtrMethod>(locations.constructScope)(scope);
        constructed = true;
        Field<uint64_t>(scope, 8) = 1;
        Field<uint32_t>(scope, 0x10) = 0;
        memcpy(scope + 0x1c, &mega, sizeof(mega));
        auto* trigger = type + locations.megaTrigger;
        result = Method<CountryCheck>(trigger, 0x18)(trigger, scope);
    } __finally {
        if (constructed)
            reinterpret_cast<void (*)(uint8_t*)>(locations.destroyScope)(scope);
        *locations.guiScope = previous;
    }
    return result;
}

inline void CycleMega(Runtime& runtime, uint8_t* view, uint8_t* idler, uint8_t* local, bool forward) {
    const auto& locations = runtime.locations;
    const auto current = Field<uint32_t>(view, locations.megaTarget);
    const auto owned = Items<uint32_t>::Read(local, locations.megaData, locations.megaCount);
    std::vector<uint8_t*> megas;
    for (int i = 0; i < owned.count; ++i) {
        auto* candidate = runtime.mega(owned.data[i]);
        if (candidate && MegaEligible(locations, candidate))
            megas.push_back(candidate);
    }
    if (runtime.sortMegas) {
        std::vector<uint8_t*> sorted;
        if (runtime.sortMegas(megas, sorted))
            megas.swap(sorted);
    }
    std::vector<uint32_t> ids;
    ids.reserve(megas.size());
    for (auto* candidate : megas)
        ids.push_back(Field<uint32_t>(candidate, runtime.megaId));
    if (std::find(ids.begin(), ids.end(), current) == ids.end())
        return;
    const auto next = Adjacent(ids, current, forward);
    if (next == current)
        return;
    if (auto* destination = runtime.mega(next)) {
        Select(runtime, idler, destination + 0x28);
        reinterpret_cast<PointerMethod>(locations.showMega)(view, destination);
    }
}

inline void CycleDiplomacy(Runtime& runtime, uint8_t* view, uint8_t* idler, uint8_t* local, bool forward) {
    const auto& locations = runtime.locations;
    auto* contacts = Field<uint8_t*>(idler, locations.contactsView);
    auto* trade = Field<uint8_t*>(view, 0x6e0);
    if (!contacts || !Field<uint8_t*>(contacts, 0xd0) || !*locations.gameRules || Field<uint8_t*>(view, 0x6a0) ||
        (trade && Method<BoolMethod>(trade, 0x70)(trade)))
        return;

    // Use the native sorted, filtered Contacts rows even while its window is hidden.
    reinterpret_cast<void (*)(uint8_t*)>(locations.updateContacts)(contacts);
    const auto rows = Items<uint8_t*>::Read(contacts, locations.contactsData, locations.contactsCount);
    const auto resolve = [&](uint32_t id) { return game::ResolveRef(*locations.countryDb, id, locations.countryId); };
    const auto available = reinterpret_cast<bool (*)(uint8_t*, uint8_t*, game::String*)>(locations.canOpenDiplomacy);
    const auto hasCustom = reinterpret_cast<CountryCheck>(locations.hasCustomDiplomacy);
    const auto shroud = reinterpret_cast<CountryCheck>(locations.opensShroud);
    std::vector<uint32_t> ids;
    for (int i = 0; i < rows.count; ++i) {
        auto* item = rows.data[i];
        auto* cache = item ? Field<uint8_t*>(item, 0x288) : nullptr;
        if (!cache)
            continue;
        const auto id = Field<uint32_t>(cache, 4);
        auto* candidate = resolve(id);
        if (candidate && candidate != local && !hasCustom(candidate, local) &&
            !shroud(*locations.gameRules, candidate) && available(local, candidate, nullptr)) {
            ids.push_back(id);
        }
    }
    auto& current = Field<uint32_t>(view, locations.diplomacyTarget);
    const auto next = Adjacent(ids, current, forward);
    if (next == UINT32_MAX || next == current || !resolve(next))
        return;

    // Show refreshes the existing panel and its active tab. The Contacts return flag stays intact.
    current = next;
    reinterpret_cast<void (*)(uint8_t*)>(locations.showDiplomacy)(view);
}

struct Control;
using ObserverCallback = void (*)(Control*, const void*);

// Button observers have one vtable pointer. Stellaris owns the attachment node and frees it
// when the button dies. The observer itself stays alive with the DLL across GUI reloads.
struct Control {
    const ObserverCallback* vtable;
    Runtime* runtime;
    uint8_t* view;
    uint8_t* root;
    View kind;
    bool forward;
};

inline void Cycle(Control* control, const void*) {
    auto& runtime = *control->runtime;
    const auto& locations = runtime.locations;
    auto* idler = *locations.idler;
    if (runtime.disabled || !idler || !*locations.gameState || Field<uint8_t*>(control->view, 0x78) != control->root ||
        !Method<BoolMethod>(control->view, 0x70)(control->view))
        return;
    auto* gui = Field<uint8_t*>(control->root, 0xb10);
    if (!gui || Field<uint8_t*>(gui, 0x300))
        return;
    if (Field<uint8_t*>(idler, 0xdd0) || Field<uint8_t*>(idler, 0xdd8))
        return;

    auto* local = reinterpret_cast<PtrMethod>(locations.observedCountry)(*locations.gameState);
    if (!local || game::ResolveRef(*locations.countryDb, Field<uint32_t>(local, locations.countryId),
                                   locations.countryId) != local)
        return;
    switch (control->kind) {
    case View::Fleet:
    case View::Combat:
        CycleFleet(runtime, idler, local, control->forward);
        break;
    case View::Megastructure:
        CycleMega(runtime, control->view, idler, local, control->forward);
        break;
    case View::Diplomacy:
        CycleDiplomacy(runtime, control->view, idler, local, control->forward);
        break;
    case View::Count:
        break;
    }
}

inline void DispatchClick(Control* control, const void* event) {
    try {
        Cycle(control, event);
    } catch (...) {
        control->runtime->disabled = true;
    }
}

inline void OnClick(Control* control, const void* event) {
    // Native UI callbacks must not unwind through the game's dispatcher.
    __try {
        DispatchClick(control, event);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        control->runtime->disabled = true;
    }
}

inline void IgnoreButtonEvent(Control*, const void*) {
}

inline constexpr ObserverCallback kObserver[] = {
    IgnoreButtonEvent, OnClick,           IgnoreButtonEvent, IgnoreButtonEvent, IgnoreButtonEvent, IgnoreButtonEvent,
    IgnoreButtonEvent, IgnoreButtonEvent, IgnoreButtonEvent, IgnoreButtonEvent, IgnoreButtonEvent, IgnoreButtonEvent,
    IgnoreButtonEvent, IgnoreButtonEvent, IgnoreButtonEvent, IgnoreButtonEvent, IgnoreButtonEvent};

inline uint8_t* GuiType(uint8_t* types, const char* name) {
    const auto key = Text(name);
    return Method<NamedLookup>(types, 8)(types, &key);
}

struct Point {
    int32_t x;
    int32_t y;
};

struct ArrowPlacement {
    Point previous;
    Point next;
    Point windowSize;
    int32_t orientation;
};

inline bool LocateArrowPlacement(uint8_t* parent, ArrowPlacement& placement) {
    // CContainerWindow holds parallel arrays of buttons and their lookup names.
    const auto buttons = Items<uint8_t*>::Read(parent, 0x668, 0x674);
    const auto names = Items<game::String>::Read(parent, 0x680, 0x68c);
    if (buttons.count != names.count)
        return false;
    uint8_t* close = nullptr;
    for (int i = 0; i < names.count; ++i) {
        const auto& name = names.data[i];
        if (std::string_view(name.data(), name.size) == "close") {
            close = buttons.data[i];
            break;
        }
    }
    if (!close)
        return false;

    // Use the close button's own coordinates so the arrows move and resize with the same header.
    const auto position = Field<Point>(close, 0x1a8); // CButtonStandard::_originalPosition
    placement.orientation = Field<int32_t>(close, 0x2a0);
    placement.windowSize = Field<Point>(close, 0x2a4);
    int32_t left = position.x;
    for (int i = 0; i < buttons.count; ++i) {
        auto* button = buttons.data[i];
        if (!button || Field<int32_t>(button, 0x2a0) != placement.orientation)
            continue;
        const auto other = Field<Point>(button, 0x1a8);
        // Include nearby actions in the same row, including hidden variants that can become visible.
        if (other.y == position.y && other.x >= position.x - 180 && other.x < left)
            left = other.x;
    }
    placement.previous = {left - 72, position.y};
    placement.next = {left - 36, position.y};
    return true;
}

inline void CreateArrow(const Locations& locations, Control& control, uint8_t* parent, uint8_t* type,
                        const ArrowPlacement& placement) {
    using ButtonFactory = uint8_t* (*)(uint8_t*, uint8_t*, uint8_t*, uint8_t*, Point);
    using InsertPointer = void (*)(uint8_t*, int, uint8_t* const*);
    using InsertName = void (*)(uint8_t*, int, const game::String*);
    using Attach = void (*)(uint8_t*, Control*);
    auto* gui = Field<uint8_t*>(parent, 0xb10);
    auto* parentType = Field<uint8_t*>(parent, 0xb18);
    auto* workspace = Field<uint8_t*>(parent, 0xb58);
    auto* button = Method<ButtonFactory>(type, 0x68)(type, Field<uint8_t*>(parentType, 0x5b0), gui, workspace,
                                                     placement.windowSize);
    Method<PointerMethod>(gui, 0x98)(gui, button);
    Method<PointerMethod>(button, 0xf0)(button, workspace);
    auto* tooltipHandler = Method<PtrMethod>(parent + 0x38, 0x228)(parent + 0x38);
    Method<PointerMethod>(button, 0x220)(button, tooltipHandler);
    const auto position = control.forward ? placement.next : placement.previous;
    Field<int32_t>(button, 0x2a0) = placement.orientation;
    Field<Point>(button, 0x1a8) = position;
    Method<void (*)(uint8_t*, Point)>(button, 0x2b0)(button, position);
    const auto shortcut = Text(control.forward ? "TAB" : "SHIFT+TAB");
    reinterpret_cast<void (*)(uint8_t*, const game::String*)>(locations.setShortcut)(button, &shortcut);

    // Match CContainerWindow::CreateButton: the button belongs to the window and its priority list.
    auto* priority = parent + 0x630;
    auto* buttons = parent + 0x660;
    auto* names = parent + 0x678;
    const auto name = Text(control.forward ? "sef_next" : "sef_previous");
    reinterpret_cast<InsertPointer>(locations.insertButton)(buttons, Field<int>(buttons, 0x14), &button);
    reinterpret_cast<InsertName>(locations.insertButtonName)(names, Field<int>(names, 0x14), &name);
    reinterpret_cast<InsertPointer>(locations.insertButton)(priority, Field<int>(priority, 0x14), &button);
    auto* observable = button + 0xd8;
    Method<Attach>(observable, 8)(observable, &control);
}

using ViewControls = std::array<std::array<Control, 2>, static_cast<size_t>(View::Count)>;

inline bool AddControls(Runtime& runtime, uint8_t* view, ViewControls& controls) {
    const auto& name = Field<game::String>(view, 0x48);
    const std::string_view window(name.data(), name.size);
    View kind;
    if (window == "fleet_view") {
        kind = View::Fleet;
    } else if (window == "combat_view") {
        kind = View::Combat;
    } else if (window == "megastructure_view") {
        kind = View::Megastructure;
    } else if (window == "diplomacy_view") {
        kind = View::Diplomacy;
    } else {
        return true;
    }

    auto* root = Field<uint8_t*>(view, 0x78);
    if (!root)
        return false;
    auto* gui = Field<uint8_t*>(root, 0xb10);
    if (!gui)
        return false;
    auto* starbase = GuiType(gui + 0x110, "starbase_view");
    auto* header = starbase ? GuiType(starbase + 0x2d0, "header_actions") : nullptr;
    auto* previous = header ? GuiType(header + 0x2d0, "previous_starbase") : nullptr;
    auto* next = header ? GuiType(header + 0x2d0, "next_starbase") : nullptr;
    using FactorySlot = void (*)();
    const auto factory = reinterpret_cast<FactorySlot>(runtime.locations.buttonFactory);
    if (!previous || !next || Method<FactorySlot>(previous, 0x68) != factory ||
        Method<FactorySlot>(next, 0x68) != factory)
        return false;

    auto* parent = root;
    if (kind == View::Fleet || kind == View::Megastructure) {
        const auto headerName = Text("header");
        parent = reinterpret_cast<NamedLookup>(runtime.locations.window)(root, &headerName);
        if (!parent)
            return false;
    }
    ArrowPlacement placement{};
    if (!LocateArrowPlacement(parent, placement))
        return false;
    auto& pair = controls[static_cast<size_t>(kind)];
    pair[0] = {kObserver, &runtime, view, root, kind, false};
    pair[1] = {kObserver, &runtime, view, root, kind, true};
    CreateArrow(runtime.locations, pair[0], parent, previous, placement);
    CreateArrow(runtime.locations, pair[1], parent, next, placement);
    return true;
}

} // namespace uinavigation
