// The starbase features' runtime: starbase templates, defense platform auto-upgrade and the
// shipyard reinforcement checkbox (re/starbase_templates.md). Included by dllmain.cpp after shared UI
// and storage infrastructure. All execution is on the game thread; sidecar snapshots use store ->
// template lock order.
//
// The parts, in the order they depend on:
//   starbasetemplates_state.inl      timers, control ids, per-view panel and editor state, skin
//   starbaseautoupgrade_runtime.inl  defense platform auto-upgrade and its icon's look and tooltip
//   starbasetemplates_frame.inl      the frame pass: auto-upgrade and staged construction
//   starbasetemplates_view.inl       the controls in the station views and their refresh
//   starbasetemplates_dialogs.inl    the name and share-code dialogs, deleting a template
//   starbasetemplates_clicks.inl     what a click on the panel does
//   starbasetemplates_widgets.inl    building the panel and the auto-upgrade icon from game GUI types
//   shipyardreinforce_runtime.inl    the Shipyard tab's checkbox and its CalcBuildLocations filter
// This file then attaches them to a view on every GUI reload, and logs what they cost (dev build).

#include "starbasetemplates_state.inl"
#include "starbaseautoupgrade_runtime.inl"
#include "starbasetemplates_frame.inl"
#include "starbasetemplates_view.inl"
#include "starbasetemplates_dialogs.inl"
#include "starbasetemplates_clicks.inl"
#include "starbasetemplates_widgets.inl"

#include "shipyardreinforce_runtime.inl"

// The content mod's types for the panel, or retrofit for each one it doesn't define.
void StarbaseLoadSkin(uint8_t* gui, uint8_t* retrofit) {
    using namespace uinavigation;
    StarbaseSkin skin;
    auto* window = GuiType(gui + 0x110, kStarbaseSkinWindow);
    skin.on = window != nullptr;
    int found = 0;
    std::string missing;
    for (int role = 0; role < kSkinRoles; ++role) {
        auto* type = window ? GuiType(window + 0x2d0, kStarbaseSkinNames[role]) : nullptr;
        // Only a standard button: the panel's code knows its fields and methods.
        if (type &&
            Method<void (*)()>(type, 0x68) != reinterpret_cast<void (*)()>(g_navigation.locations.buttonFactory))
            type = nullptr;
        if (type)
            ++found;
        else if (window)
            missing += std::string(missing.empty() ? "" : ", ") + kStarbaseSkinNames[role];
        skin.types[role] = type ? type : role == kSkinBackground ? nullptr : retrofit;
    }
    skin.rows = skin.types[kSkinRow] != retrofit;
    const bool changed = skin.on != g_starbaseSkin.on || skin.rows != g_starbaseSkin.rows ||
                         (skin.types[kSkinBackground] != nullptr) != (g_starbaseSkin.types[kSkinBackground] != nullptr);
    static bool logged = false;
    if (!logged || changed) {
        if (!skin.on)
            Log(LogLevel::Info, "starbase_templates", "panel look: basic (no %s window type)", kStarbaseSkinWindow);
        else
            Log(LogLevel::Info, "starbase_templates", "panel look: content mod, %d of %d button types%s%s", found,
                static_cast<int>(kSkinRoles), missing.empty() ? "" : "; basic for ", missing.c_str());
        logged = true;
    }
    g_starbaseSkin = skin;
}

void AddStarbaseControlsImpl(uint8_t* view, bool reloaded) {
    using namespace uinavigation;
    const auto& name = Field<game::String>(view, 0x48);
    const std::string_view viewName(name.data(), name.size);
    if (viewName == "starbase_side_view") {
        // A reload replaces the side view's window and, with it, our buttons.
        for (auto& slot : g_starbaseViews)
            if (view == slot.panel.side) {
                slot.panel = {};
                for (auto& icon : slot.icons)
                    icon = {};
                slot.background = {};
            }
        return;
    }
    // The waystation view is laid out like the starbase view: the same starbase_tab, and its
    // own side view. It gets the Templates button only when its refresh is hooked.
    const int index = viewName == "starbase_view"                               ? 0
                      : viewName == "waystation_view" && g_waystationViewUpdate ? 1
                                                                                : -1;
    if (!reloaded || !g_starbaseTemplatesOn || index < 0)
        return;
    g_starbaseView = &g_starbaseViews[index];
    g_starbaseView->kind = index == 1 ? starbasetemplates::kStationWaystation : starbasetemplates::kStationStarbase;
    auto* root = Field<uint8_t*>(view, 0x78);
    const auto tabName = Text("starbase_tab");
    auto* parent = root ? reinterpret_cast<NamedLookup>(g_navigation.locations.window)(root, &tabName) : nullptr;
    if (!parent)
        return;
    auto* gui = Field<uint8_t*>(parent, 0xb10);
    auto* typeRoot = GuiType(gui + 0x110, "starbase_view");
    auto* tabType = typeRoot ? GuiType(typeRoot + 0x2d0, "starbase_tab") : nullptr;
    auto* design = tabType ? GuiType(tabType + 0x2d0, "design_info") : nullptr;
    auto* type = design ? GuiType(design + 0x2d0, "retrofit") : nullptr;
    if (!type || Method<void (*)()>(type, 0x68) != reinterpret_cast<void (*)()>(g_navigation.locations.buttonFactory))
        return;
    g_starbaseButtonType = type;
    StarbaseLoadSkin(gui, type);
    StarbaseCreateButton(type, parent, {562, 484}, {11, 0, 150, 20}, "sef_template_toggle", "Templates",
                         g_starbaseView->toggle, view, root, kStarbaseToggleAction);
    if (index == 0 && g_autoUpgradeOn)
        AddAutoUpgradeIcon(root, view);
    if (index == 0 && g_reinforceOn)
        AddReinforceCheckbox(root, view);
    // The side view keeps its window across this reload; its controls follow the new root.
    for (auto& control : g_starbaseView->controls) {
        control.view = view;
        control.root = root;
    }
    for (auto& control : g_starbaseView->icons) {
        control.view = view;
        control.root = root;
    }
    StarbaseRefreshControls(UINT32_MAX, nullptr);
}
void AddStarbaseControlsCpp(uint8_t* view, bool reloaded) {
    try {
        AddStarbaseControlsImpl(view, reloaded);
    } catch (...) {
        StarbaseDisable("exception adding the controls");
    }
}
void AddStarbaseControls(uint8_t* view, bool reloaded) {
    const LONGLONG perfStart = devstats::Ticks();
    __try {
        AddStarbaseControlsCpp(view, reloaded);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        StarbaseDisable("fault adding the controls");
    }
    g_perfReload.AddSince(perfStart);
}

// Three lines per 30 s summary with what the starbase features cost in that interval. Each ms is
// the total in the interval, so ms / (10 * interval_s) is the percentage of one core.
#ifdef SEF_DEV_STATS
void LogStarbasePerf() {
    if (!g_starbaseTemplatesOn && !g_reinforceOn)
        return;
    static ULONGLONG last = GetTickCount64();
    const ULONGLONG now = GetTickCount64();
    const double seconds = (now - last) / 1000.0;
    last = now;
    using Taken = devstats::Timer::Taken;
    size_t jobs = 0, inFlight = 0, scheduled = 0, library = 0;
    {
        std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
        jobs = g_starbaseTemplates.jobs.size();
        inFlight = g_starbaseTemplates.inFlight.size();
        scheduled = g_autoUpgrade.bases.size();
        library = g_starbaseTemplates.library.size();
    }
    const Taken frame = g_perfFrame.Take(), pass = g_perfFramePass.Take(), job = g_perfJobStep.Take();
    const Taken view = g_perfView.Take(), refresh = g_perfViewRefresh.Take(), reload = g_perfReload.Take();
    Log(LogLevel::Stats, "starbase_perf",
        "interval_s=%.1f frame_calls=%llu frame_ms=%.3f frame_max_us=%.1f "
        "passes=%llu pass_ms=%.3f pass_max_us=%.1f job_steps=%llu job_ms=%.3f job_max_us=%.1f "
        "jobs=%zu in_flight=%zu autoupgrade_bases=%zu templates=%zu",
        seconds, frame.calls, frame.ms, frame.maxUs, pass.calls, pass.ms, pass.maxUs, job.calls, job.ms, job.maxUs,
        jobs, inFlight, scheduled, library);
    Log(LogLevel::Stats, "starbase_perf",
        "view_updates=%llu view_ms=%.3f view_max_us=%.1f "
        "view_refreshes=%llu refresh_ms=%.3f refresh_max_us=%.1f gui_reloads=%llu reload_ms=%.3f reload_max_us=%.1f",
        view.calls, view.ms, view.maxUs, refresh.calls, refresh.ms, refresh.maxUs, reload.calls, reload.ms,
        reload.maxUs);
    const Taken game = g_perfCalcGame.Take(), filter = g_perfCalcFilter.Take();
    Log(LogLevel::Stats, "starbase_perf",
        "calc_build_locations=%llu off_main_thread=%llu game_ms=%.3f game_max_us=%.1f "
        "filter_calls=%llu filter_ms=%.3f filter_max_us=%.1f lists_shortened=%llu unchecked_shipyards=%zu",
        game.calls, static_cast<unsigned long long>(g_perfCalcOffMain.exchange(0)), game.ms, game.maxUs, filter.calls,
        filter.ms, filter.maxUs, static_cast<unsigned long long>(g_perfCalcFiltered.exchange(0)),
        g_noReinforceCount.load());
}
#endif
