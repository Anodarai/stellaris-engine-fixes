// Part of starbasetemplates_runtime.inl: what a click on the panel does, in the manager, the editor
// and its picker, and the observer tables the cloned buttons call.

void StarbaseOpenPanel(uint8_t* view);
void StarbaseClosePanel(uint8_t* view);

// A click while the panel shows the editor or its picker.
void StarbaseEditorClick(int action) {
    using namespace starbasetemplates;
    auto& p = g_starbaseView->panel;
    auto& e = g_starbaseView->editor;
    const int row = action - kPanelRow0;
    p.dirty = true;
    if (e.mode == StarbaseEditorState::Picker) {
        if (action == kPanelNew) {
            e.thisKindOnly = !e.thisKindOnly;
            e.pickFollow = true;
        } else if (action == kPanelEdit) {
            e.mode = StarbaseEditorState::Editor;
        } else if (action == kPanelPrev || action == kPanelNext) {
            e.pickPage = (std::max)(0, e.pickPage + (action == kPanelNext ? 1 : -1));
        } else if (row >= 0 && row < static_cast<int>(e.picks.size())) {
            if (!SetSlot(e.draft, e.pickKind, e.pickSlot, e.picks[row].key))
                StarbaseNotice("Cannot set this slot");
            e.mode = StarbaseEditorState::Editor;
        }
        return;
    }
    if (action == kPanelCopyCode) {
        if (const char* problem = DraftProblem(e.draft))
            StarbaseNotice(problem);
        else
            StarbaseCopyCode(DraftLayout(e.draft), DraftShareName(e.draft));
    } else if (action == kPanelPasteCode) {
        StarbasePasteCode();
    } else if (action == kPanelSave || action == kPanelSaveGlobal) {
        const uint32_t local = StarbaseLocalCountry();
        if (const char* problem = DraftProblem(e.draft))
            StarbaseNotice(problem);
        else if (local == UINT32_MAX)
            StarbaseNotice("No game loaded");
        else {
            State state;
            state.id = UINT32_MAX;
            state.owner = local;
            state.built = DraftLayout(e.draft);
            OpenStarbaseName(state, action == kPanelSaveGlobal, true, DraftShareName(e.draft));
        }
    } else if (action == kPanelNew || action == kPanelEdit) {
        if (!StepLevel(e.draft, e.levels, action == kPanelEdit ? 1 : -1))
            StarbaseNotice(action == kPanelEdit ? "Already the last level" : "Already the first level");
    } else if (action == kPanelApply || action == kPanelDelete) {
        const Kind kind = action == kPanelApply ? Kind::Module : Kind::Building;
        if (!AddSlot(e.draft, kind, e.levels))
            StarbaseNotice("No more slots");
        else {
            // Show the new slot: rows are the level, the module slots, the building slots.
            const int modules = SlotCount(e.draft, Kind::Module, e.levels);
            const int index = kind == Kind::Module ? modules : modules + SlotCount(e.draft, Kind::Building, e.levels);
            e.page = index / (std::max)(1, p.geometry.slotRows);
        }
    } else if (action == kPanelStop) {
        e.mode = StarbaseEditorState::Manager;
        p.follow = true;
    } else if (action == kPanelPrev || action == kPanelNext) {
        e.page = (std::max)(0, e.page + (action == kPanelNext ? 1 : -1));
    } else if (row >= 0 && row < static_cast<int>(e.rows.size())) {
        if (e.rows[row].kind == EditorRowKind::Level) {
            StarbaseNotice("Use Level - and Level + to change it");
        } else {
            e.pickKind = e.rows[row].kind == EditorRowKind::Module ? Kind::Module : Kind::Building;
            e.pickSlot = e.rows[row].slot;
            e.pickFollow = true;
            e.mode = StarbaseEditorState::Picker;
        }
    }
}

void StarbaseClickImpl(StarbaseControl* control) {
    using namespace uinavigation;
    using namespace starbasetemplates;
    if (!g_starbaseTemplatesOn || g_starbaseDialog || StoreMultiplayer() || !control->view ||
        Field<uint8_t*>(control->view, 0x78) != control->root ||
        !Method<BoolMethod>(control->view, 0x70)(control->view))
        return;
    auto* idler = *g_navigation.locations.idler;
    auto* gui = Field<uint8_t*>(control->root, 0xb10);
    if (!idler || !gui || Field<uint8_t*>(gui, 0x300) || Field<uint8_t*>(idler, 0xdd0) || Field<uint8_t*>(idler, 0xdd8))
        return;
    auto* view = control->view;
    g_starbaseView = StarbaseSlotOfControl(control);
    auto& p = g_starbaseView->panel;
    const auto id = Field<uint32_t>(view, g_starbaseTemplateReader.selection.selectedReference);
    const auto state = g_starbaseTemplateReader.Snapshot(id);
    StarbaseUpdateKind(state);
    const char* unavailable = StarbaseUnavailable(state);
    const int action = control->action;
    if (action == kStarbaseToggleAction) {
        if (p.open && StarbasePanelVisible(view))
            StarbaseClosePanel(view);
        else
            StarbaseOpenPanel(view);
    } else if (action == kAutoUpgradeAction) {
        std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
        const int64_t today = StarbaseToday();
        const char* problem = !g_autoUpgradeOn                        ? "Auto-upgrade unavailable"
                              : !state.valid                          ? "No starbase selected"
                              : state.owner != StarbaseLocalCountry() ? "Not your starbase"
                              : today < 0                             ? "No game date"
                                                                      : nullptr;
        if (!problem && g_autoUpgrade.Disable(id)) {
            g_autoUpgradeLast.erase(id);
            Log(LogLevel::Info, "starbase_autoupgrade", "starbase %08X: off", id);
        } else if (!problem && g_autoUpgrade.Enable(id, state.owner, today)) {
            g_starbaseLastPass = 0; // the first run is today: let the next frame pass take it
            Log(LogLevel::Info, "starbase_autoupgrade", "starbase %08X: on, runs monthly from %s", id,
                FormatDate(static_cast<int32_t>(today * 24)).c_str());
        } else {
            g_autoUpgradeRefusedAt = GetTickCount64();
            Log(LogLevel::Info, "starbase_autoupgrade", "starbase %08X: %s", id,
                problem ? problem : "too many starbases scheduled");
        }
    } else if (action == kReinforceAction) {
        ReinforceClick(id);
    } else if (!p.open || !StarbasePanelVisible(view)) {
        return; // A click that raced the panel closing.
    } else if (g_starbaseView->editor.mode != StarbaseEditorState::Manager) {
        StarbaseEditorClick(action);
    } else if (action == kPanelNew || action == kPanelEdit) {
        auto& e = g_starbaseView->editor;
        std::string name;
        Layout layout;
        bool global = false;
        int kind = g_starbaseView->kind; // New: a template for the selected station's kind
        if (action == kPanelEdit) {
            std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
            const auto found = g_starbaseTemplates.library.find(g_starbaseSelected);
            if (found != g_starbaseTemplates.library.end()) {
                name = found->first;
                layout = found->second;
                global = !g_starbaseLocalLibrary.library.count(name);
                const int own = KindOfLevel(g_starbaseKinds, layout.level);
                if (own >= 0)
                    kind = own;
            }
        }
        if (action == kPanelEdit && name.empty())
            StarbaseNotice("No template selected");
        else if (!StarbaseEditorLoad(kind))
            StarbaseNotice("Cannot read the game's station definitions");
        else {
            e.draft = action == kPanelEdit ? EditDraft(name, global, layout) : NewDraft(e.levels);
            e.draft.kind = kind;
            e.page = 0;
            e.mode = StarbaseEditorState::Editor;
            p.dirty = true;
        }
    } else if (action == kPanelCopyCode) {
        std::string name;
        Layout layout;
        {
            std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
            const auto found = g_starbaseTemplates.library.find(g_starbaseSelected);
            if (found != g_starbaseTemplates.library.end()) {
                name = found->first;
                layout = found->second;
            }
        }
        if (name.empty())
            StarbaseNotice("No template selected");
        else
            StarbaseCopyCode(layout, name);
    } else if (action == kPanelPasteCode) {
        StarbasePasteCode();
    } else if (action == kPanelSave || action == kPanelSaveGlobal) {
        if (unavailable)
            StarbaseNotice(unavailable);
        else
            OpenStarbaseName(state, action == kPanelSaveGlobal);
    } else if (action == kPanelApply) {
        std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
        const auto chosen = g_starbaseTemplates.library.find(g_starbaseSelected);
        const int own =
            chosen == g_starbaseTemplates.library.end() ? -1 : KindOfLevel(g_starbaseKinds, chosen->second.level);
        if (unavailable)
            StarbaseNotice(unavailable);
        else if (g_starbaseSelected.empty())
            StarbaseNotice("Save a template first");
        else if (own >= 0 && own != KindOfLevel(g_starbaseKinds, state.built.level))
            StarbaseNotice(std::string("That template is for: ") + kStationKindInfo[own].name);
        else if (g_starbaseTemplates.Apply(g_starbaseSelected, state))
            StarbaseNotice("Plan started: " + Ellipsize(g_starbaseSelected, 30));
        else
            StarbaseNotice("Cannot start: last order not yet confirmed");
    } else if (action == kPanelStop) {
        std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
        if (!state.valid || state.owner != StarbaseLocalCountry() || !g_starbaseTemplates.jobs.count(id)) {
            StarbaseNotice("No plan to stop");
        } else {
            g_starbaseTemplates.Stop(id);
            StarbaseNotice("Plan stopped; queued work continues");
        }
    } else if (action == kPanelDelete) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
            name = g_starbaseSelected;
        }
        const auto now = GetTickCount64();
        if (name.empty()) {
            StarbaseNotice("No template selected");
        } else if (p.deleteName != name || now - p.deleteAt > 4000) {
            p.deleteName = name; // Two clicks: a deleted global template cannot be recovered.
            p.deleteAt = now;
            p.dirty = true;
        } else if (now - p.deleteAt < 500) {
            // The second half of a double click, before "Click again to delete" could be read.
        } else {
            p.deleteName.clear();
            std::string error;
            if (DeleteStarbaseTemplate(name, error)) {
                p.follow = true;
                StarbaseNotice("Deleted " + Ellipsize(name, 36));
            } else
                StarbaseNotice(error);
        }
    } else if (action == kPanelPrev || action == kPanelNext) {
        p.page = (std::max)(0, p.page + (action == kPanelNext ? 1 : -1));
        p.dirty = true;
    } else if (action >= kPanelRow0 && action < kPanelControls && !p.rows[action - kPanelRow0].empty()) {
        std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
        g_starbaseSelected = p.rows[action - kPanelRow0];
        p.deleteName.clear();
        p.dirty = true;
        const auto found = g_starbaseTemplates.library.find(g_starbaseSelected);
        if (found != g_starbaseTemplates.library.end())
            StarbaseNotice(LayoutSummary(found->second));
    }
    StarbaseRefreshControls(id, unavailable);
}
void StarbaseClickCpp(StarbaseControl* control) {
    try {
        StarbaseClickImpl(control);
    } catch (...) {
        StarbaseDisable("exception handling a click");
    }
}
StarbaseControl* g_starbaseClicked = nullptr; // the last control whose click was handled, and when
ULONGLONG g_starbaseClickedAt = 0;
void StarbaseClick(StarbaseControl* control, const void*) {
    g_starbaseClicked = control;
    g_starbaseClickedAt = GetTickCount64();
    __try {
        StarbaseClickCpp(control);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        StarbaseDisable("fault handling a click");
    }
}
// A second click soon after the first reaches a button as OnDblClick, not OnClick, so paging with
// two quick clicks moved one page. It counts as a click. The observer has two OnDblClick
// overloads (slots 11 and 12; CButtonObserverGlue in the GOG 4.5.2 PDB) and which the button
// calls is not known, so both are handled and a second call for the same press is dropped.
// Not for the shipyard checkbox: that toggles itself and reports through OnClick.
void StarbaseDblClick(StarbaseControl* control, const void*) {
    if (!control || control->action == kReinforceAction)
        return;
    if (control == g_starbaseClicked && GetTickCount64() - g_starbaseClickedAt < 30)
        return;
    StarbaseClick(control, nullptr);
}
void StarbaseIgnore(StarbaseControl*, const void*) {
}
const StarbaseControl::Callback kStarbaseObserver[] = {
    StarbaseIgnore,   StarbaseClick,  StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore,
    StarbaseIgnore,   StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseDblClick,
    StarbaseDblClick, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore};
// The backdrop takes clicks between the rows and does nothing with them.
const StarbaseControl::Callback kStarbaseInertObserver[] = {
    StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore,
    StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore,
    StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore};
