void InstallStarbaseTemplates(HMODULE self, const locate::Image& image) {
    if (!IniOnLogged(self, "interface", "starbase_templates", true, "starbase_templates"))
        return;
    if (!g_storeOn || !g_origLoad || !g_origStartSave || !g_origZipAndWrite || !g_origCheckTick ||
        !g_origNavigationReload) {
        Log(LogLevel::Warn, "starbase_templates", "required storage, frame or GUI hooks unavailable; disabled");
        return;
    }
    std::string error;
    auto& reader = g_starbaseTemplateReader;
    if (!starbasetemplates::LocateSelection(image, reader.selection, error) ||
        !starbasetemplates::LocateConstruction(image, reader.construction, error) ||
        !starbasetemplates::LocateState(image, reader.fields, error)) {
        Log(LogLevel::Warn, "starbase_templates", "%s; disabled", error.c_str());
        return;
    }
    // The template editor's slot counts and names. Without them it still works: slots come
    // from "+ slot" and from what templates already hold, names from the keys.
    {
        std::string editorError;
        reader.editorLocated = starbasetemplates::LocateEditor(image, reader.fields, reader.editor, editorError);
        reader.gameFree = reinterpret_cast<void (*)(void*)>(g_str.gameFree);
        if (!reader.editorLocated)
            Log(LogLevel::Warn, "starbase_templates", "%s; the template editor can't read slot counts",
                editorError.c_str());
        else if (!reader.editor.componentName || !reader.gameFree)
            Log(LogLevel::Warn, "starbase_templates",
                "module and building names not found; the template editor shows keys");
        if (reader.editorLocated && !reader.editor.componentType)
            Log(LogLevel::Warn, "starbase_templates",
                "starbase_type not found; the template editor lists every module and building for every kind");
    }
    g_starbaseButtonText = locate::FindOne(image, "template button text", starbasetemplates::kButtonText, error);
    g_starbaseButtonSize = locate::FindOne(image, "template button size", starbasetemplates::kButtonSize, error);
    g_starbaseIsDesignable = locate::FindOne(image, "designable starbase", starbasetemplates::kIsDesignable, error);
    auto* update = locate::FindOne(image, "starbase view update", starbasetemplates::kViewUpdate, error);
    if (!g_starbaseButtonText || !g_starbaseButtonSize || !g_starbaseIsDesignable || !update) {
        Log(LogLevel::Warn, "starbase_templates", "%s; disabled", error.c_str());
        return;
    }
    // The management panel lives in the native starbase side view. The patterns carry the
    // side view's field offsets literally, so a layout change fails here rather than at run time.
    g_sideShowForSlot = reinterpret_cast<void (*)(uint8_t*, uint8_t, int)>(
        locate::FindOne(image, "CStarbaseSideView::ShowForSlot", starbasetemplates::kSideShowForSlot, error));
    g_sideHide = reinterpret_cast<void (*)(uint8_t*)>(
        locate::FindOne(image, "CStarbaseSideView::Hide", starbasetemplates::kSideHide, error));
    g_textChange = reinterpret_cast<void (*)(uint8_t*, const char*)>(
        locate::FindOne(image, "CInstantTextBox::ChangeString", starbasetemplates::kTextChange, error));
    if (!g_sideShowForSlot || !g_sideHide || !g_textChange ||
        !locate::FindOne(image, "starbase side view layout", starbasetemplates::kSideClear, error)) {
        Log(LogLevel::Warn, "starbase_templates", "%s; disabled", error.c_str());
        return;
    }
    // Placement: without these the buttons keep the cloned type's orientation and centring,
    // and the panel uses the vanilla size. Both only matter with UI mods, so they are optional.
    {
        std::string placement;
        auto* moveTo = locate::FindOne(image, "CButtonStandard::MoveTo", starbasetemplates::kButtonMoveTo, placement);
        const bool centre =
            locate::FindOne(image, "button centerPosition", starbasetemplates::kButtonCentre, placement) != nullptr;
        g_buttonMoveTo = moveTo && centre ? moveTo : nullptr;
        g_windowGetSize = reinterpret_cast<uinavigation::Point* (*)(uint8_t*, uinavigation::Point*)>(
            locate::FindOne(image, "CContainerWindow::GetSize", starbasetemplates::kWindowGetSize, placement));
        if (!g_buttonMoveTo || !g_windowGetSize)
            Log(LogLevel::Warn, "starbase_templates", "%s; buttons use the GUI file's placement", placement.c_str());
    }
    // Icons on the template panel's rows and on the Templates button: the native lists' own icon
    // helper, on buttons placed with MoveTo. Optional.
    {
        std::string icons = "button placement unavailable";
        if (!g_buttonMoveTo || !starbasetemplates::LocateIcons(image, g_starbaseIcons, icons)) {
            g_starbaseIcons = {};
            Log(LogLevel::Warn, "starbase_templates", "%s; no icons in the template panel", icons.c_str());
        }
    }
    // Share the native dialog implementation, including when trade entry is disabled.
    if (g_tradeUi.dialogConstructor)
        g_starbaseDialogUi = g_tradeUi;
    else if (!uishortcuts::LocateTrade(image, g_starbaseDialogUi, error)) {
        Log(LogLevel::Warn, "starbase_templates", "%s; disabled", error.c_str());
        return;
    }
    auto* accept = reinterpret_cast<uint8_t*>(*g_starbaseDialogUi.dialogAcceptSlot);
    auto* destroy = reinterpret_cast<uint8_t*>(*g_starbaseDialogUi.dialogDestroySlot);
    g_starbaseAcceptOriginal = reinterpret_cast<TradeUiAction>(accept);
    g_starbaseDestroyOriginal = reinterpret_cast<TradeUiDestroy>(destroy);
    if (!SwapVtableSlot(g_starbaseDialogUi.dialogAcceptSlot, accept,
                        reinterpret_cast<void*>(&StarbaseDialogAcceptHook)) ||
        !SwapVtableSlot(g_starbaseDialogUi.dialogDestroySlot, destroy,
                        reinterpret_cast<void*>(&StarbaseDialogDestroyHook))) {
        Log(LogLevel::Warn, "starbase_templates", "could not hook template name dialog; disabled");
        return;
    }
    // Two stack stores and three pushes, with no relative operands (14 bytes).
    g_starbaseViewUpdate = reinterpret_cast<void (*)(uint8_t*)>(
        InstallJumpHook(update, 14, reinterpret_cast<void*>(&StarbaseViewUpdateHook)));
    if (!g_starbaseViewUpdate) {
        Log(LogLevel::Warn, "starbase_templates", "could not hook view refresh; disabled");
        return;
    }
    // Waystations have their own view. Without its refresh they get no Templates button; the
    // other kinds are unaffected. The first 15 bytes are three stack stores.
    {
        std::string waystationError;
        auto* waystation =
            locate::FindOne(image, "waystation view update", starbasetemplates::kWaystationViewUpdate, waystationError);
        if (waystation && reinterpret_cast<uint8_t**>(locate::RelTarget(waystation + 42, waystation + 46)) !=
                              reader.selection.starbaseDatabase) {
            waystation = nullptr;
            waystationError = "waystation view cross-check failed";
        }
        if (waystation)
            g_waystationViewUpdate = reinterpret_cast<void (*)(uint8_t*)>(
                InstallJumpHook(waystation, starbasetemplates::kWaystationViewUpdateStolen,
                                reinterpret_cast<void*>(&WaystationViewUpdateHook)));
        if (!g_waystationViewUpdate)
            Log(LogLevel::Warn, "starbase_templates", "%s; no Templates button in the waystation view",
                waystation ? "could not hook the waystation view refresh" : waystationError.c_str());
    }
    // Defense platform auto-upgrade: the defenses tab's own Upgrade command, run monthly. Its
    // toggle is an icon that shows the state through alpha and tint, and copies a native icon's
    // placement, so it needs those button methods and MoveTo.
    g_autoUpgradeOn = false;
    if (IniOnLogged(self, "interface", "starbase_autoupgrade", true, "starbase_autoupgrade")) {
        g_buttonSetAlpha =
            locate::FindOne(image, "CButtonStandard::SetAlpha", starbasetemplates::kButtonSetAlpha, error);
        g_buttonSetModulate = g_buttonSetAlpha ? locate::FindOne(image, "CButtonStandard::SetModulateColor",
                                                                 starbasetemplates::kButtonSetModulate, error)
                                               : nullptr;
        if (!g_bench.gameState)
            Log(LogLevel::Warn, "starbase_autoupgrade", "the game date wasn't found (see the ticks line); off");
        else if (!g_buttonSetModulate || !g_buttonMoveTo)
            Log(LogLevel::Warn, "starbase_autoupgrade", "%s; off",
                g_buttonMoveTo ? error.c_str() : "button placement unavailable");
        else if (!starbasetemplates::LocatePlatformUpgrade(image, reader.selection, reader.construction,
                                                           g_platformUpgrade, error))
            Log(LogLevel::Warn, "starbase_autoupgrade", "%s; off", error.c_str());
        else
            g_autoUpgradeOn = true;
    }
    // Shipyard checkbox: a hook on the game's choice of shipyards for reinforcements, and a real
    // checkbox and label made from the fleet view's types.
    g_reinforceOn = false;
    if (IniOnLogged(self, "interface", "shipyard_reinforce_toggle", true, "shipyard_reinforce")) {
        std::string reinforceError;
        if (!shipyardreinforce::Locate(image, reader.construction.mainQueue, g_reinforce, reinforceError)) {
            Log(LogLevel::Warn, "shipyard_reinforce", "%s; off", reinforceError.c_str());
        } else {
            g_calcBuildLocations = reinterpret_cast<CalcBuildLocationsFn>(
                InstallJumpHook(g_reinforce.calcBuildLocations, shipyardreinforce::kCalcBuildLocationsStolen,
                                reinterpret_cast<void*>(&CalcBuildLocationsHook)));
            if (g_calcBuildLocations)
                g_reinforceOn = true;
            else
                Log(LogLevel::Warn, "shipyard_reinforce", "could not hook the reinforcement shipyard choice; off");
        }
    }
    {
        std::lock_guard<std::mutex> storeLock(g_storeLock);
        const auto& global = StoreFor(pdxstore::Which::Global);
        std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
        const auto* payload = global.Get(kStarbaseLibraryKey);
        if (payload && !g_starbaseGlobalLibrary.Deserialize(*payload, true)) {
            Log(LogLevel::Error, "starbase_templates", "invalid template library; disabled to preserve stored data");
            return;
        }
        RebuildStarbaseLibrary();
        g_starbaseTemplatesOn = true;
    }
    Log(LogLevel::Info, "starbase_templates",
        "installed single-player Templates button, side-panel template manager and staged construction%s%s",
        g_autoUpgradeOn ? "; defense platform auto-upgrade" : "",
        g_reinforceOn ? "; shipyard reinforcement checkbox" : "");
}
