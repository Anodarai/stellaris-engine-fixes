// Part of starbasetemplates_runtime.inl: the controls in the starbase and waystation views. Button
// text and visibility, the panel's rows for the selected station, the template editor's draft, and
// the views' update hooks that refresh them.

void StarbaseText(uint8_t* button, const std::string& value) {
    auto text = uinavigation::Text(value);
    reinterpret_cast<void (*)(uint8_t*, const game::String*, void*)>(g_starbaseButtonText)(button, &text, nullptr);
}

void StarbaseShowButton(uint8_t* button, bool shown) {
    if (button)
        uinavigation::Method<void (*)(uint8_t*)>(button, shown ? starbasetemplates::kButtonShowSlot
                                                               : starbasetemplates::kButtonHideSlot)(button);
}

// True while the side view shows our panel: same window, our idle mode, and shown.
bool StarbasePanelVisible(uint8_t* view) {
    using namespace starbasetemplates;
    const auto& p = g_starbaseView->panel;
    if (!p.built || !view)
        return false;
    auto* side = uinavigation::Field<uint8_t*>(view, kSideView);
    return side && side == p.side && uinavigation::Field<uint8_t*>(side, kSideWindow) == p.window &&
           uinavigation::Field<uint8_t>(side, kSideMode) == kSideTemplateMode &&
           uinavigation::Field<uint8_t>(p.window, kWindowShown) != 0;
}

void StarbaseHidePanelButtons() {
    auto& p = g_starbaseView->panel;
    p.open = false;
    p.deleteName.clear();
    if (!p.built)
        return;
    for (auto& control : g_starbaseView->controls)
        StarbaseShowButton(control.button, false);
    for (auto& icon : g_starbaseView->icons)
        StarbaseShowButton(icon.button, false);
    StarbaseShowButton(g_starbaseView->background.button, false);
}

// nullptr when the selected starbase can be saved and planned.
const char* StarbaseUnavailable(const starbasetemplates::State& state) {
    if (!state.valid)
        return "No starbase selected";
    if (state.owner != StarbaseLocalCountry())
        return "Not your starbase";
    if (!StarbaseSupported(state))
        return "Unsupported starbase type";
    return nullptr;
}

void StarbaseRowIcon(int row, uint8_t* definition);
bool StarbaseIconButton(uint8_t* button);
void StarbasePlaceButton(uint8_t* button, const starbasetemplates::PanelRect& rect);

void StarbaseRefreshControls(uint32_t id, const char* unavailable) {
    using namespace starbasetemplates;
    std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
    auto& p = g_starbaseView->panel;
    p.dirty = false;
    const auto found = g_starbaseTemplates.jobs.find(id);
    const Job* job = found == g_starbaseTemplates.jobs.end() ? nullptr : &found->second;
    if (g_starbaseView->toggle.button)
        StarbaseText(g_starbaseView->toggle.button, !job          ? "Templates"
                                                    : job->paused ? "Templates (paused)"
                                                                  : "Templates (active)");
    StarbaseAutoUpgradeLook(id);
    // Only the selected station's kind of template is listed.
    const int kind = std::clamp(g_starbaseView->kind, 0, kStationKinds - 1);
    const auto library = KindLibrary(g_starbaseTemplates.library, g_starbaseKinds, kind);
    auto page = BuildPage(library, g_starbaseLocalLibrary.library, g_starbaseSelected, p.page, p.follow,
                          p.geometry.rows, p.geometry.rowText);
    g_starbaseSelected = page.selected;
    p.page = page.page;
    p.follow = false;
    if (!p.open || !p.built)
        return;
    const auto now = GetTickCount64();
    auto* c = g_starbaseView->controls;
    auto& e = g_starbaseView->editor;
    const bool editor = e.mode == StarbaseEditorState::Editor, picker = e.mode == StarbaseEditorState::Picker;
    std::vector<std::string> labels;
    std::string pageLabel, status, title;
    if (editor) {
        e.capacity.Apply(e.levels);
        auto view =
            BuildEditor(e.draft, e.levels, e.modules, e.buildings, e.page, p.geometry.slotRows, p.geometry.rowText);
        e.page = view.page;
        for (const auto& row : view.rows)
            labels.push_back(row.label);
        e.rows = std::move(view.rows);
        pageLabel = view.pageLabel;
        const char* problem = DraftProblem(e.draft);
        status = problem ? std::string(problem)
                 : e.draft.name.empty()
                     ? std::string("New template: click a slot to fill it")
                     : Ellipsize(std::string("Editing ") + (e.draft.global ? "[G] " : "[S] ") + e.draft.name,
                                 p.geometry.statusText);
        title = std::string("Edit: ") + kStationKindInfo[std::clamp(e.draft.kind, 0, kStationKinds - 1)].name;
        StarbaseText(c[kPanelNew].button, "Level -");
        StarbaseText(c[kPanelEdit].button, "Level +");
        StarbaseText(c[kPanelApply].button, "+ Module slot");
        StarbaseText(c[kPanelDelete].button, "+ Building slot");
        StarbaseText(c[kPanelStop].button, "Back to templates");
    } else if (picker) {
        const bool module = e.pickKind == Kind::Module;
        const auto& slots = module ? e.draft.layout.modules : e.draft.layout.buildings;
        const std::string current = e.pickSlot < static_cast<int>(slots.size()) ? slots[e.pickSlot] : std::string();
        auto view = BuildPicker(module ? e.modules : e.buildings, e.thisKindOnly ? e.draft.kind : -1, current,
                                e.pickPage, e.pickFollow, p.geometry.slotRows, p.geometry.rowText);
        e.pickPage = view.page;
        e.pickFollow = false;
        for (const auto& row : view.rows)
            labels.push_back(row.label);
        e.picks = std::move(view.rows);
        pageLabel = view.pageLabel;
        status = view.hidden ? std::to_string(view.hidden) + " for other station kinds hidden"
                             : std::string("Click an entry to put it in the slot");
        title = (module ? "Module slot " : "Building slot ") + std::to_string(e.pickSlot + 1);
        StarbaseText(c[kPanelNew].button, e.thisKindOnly ? "Show all kinds" : "This kind only");
        StarbaseText(c[kPanelEdit].button, "Cancel");
    } else {
        for (int i = 0; i < kPanelMaxRows; ++i)
            p.rows[i] = i < static_cast<int>(page.rows.size()) ? page.rows[i].name : std::string();
        for (const auto& row : page.rows)
            labels.push_back(row.label);
        pageLabel = page.pageLabel;
        status = unavailable ? std::string(unavailable) : PlanStatus(job, p.geometry.statusText);
        title = kStationKindInfo[kind].title;
        if (p.deleteName != g_starbaseSelected || now - p.deleteAt > 4000)
            p.deleteName.clear();
        StarbaseText(c[kPanelNew].button, "New");
        StarbaseText(c[kPanelEdit].button, "Edit");
        StarbaseText(c[kPanelApply].button, "Apply");
        StarbaseText(c[kPanelDelete].button, p.deleteName.empty() ? "Delete" : "Click again to delete");
        StarbaseText(c[kPanelStop].button, job ? "Stop plan" : "Stop plan (none)");
    }
    // The picker has no use for these seven; the other modes relabel the first five.
    for (int i : {kPanelSave, kPanelSaveGlobal, kPanelApply, kPanelDelete, kPanelStop, kPanelCopyCode, kPanelPasteCode})
        StarbaseShowButton(c[i].button, !picker);
    // The editor and picker have taller rows than the template list: same buttons, moved and resized.
    if (p.slotRows != (editor || picker)) {
        p.slotRows = editor || picker;
        for (int i = 0; i < kPanelMaxRows; ++i)
            StarbasePlaceButton(c[kPanelRow0 + i].button, PanelLayout(kPanelRow0 + i, p.geometry, p.slotRows));
    }
    // The content mod's rows are left-aligned: indent past the icon in the editor and picker.
    const std::string indent = !g_starbaseSkin.rows ? "" : editor || picker ? "         " : " ";
    const bool tint = g_starbaseSkin.rows && g_buttonSetModulate;
    for (int i = 0; i < kPanelMaxRows; ++i) {
        auto* button = c[kPanelRow0 + i].button;
        if (i < static_cast<int>(labels.size()))
            StarbaseText(button, indent + labels[i]);
        StarbaseShowButton(button, i < static_cast<int>(labels.size()));
        if (tint && StarbaseIconButton(button)) {
            // The selected template, or the picker's current entry, tinted blue.
            const bool selected = picker ? i < static_cast<int>(e.picks.size()) && e.picks[i].selected
                                         : !editor && i < static_cast<int>(page.rows.size()) && page.rows[i].selected;
            const float color[4] = {selected ? 0.55f : 1.0f, selected ? 0.8f : 1.0f, 1.0f, 1.0f};
            reinterpret_cast<void (*)(uint8_t*, const float*)>(g_buttonSetModulate)(button, color);
        }
    }
    // The game's icon of what an editor slot holds or a picker entry is; none for the level row,
    // empty slots, the picker's "(empty)" and the template list.
    for (int i = 0; i < kPanelMaxRows; ++i) {
        if (!g_starbaseView->icons[i].button)
            continue;
        uint8_t* definition = nullptr;
        try {
            if (editor && i < static_cast<int>(e.rows.size()) && e.rows[i].kind != EditorRowKind::Level) {
                const bool module = e.rows[i].kind == EditorRowKind::Module;
                const auto& slots = module ? e.draft.layout.modules : e.draft.layout.buildings;
                const int index = e.rows[i].slot;
                if (index >= 0 && index < static_cast<int>(slots.size()) && !slots[index].empty())
                    definition =
                        g_starbaseTemplateReader.FindDefinition(module ? Kind::Module : Kind::Building, slots[index]);
            } else if (picker && i < static_cast<int>(e.picks.size()) && !e.picks[i].key.empty()) {
                definition = g_starbaseTemplateReader.FindDefinition(e.pickKind, e.picks[i].key);
            }
        } catch (const std::exception&) {
            definition = nullptr;
        }
        StarbaseRowIcon(i, definition);
    }
    StarbaseText(c[kPanelPage].button, pageLabel);
    const bool notice = !p.notice.empty() && now - p.noticeAt < 5000;
    StarbaseText(c[kPanelStatus].button, notice ? p.notice : status);
    if (p.title != title) {
        if (auto* text = uinavigation::Field<uint8_t*>(p.side, kSideTitle))
            g_textChange(text, title.c_str());
        p.title = title;
    }
}

// What the selected starbase can hold: its capacity is a sum of modifiers, so a mod or a
// technology can give it more slots than its level's definition lists. The editor shows a
// level with at least as many slots as a real starbase of that level had.
void StarbaseObserveCapacity(const starbasetemplates::State& state) {
    const auto& r = g_starbaseTemplateReader;
    int modules = 0, buildings = 0;
    auto* base = starbasetemplates::Reference(r.selection.starbaseDatabase, state.id, r.selection.objectReference);
    if (base && r.Capacity(base, modules, buildings))
        g_starbaseView->editor.capacity.Observe(state.built.level, modules, buildings);
}

// Reads the level chain and the module and building definitions the game has loaded.
bool StarbaseEditorLoad(int kind) {
    auto& e = g_starbaseView->editor;
    const auto& r = g_starbaseTemplateReader;
    try {
        if (kind < 0 || kind >= starbasetemplates::kStationKinds)
            return false;
        auto levels = r.Levels(starbasetemplates::kStationKindInfo[kind].firstLevel);
        auto modules = r.Choices(starbasetemplates::Kind::Module);
        auto buildings = r.Choices(starbasetemplates::Kind::Building);
        if (levels.empty() || (modules.empty() && buildings.empty()))
            return false;
        e.levels = std::move(levels);
        e.modules = std::move(modules);
        e.buildings = std::move(buildings);
        e.capacity.Apply(e.levels);
        // Once per run: whether the picker can tell the station kinds' components apart.
        static bool logged = false;
        if (!logged) {
            logged = true;
            const auto types = r.Types();
            Log(types.trusted ? LogLevel::Info : LogLevel::Warn, "starbase_templates",
                "starbase_type values: %s%s; %zu modules, %zu buildings", types.note.c_str(),
                types.trusted ? "" : " - the picker lists every kind's", e.modules.size(), e.buildings.size());
        }
        return true;
    } catch (const std::exception& error) {
        Log(LogLevel::Warn, "starbase_templates", "template editor: %s", error.what());
        return false;
    }
}

void StarbaseRefreshViewCpp(uint8_t* view) {
    try {
        auto* slot = StarbaseSlotOfView(view);
        if (!g_starbaseTemplatesOn || !slot || starbasetemplates::ReadField<uint8_t*>(view, 0x78) != slot->toggle.root)
            return;
        g_starbaseView = slot;
        const bool starbaseView = slot == &g_starbaseViews[0];
        auto& p = g_starbaseView->panel;
        // Every frame: the side view switched to native content or closed, or a parent show
        // re-showed our hidden buttons. Hide is a no-op for hidden buttons.
        if (p.built && !(p.open && StarbasePanelVisible(view)))
            StarbaseHidePanelButtons();
        // CStarbaseDefensesTabController::Setup hands the whole Defenses tab its own tooltip handler
        // (recursively), which has nothing for our icon. Put ours back every frame: one store.
        if (auto* icon = starbaseView ? g_autoUpgradeToggle.button : nullptr)
            uinavigation::Method<uinavigation::PointerMethod>(icon, 0x220)(
                icon, reinterpret_cast<uint8_t*>(&g_autoUpgradeTooltip));
        // The shipyard checkbox follows the selected starbase every frame (a few field reads).
        if (starbaseView)
            ReinforceRefresh(view);
        const auto now = GetTickCount64();
        if (!p.dirty && now - p.refreshed < 500)
            return;
        p.refreshed = now;
        devstats::Scope perfRefresh(g_perfViewRefresh);
        const auto id =
            starbasetemplates::ReadField<uint32_t>(view, g_starbaseTemplateReader.selection.selectedReference);
        const auto state = g_starbaseTemplateReader.Snapshot(id);
        StarbaseUpdateKind(state);
        const char* unavailable = StarbaseUnavailable(state);
        if (!unavailable)
            StarbaseObserveCapacity(state);
        StarbaseRefreshControls(id, unavailable);
    } catch (...) {
        StarbaseDisable("exception refreshing the station view");
    }
}
void StarbaseRefreshView(uint8_t* view) {
    const LONGLONG perfStart = devstats::Ticks();
    __try {
        StarbaseRefreshViewCpp(view);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        StarbaseDisable("fault refreshing the station view");
    }
    g_perfView.AddSince(perfStart);
}
void StarbaseViewUpdateHook(uint8_t* view) {
    g_starbaseViewUpdate(view);
    StarbaseRefreshView(view);
}
void WaystationViewUpdateHook(uint8_t* view) {
    g_waystationViewUpdate(view);
    StarbaseRefreshView(view);
}
