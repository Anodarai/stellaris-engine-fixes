// Included by dllmain.cpp after starbasetemplates_runtime.inl, whose button cloning and placement
// (starbasetemplates_widgets.inl) and naming dialog (starbasetemplates_dialogs.inl) it shares.
// Everything runs on the game's main thread (clicks, the frame hook).
//
// The ship designer gets a "Design library" button. It opens a panel of buttons below it: Save current,
// Load selected, a paged list of saved designs, Delete (two clicks) and Close. Saved designs live in
// the global sefdata store (shipdesignlibrary::kStoreKey), so every game and empire sees them.
// Loading starts a new design of the saved hull the way the hull picker does, then places the saved
// sections, components and required components through the designer, checking each the way the
// designer does; whatever can't be placed is left empty and listed in a popup.
//
// The code is in parts this file includes: shipdesignlibrary_read.inl (reading the open design),
// shipdesignlibrary_load.inl (loading a saved one) and shipdesignlibrary_panel.inl (the panel and its
// clicks). This file keeps the state, the frame hook, adding the controls and the installer.
namespace sdl = shipdesignlibrary;
sdl::Locations g_designLib;
std::atomic<bool> g_designLibOn{false};
sdl::Library g_designLibrary; // main thread only; written to the global store under g_storeLock
StarbaseControl g_designControls[sdl::kControls];
struct DesignPanel {
    uint8_t* view = nullptr;                  // the CShipDesigner
    uint8_t* root = nullptr;                  // its window when the controls were made
    uintptr_t viewVtable = 0, rootVtable = 0; // to recognise a destroyed view or window
    bool built = false, open = false, follow = true, shown = false;
    int page = 0;
    sdl::Geometry geometry;
    std::string selected;
    std::string rows[sdl::kMaxRows];
    std::string deleteName;
    ULONGLONG deleteAt = 0;
    std::string notice;
    ULONGLONG noticeAt = 0;
};
DesignPanel g_designPanel;
sdl::Design g_designPending; // the design being named in the Save dialog
int g_designX = 445, g_designY = 0;

using starbasetemplates::ReadField;
using starbasetemplates::CopyMemoryChecked;

#include "shipdesignlibrary_read.inl"
#include "shipdesignlibrary_load.inl"
#include "shipdesignlibrary_panel.inl"

// Every frame: showing a window re-shows all of its children, including a closed panel's buttons,
// so the panel's visibility is applied again while the designer is shown.
void ShipDesignLibraryFrameCpp() {
    auto& p = g_designPanel;
    if (!g_designLibOn || !p.built || !p.view)
        return;
    // The designer and its window die with the game session (or a GUI reload) without telling us.
    // A destroyed object no longer has its class's vftable, so nothing below runs on a dead one;
    // the controls are made again when the new designer's window loads.
    uintptr_t viewTable = 0, rootTable = 0;
    uint8_t* root = nullptr;
    uint8_t shownByte = 0;
    if (!CopyMemoryChecked(&viewTable, p.view, sizeof(viewTable)) || viewTable != p.viewVtable ||
        !CopyMemoryChecked(&root, p.view + 0x78, sizeof(root)) || root != p.root ||
        !CopyMemoryChecked(&rootTable, root, sizeof(rootTable)) || rootTable != p.rootVtable ||
        !CopyMemoryChecked(&shownByte, root + starbasetemplates::kWindowShown, 1)) {
        p.built = false;
        p.view = nullptr;
        return;
    }
    const bool shown = shownByte != 0;
    // Applied when the designer appears and twice a second while it is shown; this also expires
    // notices and the Delete confirmation.
    static ULONGLONG refreshed = 0;
    const auto now = GetTickCount64();
    if (shown && (!p.shown || now - refreshed > 500)) {
        refreshed = now;
        DesignRefresh();
    }
    p.shown = shown;
}
void ShipDesignLibraryFrameGuarded() {
    try {
        ShipDesignLibraryFrameCpp();
    } catch (...) {
        DesignOff("error refreshing the library panel");
    }
}
void ShipDesignLibraryFrame() {
    __try {
        ShipDesignLibraryFrameGuarded();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DesignOff("fault refreshing the library panel");
    }
}

// The ship designer's choose_role button type: a text button in vanilla and in UI mods.
uint8_t* DesignButtonType(uint8_t* gui) {
    using namespace uinavigation;
    auto* window = gui ? GuiType(gui + 0x110, "ship_designer") : nullptr;
    auto* controls = window ? GuiType(window + 0x2d0, "controls") : nullptr;
    auto* type = controls ? GuiType(controls + 0x2d0, "choose_role") : nullptr;
    return type && Method<void (*)()>(type, 0x68) == reinterpret_cast<void (*)()>(g_navigation.locations.buttonFactory)
               ? type
               : nullptr;
}

void AddShipDesignControlsImpl(uint8_t* view, bool reloaded) {
    using namespace uinavigation;
    const auto& name = Field<game::String>(view, 0x48);
    if (!reloaded || !g_designLibOn || std::string_view(name.data(), name.size) != "ship_designer")
        return;
    if (Field<int>(view, sdl::kDesignerType) != 0)
        return; // the creature designer shares the window
    auto* root = Field<uint8_t*>(view, 0x78);
    auto* type = root ? DesignButtonType(Field<uint8_t*>(root, 0xb10)) : nullptr;
    if (!type) {
        Log(LogLevel::Warn, "ship_design_library", "the designer's button type wasn't found; no library button");
        return;
    }
    auto& p = g_designPanel;
    const std::string selected = p.selected; // a reload keeps the selection, nothing else
    p = {};
    p.selected = selected;
    p.view = view;
    p.root = root;
    p.viewVtable = Field<uintptr_t>(view, 0);
    p.rootVtable = Field<uintptr_t>(root, 0);
    Point size{1272, 642};
    if (g_windowGetSize)
        g_windowGetSize(root, &size);
    p.geometry = sdl::MakeGeometry(g_designX, g_designY, size.y);
    static const char* const names[sdl::kControls] = {
        "sef_design_library", "sef_design_save",   "sef_design_load",  "sef_design_prev",   "sef_design_page",
        "sef_design_next",    "sef_design_delete", "sef_design_close", "sef_design_status", "sef_design_row00",
        "sef_design_row01",   "sef_design_row02",  "sef_design_row03", "sef_design_row04",  "sef_design_row05",
        "sef_design_row06",   "sef_design_row07",  "sef_design_row08", "sef_design_row09",  "sef_design_row10",
        "sef_design_row11",   "sef_design_row12",  "sef_design_row13", "sef_design_row14",  "sef_design_row15"};
    static const char* const labels[sdl::kRow0] = {
        "Design library", "Save current", "Load selected", "<", "", ">", "Delete", "Close", ""};
    static_assert(sdl::kRow0 == 9 && sdl::kMaxRows == 16, "names and labels follow Control");
    for (int i = 0; i < sdl::kControls; ++i) {
        const auto rect = sdl::Layout(i, p.geometry);
        auto* button = StarbaseCreateButton(type, root, size, {rect.x, rect.y, rect.w, rect.h}, names[i],
                                            i < sdl::kRow0 ? labels[i] : "", g_designControls[i], view, root, i,
                                            nullptr, 0, kDesignObserver);
        if (!button) {
            Log(LogLevel::Warn, "ship_design_library", "could not create the library buttons");
            return;
        }
        if (i != sdl::kToggle)
            StarbaseShowButton(button, false);
    }
    p.built = true;
    Log(LogLevel::Info, "ship_design_library", "library button at %d,%d in the %dx%d designer, %d rows, %zu designs",
        p.geometry.x, p.geometry.y, size.x, size.y, p.geometry.rows, g_designLibrary.size());
}
void AddShipDesignControlsCpp(uint8_t* view, bool reloaded) {
    try {
        AddShipDesignControlsImpl(view, reloaded);
    } catch (...) {
        DesignOff("error adding the library button");
    }
}
void AddShipDesignControls(uint8_t* view, bool reloaded) {
    __try {
        AddShipDesignControlsCpp(view, reloaded);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DesignOff("fault adding the library button");
    }
}

// Needs the starbase templates' button helpers and naming dialog (InstallStarbaseTemplates, run first)
// and the script storage's global store.
void InstallShipDesignLibrary(HMODULE self, const locate::Image& image) {
    const auto ini = IniPath(self);
    if (!IniOnLogged(self, "interface", "ship_design_library", true, "ship_design_library"))
        return;
    g_designX = std::clamp(
        static_cast<int>(GetPrivateProfileIntA("interface", "ship_design_library_x", 445, ini.c_str())), 0, 3840);
    g_designY = std::clamp(
        static_cast<int>(GetPrivateProfileIntA("interface", "ship_design_library_y", 0, ini.c_str())), 0, 2160);
    if (!g_storeOn || !g_origNavigationReload || !g_origCheckTick) {
        Log(LogLevel::Warn, "ship_design_library", "needs [script_storage] enabled=1 and the GUI hooks; off");
        return;
    }
    if (!g_starbaseButtonText || !g_starbaseButtonSize || !g_starbaseAcceptOriginal ||
        !g_starbaseDialogUi.dialogConstructor || !g_starbaseTemplateReader.construction.allocate ||
        !g_starbaseTemplateReader.selection.gameState || !g_str.buildString || !g_str.gameFree) {
        Log(LogLevel::Warn, "ship_design_library", "needs starbase_templates=1 (its buttons and name dialog); off");
        return;
    }
    std::string error;
    if (!sdl::Locate(image, g_designLib, error)) {
        Log(LogLevel::Warn, "ship_design_library", "%s; off", error.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> storeLock(g_storeLock);
        const auto* payload = StoreFor(pdxstore::Which::Global).Get(sdl::kStoreKey);
        if (payload && !sdl::Deserialize(*payload, g_designLibrary)) {
            Log(LogLevel::Error, "ship_design_library",
                "the stored design library is invalid; off, and the stored data is kept");
            return;
        }
    }
    g_designLibOn = true;
    Log(LogLevel::Info, "ship_design_library",
        "installed the ship designer's Design library button (%zu saved designs)", g_designLibrary.size());
}
