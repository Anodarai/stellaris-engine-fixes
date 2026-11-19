#pragma once

#include "../core/locate.h"

namespace uinavigation {

// CGalaxyView::Hide looks up empires_window/galaxy_empires before resetting this scrollbar.
inline uint8_t* LocateContactsScrollReset(const locate::Image& image, std::string& error) {
    constexpr char reset[] = "48 8B 83 88 02 00 00 48 85 C0 74 11 80 78 09 00 74 0B 0F 57 C9 48 8B CB "
                             "E8 ?? ?? ?? ?? 48 8B 4F 78 48 8B 01 FF 50 48";
    auto* match = locate::FindOne(image, "Contacts scroll reset", reset, error);
    return match ? match + 24 : nullptr;
}

inline uint8_t* LocateDiplomacyContactsBranch(const locate::Image& image, std::string& error) {
    return locate::FindOne(image, "Diplomacy close path from Contacts",
                           "38 9F A9 1D 00 00 74 ?? 48 8B 1D ?? ?? ?? ?? 48 8B 8B 80 0D 00 00", error);
}

struct ContactsReturn {
    uint8_t* closeCallbackLoad = nullptr;
    uint8_t** idler = nullptr;
    size_t fromContacts = 0;
    size_t contactsView = 0;
};

inline bool LocateContactsReturn(const locate::Image& image, ContactsReturn& out, std::string& error) {
    auto* branch = LocateDiplomacyContactsBranch(image, error);
    if (!branch)
        return false;
    // CDiplomacyView's constructor binds the close button (including Escape) to the virtual Hide.
    auto* load = locate::FindOne(image, "Diplomacy close button binding",
                                 "48 8D 05 ?? ?? ?? ?? 48 89 45 E0 44 89 7D E8 48 89 8F 68 01 00 00 "
                                 "48 89 BF 70 01 00 00 0F 10 45 E0 0F 11 87 78 01 00 00",
                                 error);
    if (!load)
        return false;
    const auto* callback = locate::RelTarget(load + 3, load + 7);
    if (!locate::MatchAt(callback, locate::Parse("48 8B 01 FF 60 38"))) {
        error = "Diplomacy close callback does not dispatch to Hide";
        return false;
    }
    out.closeCallbackLoad = load;
    out.idler = reinterpret_cast<uint8_t**>(locate::RelTarget(branch + 11, branch + 15));
    out.fromContacts = locate::Disp32(branch + 2);
    out.contactsView = locate::Disp32(branch + 18);
    return true;
}

struct Locations {
    uint8_t* reload = nullptr;
    uint8_t* window = nullptr;
    uint8_t* buttonFactory = nullptr;
    uint8_t* setShortcut = nullptr;
    uint8_t* insertButton = nullptr;
    uint8_t* insertButtonName = nullptr;

    uint8_t** idler = nullptr;
    uint8_t** gameState = nullptr;
    uint8_t* observedCountry = nullptr;
    uint8_t* select = nullptr;
    uint8_t* selectedFleet = nullptr;
    uint8_t* fleetVisible = nullptr;
    uint8_t* isTransport = nullptr;
    size_t fleetData = 0;
    size_t fleetCount = 0;
    size_t fleetClass = 0;
    size_t selectionCount = 0;

    uint8_t* showMega = nullptr;
    uint8_t* constructScope = nullptr;
    uint8_t* destroyScope = nullptr;
    uint8_t* guiScope = nullptr;
    size_t megaData = 0;
    size_t megaCount = 0;
    size_t megaType = 0;
    size_t megaFlags = 0;
    size_t megaTriggerCount = 0;
    size_t megaTrigger = 0;
    size_t megaTarget = 0;

    uint8_t* showDiplomacy = nullptr;
    uint8_t* updateContacts = nullptr;
    uint8_t* canOpenDiplomacy = nullptr;
    uint8_t* hasCustomDiplomacy = nullptr;
    uint8_t* opensShroud = nullptr;
    uint8_t** gameRules = nullptr;
    uint8_t** countryDb = nullptr;
    size_t countryId = 0;
    size_t diplomacyTarget = 0;
    size_t contactsData = 0;
    size_t contactsCount = 0;
    size_t contactsView = 0;
};

inline bool Locate(const locate::Image& image, Locations& out, std::string& error) {
    const auto find = [&](const char* name, const char* pattern) {
        return locate::FindOne(image, name, pattern, error);
    };
    const auto call = [](uint8_t* instruction) { return locate::RelTarget(instruction + 1, instruction + 5); };

    out.reload = find("CGuiView::Reload", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F9 48 8B 49 78");
    if (!out.reload)
        return false;
    out.window = find("recursive GUI window lookup", "48 89 74 24 10 57 48 83 EC 20 48 8B F2 4C 8D 81 70 08 00 00");
    if (!out.window)
        return false;
    out.buttonFactory =
        find("CButtonType::InstantiateStandardButton", "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 89 48 08 "
                                                       "55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 18 FF FF FF");
    if (!out.buttonFactory)
        return false;
    auto* shortcut = find("CButton::SetShortcutFromString",
                          "48 83 EC 30 48 8B DA 48 8B F9 48 83 C2 10 48 83 7A 18 10 72 ?? 48 8B 12 "
                          "49 C7 C0 FF FF FF FF 49 FF C0 42 80 3C 02 00 75 ?? 48 81 C1 38 01 00 00");
    if (!shortcut)
        return false;
    out.setShortcut = shortcut - 6;
    auto* insert = find("CContainerWindow button registration", "41 8B 56 14 4C 8D 44 24 30 49 8B CE E8 ?? ?? ?? ?? "
                                                                "41 8B 55 14 4C 8B C5 49 8B CD E8 ?? ?? ?? ??");
    if (!insert)
        return false;
    out.insertButton = call(insert + 12);
    out.insertButtonName = call(insert + 27);

    auto* open = find("Contacts diplomacy opener", "E8 ?? ?? ?? ?? 4C 8B F0 48 8B 0D ?? ?? ?? ?? 48 8B B9 80 0B 00 00");
    if (!open)
        return false;
    out.idler = reinterpret_cast<uint8_t**>(locate::RelTarget(open + 11, open + 15));
    auto* observed =
        find("Contacts local observed country", "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B E8 48 89 45 E7");
    if (!observed)
        return false;
    out.gameState = reinterpret_cast<uint8_t**>(locate::RelTarget(observed + 3, observed + 7));
    out.observedCountry = call(observed + 7);
    out.select =
        find("CInGameIdler::Select", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F2 48 8B D9 E8 ?? ?? ?? ??");
    if (!out.select)
        return false;
    out.selectedFleet =
        find("CInGameIdler::GetSelectedFleet", "48 83 EC 28 83 B9 8C 0A 00 00 00 7E ?? 48 8B 89 80 0A 00 00");
    if (!out.selectedFleet)
        return false;
    out.selectionCount = locate::Disp32(out.selectedFleet + 6);
    out.fleetVisible =
        find("CFleet::ShouldShowInOutliner", "48 89 5C 24 08 57 48 83 EC 20 0F B6 FA 48 8B D9 38 91 80 12 00 00");
    if (!out.fleetVisible)
        return false;
    out.fleetClass = locate::Disp32(out.fleetVisible + 18);
    out.isTransport = find("CFleet::IsTransportFleet", "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 ?? 32 C0");
    if (!out.isTransport)
        return false;
    auto* fleets = find("controlled fleet list", "49 8B BD 98 2C 00 00 49 63 85 A4 2C 00 00 4C 8D 34 87");
    if (!fleets)
        return false;
    out.fleetData = locate::Disp32(fleets + 3);
    out.fleetCount = locate::Disp32(fleets + 10);

    out.showMega = find("CMegaStructureView::Show",
                        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 42 08 48 8B F9 89 81 B0 04 00 00");
    if (!out.showMega)
        return false;
    out.megaTarget = locate::Disp32(out.showMega + 23);
    auto* megas = find("owned megastructure list", "49 8B BE C8 2F 00 00 49 63 86 D4 2F 00 00 4C 8D 34 87");
    if (!megas)
        return false;
    out.megaData = locate::Disp32(megas + 3);
    out.megaCount = locate::Disp32(megas + 10);
    auto* flags = find("megastructure outliner eligibility",
                       "48 8B 8B 78 01 00 00 8B 81 48 24 00 00 C1 E8 05 24 01 83 B9 FC 23 00 00 00 75 ??");
    if (!flags)
        return false;
    out.megaType = locate::Disp32(flags + 3);
    out.megaFlags = locate::Disp32(flags + 9);
    out.megaTriggerCount = locate::Disp32(flags + 20);
    auto* trigger = find("megastructure outliner trigger", "48 81 C1 70 23 00 00 48 8B 01 48 8D 54 24 40 FF 50 18");
    if (!trigger)
        return false;
    out.megaTrigger = locate::Disp32(trigger + 3);
    out.constructScope = find("CEventScope constructor", "40 53 48 83 EC 20 48 8B D9 48 8D 05 ?? ?? ?? ?? 33 C9 33 D2");
    if (!out.constructScope)
        return false;
    out.destroyScope =
        find("CEventScope destructor", "40 53 48 83 EC 20 48 8B D9 48 83 C1 70 E8 ?? ?? ?? ?? 48 8B 4B 60");
    if (!out.destroyScope)
        return false;
    auto* guard = find("megastructure outliner GUI scope flag",
                       "C6 05 ?? ?? ?? ?? 01 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B F0 C6 05 ?? ?? ?? ?? 01");
    if (!guard)
        return false;
    out.guiScope = locate::RelTarget(guard + 2, guard + 7);
    if (out.guiScope != locate::RelTarget(guard + 24, guard + 29) || call(guard + 14) != out.observedCountry) {
        error = "megastructure outliner GUI scope call sites disagree";
        return false;
    }

    out.updateContacts =
        find("CGalaxyView::UpdateEmpires", "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC E8 00 00 00 "
                                           "0F 29 B4 24 D0 00 00 00 4C 8B F9 48 8B 89 D0 00 00 00");
    if (!out.updateContacts)
        return false;
    auto* contacts = find("sorted Contacts rows", "45 8B 87 74 04 00 00 49 8B 97 68 04 00 00 48 8B 4D F7");
    if (!contacts)
        return false;
    out.contactsCount = locate::Disp32(contacts + 3);
    out.contactsData = locate::Disp32(contacts + 10);
    auto* close = LocateDiplomacyContactsBranch(image, error);
    if (!close)
        return false;
    if (reinterpret_cast<uint8_t**>(locate::RelTarget(close + 11, close + 15)) != out.idler) {
        error = "Diplomacy open and close paths disagree about the active idler";
        return false;
    }
    out.contactsView = locate::Disp32(close + 18);
    auto* available =
        find("standard diplomacy availability call", "E8 ?? ?? ?? ?? 84 C0 40 0F 94 C7 84 C0 75 ?? 48 8B 03");
    if (!available)
        return false;
    out.canOpenDiplomacy = call(available);
    out.hasCustomDiplomacy =
        find("CCountry::HasCustomDiplomacyWith", "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 10 03 00 00 48 8B FA");
    if (!out.hasCustomDiplomacy)
        return false;
    auto* shroud = find("Contacts Shroud routing", "48 8B D0 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B CF");
    if (!shroud)
        return false;
    out.gameRules = reinterpret_cast<uint8_t**>(locate::RelTarget(shroud + 6, shroud + 10));
    out.opensShroud = call(shroud + 10);
    out.showDiplomacy =
        find("CDiplomacyView::Show", "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 "
                                     "48 8B F1 48 8B 15 ?? ?? ?? ??");
    if (!out.showDiplomacy)
        return false;
    auto* target = find("Diplomacy current country lookup",
                        "48 85 D2 74 ?? 44 8B 81 98 06 00 00 41 8B C0 25 FF FF FF 00 "
                        "3B 42 20 73 ?? 8B C8 48 03 C9 48 8B 42 18 48 8B 4C C8 08 48 85 C9 74 ?? 44 39 41 20");
    if (!target)
        return false;
    if (target != out.showDiplomacy + 36 || observed < out.updateContacts || observed > out.updateContacts + 0x200) {
        error = "navigation call sites disagree with their owning functions";
        return false;
    }
    out.countryDb = reinterpret_cast<uint8_t**>(locate::RelTarget(target - 4, target));
    out.diplomacyTarget = locate::Disp32(target + 8);
    out.countryId = target[47];
    return true;
}

} // namespace uinavigation
