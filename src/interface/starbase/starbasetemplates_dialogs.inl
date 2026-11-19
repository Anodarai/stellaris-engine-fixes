// Part of starbasetemplates_runtime.inl: the name and share-code dialogs. Saving a template under a
// name, Copy code and Paste code (starbasetemplates_share.h, through the Windows clipboard), the
// dialog's accept and destroy hooks, and deleting a template.

void AcceptStarbaseNameImpl(const game::String* text) {
    if (!g_starbaseDialog || !text)
        return;
    g_starbaseDialogError.clear();
    const auto value = starbasetemplates::ReadField<game::String>(text);
    if (!value.size || value.size > 128) {
        g_starbaseDialogError = "Name must contain 1-128 bytes";
        return;
    }
    std::string name(value.size, '\0');
    if (!starbasetemplates::CopyMemoryChecked(name.data(), value.data(), value.size))
        return;
    if (name.find_first_of("\r\n\t") != std::string::npos || name.find('\0') != std::string::npos ||
        name.find_first_not_of(' ') == std::string::npos) {
        g_starbaseDialogError = "Enter a template name";
        return;
    }
    if (StarbaseLocalCountry() != g_starbaseDialogOwner) {
        g_starbaseDialogError = "Game or owner changed; cancel this dialog";
        return;
    }
    starbasetemplates::State current;
    if (g_starbaseDialogDraft) {
        current.id = UINT32_MAX; // an editor draft belongs to no starbase
        current.owner = g_starbaseDialogOwner;
    } else {
        current = g_starbaseTemplateReader.Snapshot(g_starbaseDialogId);
        if (!current.valid || current.owner != g_starbaseDialogOwner) {
            g_starbaseDialogError = "Starbase is no longer available";
            return;
        }
    }
    std::lock_guard<std::mutex> storeLock(g_storeLock);
    std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
    if (g_starbaseDialogGeneration != g_starbaseSaveGeneration) {
        g_starbaseDialogError = "Game changed; cancel and save again";
        return;
    }
    auto& library = g_starbaseDialogGlobal ? g_starbaseGlobalLibrary : g_starbaseLocalLibrary;
    auto candidate = library;
    starbasetemplates::State saved = current;
    saved.built = g_starbaseDialogLayout; // Save the layout from the moment Save was clicked.
    if (!candidate.Save(name, saved) || candidate.library.size() > 4096) {
        g_starbaseDialogError = "Cannot save template";
        return;
    }
    auto store = g_starbaseDialogGlobal ? g_globalStore : g_saveStore;
    if (!store.Set(kStarbaseLibraryKey, candidate.Serialize(true))) {
        g_starbaseDialogError = "Template library is full";
        return;
    }
    if (g_starbaseDialogGlobal) {
        auto previous = g_globalStore;
        g_globalStore = std::move(store);
        WriteGlobalStore();
        if (g_globalStore.Dirty()) {
            g_globalStore = std::move(previous);
            g_starbaseDialogError = "Could not write template library; retry";
            return;
        }
    } else {
        // Persist with the next successful game save, using its accepted snapshot.
        g_saveStore = std::move(store);
    }
    library.library.swap(candidate.library);
    RebuildStarbaseLibrary();
    g_starbaseSelected = name;
    if (g_starbaseDialogView)
        g_starbaseView = g_starbaseDialogView; // the other view may have refreshed since
    g_starbaseView->panel.follow = true;
    if (g_starbaseDialogDraft)
        g_starbaseView->editor.mode = StarbaseEditorState::Manager; // back to the list, on the saved entry
    StarbaseNotice((g_starbaseDialogGlobal ? "Saved global " : "Saved ") + starbasetemplates::Ellipsize(name, 30));
}

// ---- Share codes (starbasetemplates_share.h): Copy code and Paste code ----

#pragma comment(lib, "user32.lib")
// The Windows clipboard as UTF-8. The game's own edit boxes paste from it too (SDL reads
// CF_UNICODETEXT), so a code copied here can also be pasted into the code dialog with Ctrl+V.
bool StarbaseClipboardSet(const std::string& text) {
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wide <= 0 || !OpenClipboard(nullptr))
        return false;
    bool ok = false;
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(wide) + 1) * sizeof(wchar_t))) {
        if (auto* buffer = static_cast<wchar_t*>(GlobalLock(memory))) {
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buffer, wide);
            buffer[wide] = 0;
            GlobalUnlock(memory);
            ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
        }
        if (!ok)
            GlobalFree(memory); // the clipboard owns it once SetClipboardData succeeds
    }
    CloseClipboard();
    return ok;
}
// The clipboard's text as UTF-8, when it has some of at most `limit` bytes.
bool StarbaseClipboardGet(std::string& text, size_t limit) {
    text.clear();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr))
        return false;
    bool ok = false;
    if (HANDLE memory = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* buffer = static_cast<const wchar_t*>(GlobalLock(memory))) {
            const size_t chars = wcsnlen(buffer, GlobalSize(memory) / sizeof(wchar_t));
            const int bytes =
                chars <= limit
                    ? WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(chars), nullptr, 0, nullptr, nullptr)
                    : -1;
            if (bytes >= 0 && static_cast<size_t>(bytes) <= limit) {
                text.resize(static_cast<size_t>(bytes));
                if (bytes > 0)
                    WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(chars), text.data(), bytes, nullptr,
                                        nullptr);
                ok = true;
            }
            GlobalUnlock(memory);
        }
    }
    CloseClipboard();
    return ok;
}

// Copies a layout's share code to the clipboard and says so on the status line.
void StarbaseCopyCode(const starbasetemplates::Layout& layout, const std::string& name) {
    const auto code = starbasetemplates::EncodeShare(layout, name);
    if (code.empty())
        StarbaseNotice("This template has no code");
    else if (!StarbaseClipboardSet(code))
        StarbaseNotice("Could not use the clipboard; retry");
    else {
        StarbaseNotice("Code copied (" + std::to_string(code.size()) + " characters)");
        Log(LogLevel::Info, "starbase_templates", "share code copied: %s", code.c_str());
    }
}

// Opens a share code in the editor as a new template. Saving it there puts it in a library.
void StarbaseLoadShared(const starbasetemplates::SharedTemplate& shared) {
    using namespace starbasetemplates;
    auto& e = g_starbaseView->editor;
    const int own = KindOfLevel(StarbaseKinds(), shared.layout.level);
    const int kind = own >= 0 ? own : std::clamp(g_starbaseView->kind, 0, kStationKinds - 1);
    if (!StarbaseEditorLoad(kind)) {
        StarbaseNotice("Cannot read the game's station definitions");
        return;
    }
    e.draft = ImportDraft(shared, StarbaseKinds(), kind);
    e.page = 0;
    e.mode = StarbaseEditorState::Editor;
    g_starbaseView->panel.dirty = true;
    const int missing = MissingParts(e.draft.layout, e.levels, e.modules, e.buildings);
    StarbaseNotice(missing ? "Code loaded; " + std::to_string(missing) +
                                 (missing == 1 ? " part is" : " parts are") + " not in this game"
                           : "Code loaded: " + LayoutSummary(e.draft.layout));
    Log(LogLevel::Info, "starbase_templates", "share code loaded: %s, %d missing", e.draft.layout.level.c_str(),
        missing);
}

// Paste code: straight from the clipboard when it holds a code, else a dialog to paste one into.
void OpenStarbaseCode(const std::string& initial);
void StarbasePasteCode() {
    std::string text, error;
    starbasetemplates::SharedTemplate shared;
    const bool clipboard = StarbaseClipboardGet(text, starbasetemplates::kShareMaxText);
    if (clipboard && starbasetemplates::DecodeShare(text, shared, error)) {
        StarbaseLoadShared(shared);
        return;
    }
    // The dialog shows one line: a long or multi-line text from the clipboard starts it empty.
    const bool keep = clipboard && text.size() <= 1000 && text.find_first_of("\r\n") == std::string::npos;
    OpenStarbaseCode(keep ? text : std::string());
}

void AcceptStarbaseCodeImpl(const game::String* text) {
    if (!g_starbaseDialog || !text)
        return;
    g_starbaseDialogError.clear();
    const auto value = starbasetemplates::ReadField<game::String>(text);
    if (value.size > starbasetemplates::kShareMaxText) {
        g_starbaseDialogError = "Text is too long for a template code";
        return;
    }
    std::string code(value.size, '\0');
    if (!starbasetemplates::CopyMemoryChecked(code.data(), value.data(), value.size))
        return;
    starbasetemplates::SharedTemplate shared;
    std::string error;
    if (!starbasetemplates::DecodeShare(code, shared, error)) {
        g_starbaseDialogError = error;
        return;
    }
    if (g_starbaseDialogView)
        g_starbaseView = g_starbaseDialogView; // the other view may have refreshed since
    StarbaseLoadShared(shared);
}

void AcceptStarbaseName(void*, const game::String* text) {
    try {
        if (g_starbaseDialogCode)
            AcceptStarbaseCodeImpl(text);
        else
            AcceptStarbaseNameImpl(text);
    } catch (...) {
        g_starbaseDialogError = "Unable to read starbase; cancel and retry";
    }
}

void StarbaseDialogAcceptHook(uint8_t* dialog) {
    g_starbaseAcceptOriginal(dialog);
    // A refused name keeps the dialog open with the reason as its title.
    const std::string* error = dialog == g_starbaseDialog ? &g_starbaseDialogError
                               : dialog == g_designDialog ? &g_designDialogError
                                                          : nullptr;
    if (error && !error->empty()) {
        dialog[g_starbaseDialogUi.dialogFinishedOffset] = false;
        auto title = uinavigation::Text(*error);
        reinterpret_cast<TradeDialogSetTitle>(g_starbaseDialogUi.setTitle)(dialog, &title);
    }
}
void* StarbaseDialogDestroyHook(void* dialog, unsigned flags) {
    if (dialog == g_starbaseDialog) {
        g_starbaseDialog = nullptr;
        g_starbaseDialogId = UINT32_MAX;
    }
    if (dialog == g_designDialog)
        g_designDialog = nullptr;
    return g_starbaseDestroyOriginal(dialog, flags);
}

// `draft` saves state.built as given (the editor's layout; state.id names no starbase) under a
// name that starts as `suggested`.
void OpenStarbaseName(const starbasetemplates::State& state, bool global, bool draft = false,
                      const std::string& suggested = {}) {
    if (g_starbaseDialog || g_designDialog)
        return;
    static const struct {
        uintptr_t manager;
        void (*invoke)(void*, const game::String*);
    } callbackVtable{0, AcceptStarbaseName};
    TradeTextCallback callback{reinterpret_cast<uintptr_t>(&callbackVtable) | 1};
    auto title = uinavigation::Text(global ? "Save GLOBAL template (same name replaces)"
                                           : "Save template with this game (save game to keep)");
    g_starbaseDialogGlobal = global;
    g_starbaseDialogGeneration = g_starbaseSaveGeneration;
    const std::string initial =
        !suggested.empty() ? suggested : "Template " + std::to_string(g_starbaseTemplates.library.size() + 1);
    auto initialText = uinavigation::Text(initial);
    auto* memory = reinterpret_cast<void* (*)(size_t)>(g_starbaseDialogUi.allocate)(g_starbaseDialogUi.dialogSize);
    if (!memory)
        return;
    g_starbaseDialogId = state.id;
    g_starbaseDialogOwner = state.owner;
    g_starbaseDialogLayout = state.built;
    g_starbaseDialogDraft = draft;
    g_starbaseDialogCode = false;
    g_starbaseDialogView = g_starbaseView;
    g_starbaseDialogError.clear();
    g_starbaseDialog =
        reinterpret_cast<uint8_t* (*)(void*, const game::String*, const game::String*, const TradeTextCallback*)>(
            g_starbaseDialogUi.dialogConstructor)(memory, &title, &initialText, &callback);
    reinterpret_cast<int (*)(void*, void*)>(g_starbaseDialogUi.addUpdateable)(*g_starbaseDialogUi.idler,
                                                                              g_starbaseDialog);
}

// The same native dialog, asking for a share code. OK loads it into the editor.
void OpenStarbaseCode(const std::string& initial) {
    if (g_starbaseDialog || g_designDialog)
        return;
    static const struct {
        uintptr_t manager;
        void (*invoke)(void*, const game::String*);
    } callbackVtable{0, AcceptStarbaseName};
    TradeTextCallback callback{reinterpret_cast<uintptr_t>(&callbackVtable) | 1};
    auto title = uinavigation::Text("Paste a template code (Ctrl+V), then OK");
    auto initialText = uinavigation::Text(initial);
    auto* memory = reinterpret_cast<void* (*)(size_t)>(g_starbaseDialogUi.allocate)(g_starbaseDialogUi.dialogSize);
    if (!memory)
        return;
    g_starbaseDialogId = UINT32_MAX;
    g_starbaseDialogOwner = StarbaseLocalCountry();
    g_starbaseDialogGeneration = g_starbaseSaveGeneration;
    g_starbaseDialogDraft = false;
    g_starbaseDialogCode = true;
    g_starbaseDialogView = g_starbaseView;
    g_starbaseDialogError.clear();
    g_starbaseDialog =
        reinterpret_cast<uint8_t* (*)(void*, const game::String*, const game::String*, const TradeTextCallback*)>(
            g_starbaseDialogUi.dialogConstructor)(memory, &title, &initialText, &callback);
    reinterpret_cast<int (*)(void*, void*)>(g_starbaseDialogUi.addUpdateable)(*g_starbaseDialogUi.idler,
                                                                              g_starbaseDialog);
}

// Removes a template from the scope it lives in: the save-specific one when both have the
// name, which reveals the global one again. Globals are written at once, like Save global.
bool DeleteStarbaseTemplate(const std::string& name, std::string& error) {
    std::lock_guard<std::mutex> storeLock(g_storeLock);
    std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
    const bool local = g_starbaseLocalLibrary.library.count(name) != 0;
    auto& library = local ? g_starbaseLocalLibrary : g_starbaseGlobalLibrary;
    auto candidate = library;
    if (!candidate.Remove(name)) {
        error = "Template no longer exists";
        return false;
    }
    auto store = local ? g_saveStore : g_globalStore;
    if (local && candidate.library.empty())
        store.Remove(kStarbaseLibraryKey);
    else if (!store.Set(kStarbaseLibraryKey, candidate.Serialize(true))) {
        error = "Cannot update template library";
        return false;
    }
    if (local) {
        g_saveStore = std::move(store); // persists with the next successful game save
    } else {
        auto previous = g_globalStore;
        g_globalStore = std::move(store);
        WriteGlobalStore();
        if (g_globalStore.Dirty()) {
            g_globalStore = std::move(previous);
            error = "Could not write template library; retry";
            return false;
        }
    }
    library.library.swap(candidate.library);
    RebuildStarbaseLibrary();
    return true;
}
