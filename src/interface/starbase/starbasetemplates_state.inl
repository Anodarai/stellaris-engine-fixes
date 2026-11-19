// Part of starbasetemplates_runtime.inl: the starbase features' shared state. The dev-build
// timers, StarbaseDisable, the control ids and their objects, the panel and editor state of each
// station view, the content mod's skin, the dialogs' state, and small readers of the game (the local
// country, the station kinds, today's date, a starbase's owner).

// Dev-build timings (devstats.h) of what the starbase features add to the game's own work, logged
// by LogStarbasePerf. Empty in a release build.
devstats::Timer g_perfFrame;       // StarbaseTemplateFrame, every frame (mostly the 100 ms throttle)
devstats::Timer g_perfFramePass;   // the passes that got past the throttle
devstats::Timer g_perfJobStep;     // of those, the staged-construction step for one job
devstats::Timer g_perfView;        // our part of the starbase and waystation views' update, every call
devstats::Timer g_perfViewRefresh; // of those, the 500 ms snapshot and control refresh
devstats::Timer g_perfReload;      // AddStarbaseControls, every GUI view reload
devstats::Timer g_perfCalcGame;    // the game's CalcBuildLocations itself, every call
devstats::Timer g_perfCalcFilter;  // our filter, the calls made with a shipyard unchecked
devstats::Counter g_perfCalcOffMain{0}, g_perfCalcFiltered{0};

// Turns the starbase features off after a fault or an exception, with one log line.
void StarbaseDisable(const char* where) {
    if (g_starbaseTemplatesOn.exchange(false))
        Log(LogLevel::Error, "starbase_templates", "%s; disabled", where);
}

struct StarbaseControl {
    using Callback = void (*)(StarbaseControl*, const void*);
    const Callback* vtable = nullptr;
    uint8_t* view = nullptr;
    uint8_t* root = nullptr;
    uint8_t* button = nullptr;
    int action = 0;
};
// The Starbase tab gets one Templates button. It opens the native starbase side view (the
// panel modules and buildings use) in an idle mode and fills it with these controls.
constexpr int kStarbaseToggleAction = starbasetemplates::kPanelControls;
// The defenses tab gets an Auto-upgrade toggle for the selected starbase's defense platforms.
constexpr int kAutoUpgradeAction = kStarbaseToggleAction + 1;
// The Shipyard tab gets a checkbox: may automatic fleet reinforcement build at this shipyard
// (shipyardreinforce_runtime.inl, included further down).
constexpr int kReinforceAction = kAutoUpgradeAction + 1;
void ReinforceClick(uint32_t id);
void ReinforceRefresh(uint8_t* view);
void ReinforcePrune();
StarbaseControl g_autoUpgradeToggle;
starbasetemplates::PlatformUpgradeLocations g_platformUpgrade;
ULONGLONG g_autoUpgradeRefusedAt = 0;   // last click that couldn't toggle it
uint8_t* g_buttonSetAlpha = nullptr;    // CButtonStandard::SetAlpha(float)
uint8_t* g_buttonSetModulate = nullptr; // CButtonStandard::SetModulateColor(const CColor*)
struct StarbasePanel {
    uint8_t* side = nullptr;   // CStarbaseSideView that owns the window
    uint8_t* window = nullptr; // its CContainerWindow, which holds our buttons
    bool built = false, open = false, follow = true, dirty = false;
    bool slotRows = false; // the row buttons are at the editor's and picker's size
    int page = 0;
    starbasetemplates::PanelGeometry geometry; // from the side view's size when built
    std::string rows[starbasetemplates::kPanelMaxRows];
    std::string deleteName;
    ULONGLONG deleteAt = 0;
    std::string notice;
    ULONGLONG noticeAt = 0;
    std::string title; // what the side view's title shows; empty after the native opener reset it
    ULONGLONG refreshed = 0;
};
// The template editor (starbasetemplates_editor.h) shows in the same panel, with the same
// buttons relabelled: the draft's level and slots, or the picker for one slot. The draft
// outlives the panel closing, so a look at the native module list doesn't lose it. Each view
// has its own: a starbase draft doesn't show in the waystation view's panel.
struct StarbaseEditorState {
    enum Mode { Manager, Editor, Picker } mode = Manager;
    starbasetemplates::Draft draft;
    std::vector<starbasetemplates::LevelInfo> levels;          // the normal chain, as loaded
    std::vector<starbasetemplates::Choice> modules, buildings; // every definition, as loaded
    starbasetemplates::CapacityMemory capacity;                // slots real starbases showed
    int page = 0;
    starbasetemplates::Kind pickKind = starbasetemplates::Kind::Module;
    int pickSlot = 0, pickPage = 0;
    bool pickFollow = true, thisKindOnly = true;
    std::vector<starbasetemplates::EditorRow> rows; // what the row buttons show, per mode
    std::vector<starbasetemplates::PickerRow> picks;
};
// One set of controls per view that shows stations: the starbase view (starbases, orbital rings
// and deep space citadels) and the waystation view. Each view has its own side view, so its own
// panel. `kind` is the selected station's kind: the templates listed and what New makes.
struct StarbaseViewSlot {
    StarbaseControl toggle;
    StarbaseControl controls[starbasetemplates::kPanelControls];
    // One icon per row, over the row button's left end: the module or building an editor slot
    // holds or a picker entry is. It clicks like its row. iconDef is the definition it shows.
    StarbaseControl icons[starbasetemplates::kPanelMaxRows];
    uint8_t* iconDef[starbasetemplates::kPanelMaxRows] = {};
    StarbaseControl background; // the list's backdrop, only with the content mod's look
    StarbasePanel panel;
    StarbaseEditorState editor;
    int kind = starbasetemplates::kStationStarbase;
};
StarbaseViewSlot g_starbaseViews[2];
StarbaseViewSlot* g_starbaseView = &g_starbaseViews[0]; // the one being refreshed or clicked
starbasetemplates::KindChains g_starbaseKinds;          // each kind's levels; read through StarbaseKinds
StarbaseViewSlot* StarbaseSlotOfView(const uint8_t* view) {
    for (auto& slot : g_starbaseViews)
        if (view && slot.toggle.view == view)
            return &slot;
    return nullptr;
}
StarbaseViewSlot* StarbaseSlotOfControl(const StarbaseControl* control) {
    for (auto& slot : g_starbaseViews)
        if (control == &slot.toggle ||
            (control >= slot.controls && control < slot.controls + starbasetemplates::kPanelControls) ||
            (control >= slot.icons && control < slot.icons + starbasetemplates::kPanelMaxRows))
            return &slot;
    return &g_starbaseViews[0]; // the Defenses and Shipyard tab controls: starbase view only
}
uint8_t* g_starbaseButtonType = nullptr; // design_info/retrofit, cloned for every control
// The panel's look. Without the content mod (mods/sef_starbase_templates_ui) every control is a
// clone of design_info/retrofit. The mod's interface/*.gui defines the window type
// kStarbaseSkinWindow with one button type per role; a role it leaves out keeps retrofit, and
// without a "background" type there is no backdrop. Read again on every starbase_view reload.
enum StarbaseSkinRole {
    kSkinAction,
    kSkinPrimary,
    kSkinPager,
    kSkinLabel,
    kSkinRow,
    kSkinIcon,
    kSkinBackground,
    kSkinRoles
};
constexpr const char* kStarbaseSkinWindow = "sef_starbase_templates_skin";
constexpr const char* kStarbaseSkinNames[kSkinRoles] = {"action", "primary", "pager",     "label",
                                                        "row",    "icon",    "background"};
struct StarbaseSkin {
    uint8_t* types[kSkinRoles] = {}; // retrofit where the mod has no type (background: none)
    bool on = false;                 // the mod's window type was found
    bool rows = false;               // rows use its "row" type: left-aligned, so indented
};
StarbaseSkin g_starbaseSkin;
void (*g_sideShowForSlot)(uint8_t*, uint8_t, int) = nullptr;
void (*g_sideHide)(uint8_t*) = nullptr;
void (*g_textChange)(uint8_t*, const char*) = nullptr;
uint8_t* g_buttonMoveTo = nullptr;                // CButtonStandard::MoveTo, to recognise cloned buttons
starbasetemplates::IconLocations g_starbaseIcons; // apply == nullptr: no icons
uinavigation::Point* (*g_windowGetSize)(uint8_t*, uinavigation::Point*) = nullptr;
void StarbaseNotice(std::string text) {
    g_starbaseView->panel.notice = std::move(text);
    g_starbaseView->panel.noticeAt = GetTickCount64();
    g_starbaseView->panel.dirty = true;
}
uishortcuts::TradeLocations g_starbaseDialogUi;
uint8_t* g_starbaseButtonText = nullptr;
uint8_t* g_starbaseButtonSize = nullptr;
uint8_t* g_starbaseIsDesignable = nullptr;
void (*g_starbaseViewUpdate)(uint8_t*) = nullptr;
void (*g_waystationViewUpdate)(uint8_t*) = nullptr; // null: no Templates button on waystations
uint8_t* g_starbaseDialog = nullptr;
// The ship design library (shipdesignlibrary_runtime.inl) shares the naming dialog and its hooks.
uint8_t* g_designDialog = nullptr;
std::string g_designDialogError;
uint32_t g_starbaseDialogId = UINT32_MAX;
uint32_t g_starbaseDialogOwner = UINT32_MAX;
bool g_starbaseDialogGlobal = false;
bool g_starbaseDialogDraft = false;               // saving the editor's draft, not the selected starbase
bool g_starbaseDialogCode = false;                // reading a share code, not a template name
StarbaseViewSlot* g_starbaseDialogView = nullptr; // the view whose panel opened the dialog
uint64_t g_starbaseDialogGeneration = 0;
starbasetemplates::Layout g_starbaseDialogLayout;
std::string g_starbaseSelected;
std::string g_starbaseDialogError;
TradeUiAction g_starbaseAcceptOriginal = nullptr;
TradeUiDestroy g_starbaseDestroyOriginal = nullptr;
std::map<uint32_t, ULONGLONG> g_starbasePostedAt;
ULONGLONG g_starbaseLastPass = 0;
uint32_t g_starbaseLastId = 0;

uint32_t StarbaseLocalCountry() {
    if (!g_starbaseTemplatesOn || StoreMultiplayer())
        return UINT32_MAX;
    const auto& s = g_starbaseTemplateReader.selection;
    auto* state = starbasetemplates::ReadField<uint8_t*>(s.gameState);
    if (!state)
        return UINT32_MAX;
    auto* country = starbasetemplates::CallOwner(s.observedCountry, state);
    return country ? starbasetemplates::ReadField<uint32_t>(country, 0x20) : UINT32_MAX;
}

// Each kind's levels. They come from the game's script, which is loaded once, so they are read
// until every kind has a chain and kept from then on (a mod that removes a kind keeps the scans).
const starbasetemplates::KindChains& StarbaseKinds() {
    for (const auto& chain : g_starbaseKinds)
        if (chain.empty()) {
            g_starbaseKinds = g_starbaseTemplateReader.Kinds();
            break;
        }
    return g_starbaseKinds;
}

bool StarbaseSupported(const starbasetemplates::State& s) {
    // The four kinds with templates: a level on the starbase, orbital ring, deep space citadel
    // or waystation chain, modded successors included. Other stations (arkships, enclaves,
    // juggernauts) are left alone, and so is anything designable that isn't a citadel.
    auto* base = starbasetemplates::Reference(g_starbaseTemplateReader.selection.starbaseDatabase, s.id,
                                              g_starbaseTemplateReader.selection.objectReference);
    if (!base)
        return false;
    const int kind = starbasetemplates::KindOfLevel(StarbaseKinds(), s.built.level);
    if (kind < 0)
        return false;
    return kind == starbasetemplates::kStationCitadel ||
           !starbasetemplates::CallOccupation(g_starbaseIsDesignable, base);
}

// Takes the view's kind from the selected station. A view
// with no station of a known kind selected keeps its own: waystation or starbase.
void StarbaseUpdateKind(const starbasetemplates::State& state) {
    using namespace starbasetemplates;
    const int kind = state.valid ? KindOfLevel(StarbaseKinds(), state.built.level) : -1;
    g_starbaseView->kind = kind >= 0                               ? kind
                           : g_starbaseView == &g_starbaseViews[1] ? kStationWaystation
                                                                   : kStationStarbase;
}

starbasetemplates::Submission StarbaseSubmitGuarded(const starbasetemplates::CommandApi& api, uint32_t owner,
                                                    uint8_t* base, const starbasetemplates::Order& order,
                                                    uint8_t* definition, bool post) {
    __try {
        return starbasetemplates::Submit(api, owner, base, order, definition, post);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_starbaseTemplatesOn = false;
        Log(LogLevel::Error, "starbase_templates", "native construction fault; disabled");
        return starbasetemplates::Submission::Invalid;
    }
}

// Today's game day number, or -1 before a game is loaded or on a fault.
int64_t StarbaseToday() {
    uint8_t* state = g_bench.gameState ? *g_bench.gameState : nullptr;
    int32_t hours = 0;
    if (!state || !ReadInt(state + g_bench.stateDate, &hours) || hours < 0)
        return -1;
    return hours / 24;
}

// The starbase's owner id, or UINT32_MAX when the base is gone. Reads no layout or queue.
uint32_t StarbaseOwner(uint32_t id) {
    const auto& r = g_starbaseTemplateReader;
    auto* base = starbasetemplates::Reference(r.selection.starbaseDatabase, id, r.selection.objectReference);
    auto* owner = base ? starbasetemplates::CallOwner(r.fields.owner, base) : nullptr;
    return owner ? starbasetemplates::ReadField<uint32_t>(owner, 0x20) : UINT32_MAX;
}
