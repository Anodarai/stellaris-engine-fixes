// Part of shipdesignlibrary_runtime.inl: the library panel. Writing the library to the global store,
// the Save dialog's name, the panel's rows and paging, and what each click does.

// --- Library writes --------------------------------------------------------------------------------
// Writes `library` to the global store at once. On failure nothing changes and `error` says why.
bool WriteDesignLibrary(const sdl::Library& library, std::string& error) {
    std::lock_guard<std::mutex> storeLock(g_storeLock);
    auto store = g_globalStore;
    if (library.empty())
        store.Remove(sdl::kStoreKey);
    else if (!store.Set(sdl::kStoreKey, sdl::Serialize(library))) {
        error = "Design library is full";
        return false;
    }
    auto previous = g_globalStore;
    g_globalStore = std::move(store);
    WriteGlobalStore();
    if (g_globalStore.Dirty()) {
        g_globalStore = std::move(previous);
        error = "Could not write the design library; retry";
        return false;
    }
    return true;
}

// Turns the feature off for this run (its buttons then do nothing) and says why, once.
void DesignOff(const char* why) {
    if (g_designLibOn.exchange(false))
        Log(LogLevel::Error, "ship_design_library", "%s; turned off until the game restarts", why);
}

void DesignNotice(std::string text) {
    g_designPanel.notice = std::move(text);
    g_designPanel.noticeAt = GetTickCount64();
}

void AcceptDesignNameImpl(const game::String* text) {
    g_designDialogError.clear();
    if (!text)
        return;
    const auto value = ReadField<game::String>(text);
    std::string name(value.size <= 4096 ? value.size : 0, '\0');
    if (!name.empty() && !CopyMemoryChecked(name.data(), value.data(), value.size))
        name.clear();
    if (!sdl::ValidName(name)) {
        g_designDialogError = "Enter a name of 1-128 characters";
        return;
    }
    auto design = g_designPending;
    design.name = name;
    auto library = g_designLibrary;
    library[name] = design;
    if (library.size() > sdl::kMaxDesigns) {
        g_designDialogError = "Design library is full";
        return;
    }
    std::string error;
    if (!WriteDesignLibrary(library, error)) {
        g_designDialogError = error;
        return;
    }
    const bool replaced = g_designLibrary.count(name) != 0;
    g_designLibrary.swap(library);
    g_designPanel.selected = name;
    g_designPanel.follow = true;
    DesignNotice((replaced ? "Replaced " : "Saved ") + sdl::Ellipsize(name, 30));
    Log(LogLevel::Info, "ship_design_library", "saved \"%s\" (%s, %zu sections)", name.c_str(), design.size.c_str(),
        design.sections.size());
}
void AcceptDesignName(void*, const game::String* text) {
    try {
        AcceptDesignNameImpl(text);
    } catch (...) {
        g_designDialogError = "Could not read the name; cancel and retry";
    }
}

void OpenDesignName(const sdl::Design& design) {
    if (g_starbaseDialog || g_designDialog)
        return;
    static const struct {
        uintptr_t manager;
        void (*invoke)(void*, const game::String*);
    } callbackVtable{0, AcceptDesignName};
    TradeTextCallback callback{reinterpret_cast<uintptr_t>(&callbackVtable) | 1};
    auto title = uinavigation::Text("Save design to the library (all games; same name replaces)");
    auto initial = uinavigation::Text(design.name);
    auto* memory = reinterpret_cast<void* (*)(size_t)>(g_starbaseDialogUi.allocate)(g_starbaseDialogUi.dialogSize);
    if (!memory)
        return;
    g_designPending = design;
    g_designDialogError.clear();
    g_designDialog =
        reinterpret_cast<uint8_t* (*)(void*, const game::String*, const game::String*, const TradeTextCallback*)>(
            g_starbaseDialogUi.dialogConstructor)(memory, &title, &initial, &callback);
    reinterpret_cast<int (*)(void*, void*)>(g_starbaseDialogUi.addUpdateable)(*g_starbaseDialogUi.idler,
                                                                              g_designDialog);
}

// --- Panel -----------------------------------------------------------------------------------------
void DesignShowPanel(bool open) {
    auto& p = g_designPanel;
    p.open = open && p.built;
    if (!p.built)
        return;
    for (int i = sdl::kSave; i < sdl::kRow0; ++i)
        StarbaseShowButton(g_designControls[i].button, p.open);
    for (int i = 0; i < sdl::kMaxRows; ++i)
        StarbaseShowButton(g_designControls[sdl::kRow0 + i].button,
                           p.open && i < p.geometry.rows && !p.rows[i].empty());
}

void DesignRefresh() {
    auto& p = g_designPanel;
    if (!p.built)
        return;
    auto* c = g_designControls;
    StarbaseText(c[sdl::kToggle].button, p.open ? "Design library (close)" : "Design library");
    if (!p.open) {
        DesignShowPanel(false);
        return;
    }
    auto page = sdl::BuildPage(g_designLibrary, p.selected, p.page, p.follow, p.geometry.rows, p.geometry.rowText);
    p.selected = page.selected;
    p.page = page.page;
    p.follow = false;
    for (int i = 0; i < sdl::kMaxRows; ++i) {
        const bool row = i < static_cast<int>(page.rows.size());
        p.rows[i] = row ? page.rows[i].name : std::string();
        if (row)
            StarbaseText(c[sdl::kRow0 + i].button, page.rows[i].label);
    }
    StarbaseText(c[sdl::kPage].button, page.pageLabel);
    const auto now = GetTickCount64();
    if (p.deleteName != p.selected || now - p.deleteAt > 4000)
        p.deleteName.clear();
    StarbaseText(c[sdl::kDelete].button, p.deleteName.empty() ? "Delete" : "Click again to delete");
    const bool notice = !p.notice.empty() && now - p.noticeAt < 6000;
    StarbaseText(c[sdl::kStatus].button, sdl::Ellipsize(notice               ? p.notice
                                                        : p.selected.empty() ? std::string("Save a design to start")
                                                                             : "Selected: " + p.selected,
                                                        p.geometry.statusText));
    DesignShowPanel(true);
}

void DesignLoadSelected(uint8_t* designer) {
    auto& p = g_designPanel;
    const auto found = g_designLibrary.find(p.selected);
    if (found == g_designLibrary.end()) {
        DesignNotice("Select a design first");
        return;
    }
    const auto& d = found->second;
    std::string fatal;
    const auto report = LoadIntoDesigner(designer, d, fatal);
    if (!fatal.empty()) {
        Log(LogLevel::Warn, "ship_design_library", "\"%s\" not loaded: %s", d.name.c_str(), fatal.c_str());
        DesignNotice("Not loaded; see the message");
        DesignPopup("Design not loaded", fatal);
        return;
    }
    Log(LogLevel::Info, "ship_design_library", "loaded \"%s\" (%s): %d parts placed, %d skipped", d.name.c_str(),
        d.size.c_str(), report.placed, report.skipped);
    for (const auto& problem : report.problems)
        Log(LogLevel::Info, "ship_design_library", "  skipped: %s", problem.c_str());
    if (report.problems.empty()) {
        DesignNotice("Loaded " + sdl::Ellipsize(d.name, 30) + "; Save it to use it");
    } else {
        DesignNotice("Loaded with " + std::to_string(report.skipped) + " parts left empty");
        DesignPopup("Design loaded with changes", sdl::ReportText(d.name, report));
    }
}

void DesignClickImpl(StarbaseControl* control) {
    using namespace uinavigation;
    auto& p = g_designPanel;
    auto* designer = control->view;
    if (!g_designLibOn || !p.built || !designer || designer != p.view ||
        Field<uint8_t*>(designer, 0x78) != control->root || g_starbaseDialog || g_designDialog ||
        !Method<BoolMethod>(designer, 0x70)(designer))
        return;
    const int action = control->action;
    if (action == sdl::kToggle) {
        p.open = !p.open;
        p.follow = true;
    } else if (!p.open) {
        return; // a click that raced the panel closing
    } else if (action == sdl::kClose) {
        p.open = false;
    } else if (action == sdl::kSave) {
        sdl::Design design;
        std::string error;
        if (StoreMultiplayer())
            DesignNotice("Saving is off in multiplayer");
        else if (!ReadOpenDesign(designer, design, error))
            DesignNotice(error);
        else
            OpenDesignName(design);
    } else if (action == sdl::kLoad) {
        DesignLoadSelected(designer);
    } else if (action == sdl::kDelete) {
        const auto now = GetTickCount64();
        if (p.selected.empty() || !g_designLibrary.count(p.selected)) {
            DesignNotice("No design selected");
        } else if (StoreMultiplayer()) {
            DesignNotice("Deleting is off in multiplayer");
        } else if (p.deleteName != p.selected || now - p.deleteAt > 4000) {
            p.deleteName = p.selected; // two clicks: the library is shared by every game
            p.deleteAt = now;
        } else {
            auto library = g_designLibrary;
            library.erase(p.selected);
            std::string error;
            if (WriteDesignLibrary(library, error)) {
                Log(LogLevel::Info, "ship_design_library", "deleted \"%s\"", p.selected.c_str());
                DesignNotice("Deleted " + sdl::Ellipsize(p.selected, 34));
                g_designLibrary.swap(library);
                p.follow = true;
            } else {
                DesignNotice(error);
            }
            p.deleteName.clear();
        }
    } else if (action == sdl::kPrev || action == sdl::kNext) {
        p.page = (std::max)(0, p.page + (action == sdl::kNext ? 1 : -1));
    } else if (action >= sdl::kRow0 && action < sdl::kControls && !p.rows[action - sdl::kRow0].empty()) {
        p.selected = p.rows[action - sdl::kRow0];
        p.deleteName.clear();
    }
    DesignRefresh();
}
void DesignClickCpp(StarbaseControl* control) {
    try {
        DesignClickImpl(control);
    } catch (const std::exception& e) {
        g_designLibOn = false;
        Log(LogLevel::Error, "ship_design_library", "%s; turned off", e.what());
    } catch (...) {
        DesignOff("error in the ship designer");
    }
}
void DesignClick(StarbaseControl* control, const void*) {
    __try {
        DesignClickCpp(control);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DesignOff("fault in the ship designer");
    }
}
const StarbaseControl::Callback kDesignObserver[] = {
    StarbaseIgnore, DesignClick,    StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore,
    StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore,
    StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore, StarbaseIgnore};
