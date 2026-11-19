// View navigation ([interface] view_navigation; uinavigation_runtime.h) and the hook on
// CGuiView::Reload that attaches its buttons. The same hook attaches the starbase and ship
// designer controls, so it is installed when either view_navigation or starbase_templates is on.
//
// dllmain.cpp includes this file inside its anonymous namespace, after starbasetemplates_runtime.inl
// and shipdesignlibrary_runtime.inl; the hook's state is at the end of interface_runtime.inl.

void SortNavigationMilitaryFleets(std::vector<uint32_t>& ids) {
    if (g_fleetFailed || g_inFleetPass || !g_fleet.fleetGroupVtblSlot ||
        *g_fleet.fleetGroupVtblSlot != reinterpret_cast<uintptr_t>(&FleetGroupUpdateHook))
        return;
    auto sorted = ids;
    g_fleetKeys.clear();
    std::stable_sort(sorted.begin(), sorted.end(), FleetLess);
    g_fleetKeys.clear();
    if (!g_fleetFailed)
        ids.swap(sorted);
}

bool SortNavigationMegastructures(const std::vector<uint8_t*>& shown, std::vector<uint8_t*>& sorted) {
    return !g_megaFailed && g_mega.groupVtblSlot &&
           *g_mega.groupVtblSlot == reinterpret_cast<uintptr_t>(&MegaGroupUpdateHook) &&
           SortMegastructures(shown, sorted);
}

bool __fastcall NavigationReloadHook(uint8_t* view) {
    const bool result = g_origNavigationReload(view);
    AddStarbaseControls(view, result);
    AddShipDesignControls(view, result);
    if (result && !g_navigation.disabled) {
        __try {
            if (!uinavigation::AddControls(g_navigation, view, g_navigationControls)) {
                Log(LogLevel::Warn, "view_navigation", "could not attach navigation buttons to a view");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_navigation.disabled = true;
            Log(LogLevel::Error, "view_navigation", "fault attaching navigation buttons; disabled");
        }
    }
    return result;
}

void InstallViewNavigation(HMODULE self, const locate::Image& image) {
    const bool navigation = IniOnLogged(self, "interface", "view_navigation", true, "view_navigation");
    g_navigation.disabled = !navigation;
    if (!navigation && !IniOn(self, "interface", "starbase_templates", true))
        return;

    std::string error;
    if (!uinavigation::Locate(image, g_navigation.locations, error)) {
        Log(LogLevel::Warn, "view_navigation", "%s", error.c_str());
        return;
    }
    if (!g_fleet.fleetDb || !g_mega.megaDb) {
        Log(LogLevel::Warn, "view_navigation", "could not locate the fleet and megastructure databases");
        g_navigation.disabled = true;
    }
    g_navigation.fleet = [](uint32_t id) { return ResolveRef(*g_fleet.fleetDb, id, g_fleet.fleetIdOffset); };
    g_navigation.mega = [](uint32_t id) { return ResolveRef(*g_mega.megaDb, id, g_mega.megaIdOffset); };
    g_navigation.fleetId = g_fleet.fleetIdOffset;
    g_navigation.megaId = g_mega.megaIdOffset;
    g_navigation.sortMilitary = SortNavigationMilitaryFleets;
    g_navigation.sortMegas = SortNavigationMegastructures;
    // Two stack stores, push rdi and sub rsp,20h. No relative operands.
    constexpr size_t stolen = 15;
    g_origNavigationReload = reinterpret_cast<GuiReloadFn>(
        InstallJumpHook(g_navigation.locations.reload, stolen, reinterpret_cast<void*>(&NavigationReloadHook)));
    if (!g_origNavigationReload) {
        Log(LogLevel::Warn, "view_navigation", "could not hook CGuiView::Reload");
        return;
    }
    Log(LogLevel::Info, "view_navigation",
        g_navigation.disabled
            ? "GUI reload hook installed for the starbase and ship designer controls only"
            : "installed next/previous controls for fleets, space combat, megastructures and Diplomacy");
}
