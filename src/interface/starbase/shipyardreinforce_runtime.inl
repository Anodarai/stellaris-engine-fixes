// Included by starbasetemplates_runtime.inl. The Shipyard tab's "Allow automatic fleet reinforcement
// from this shipyard" checkbox, and the hook that keeps unchecked shipyards out of the game's choice.
//
// The game works out reinforcements in CFleetTemplateManager::CalcShipsToReinforceInternal, for a
// fleet's Reinforce button, the fleet manager's Reinforce All, their tooltips and the commands they
// post. It hands CalcBuildLocations the country's construction queues; the hook passes a copy without
// the ships queues of unchecked starbases. That can run on AI threads too, so the set has its own lock.
shipyardreinforce::Locations g_reinforce;
using CalcBuildLocationsFn = void (*)(void*, void*, const uint8_t*, void*, const void*);
CalcBuildLocationsFn g_calcBuildLocations = nullptr; // trampoline
std::atomic<bool> g_reinforceFilterLogged{false};

// The starbase's ships queue id (CStarbase::GetShipsBuildQueueRef), or UINT32_MAX on a fault.
uint32_t ReinforceShipsQueue(uint8_t* base) {
    __try {
        uint32_t queue = UINT32_MAX;
        reinterpret_cast<uint32_t* (*)(uint8_t*, uint32_t*)>(g_reinforce.shipsQueueRef)(base, &queue);
        return queue;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return UINT32_MAX;
    }
}

// The ships queues of the unchecked starbases that still belong to whoever unchecked them.
bool ReinforceExcludedQueues(std::vector<uint32_t>& queues) {
    std::vector<std::pair<uint32_t, uint32_t>> bases;
    {
        std::lock_guard<std::mutex> lock(g_noReinforceLock);
        bases.assign(g_noReinforce.bases.begin(), g_noReinforce.bases.end());
    }
    const auto& r = g_starbaseTemplateReader;
    try {
        for (const auto& [id, owner] : bases) {
            auto* base = starbasetemplates::Reference(r.selection.starbaseDatabase, id, r.selection.objectReference);
            auto* country = base ? starbasetemplates::CallOwner(r.fields.owner, base) : nullptr;
            if (!country || starbasetemplates::ReadField<uint32_t>(country, 0x20) != owner)
                continue;
            const uint32_t queue = ReinforceShipsQueue(base);
            if (queue != UINT32_MAX)
                queues.push_back(queue);
        }
    } catch (...) {
        return false;
    }
    return true;
}

// Fills `header` (a CPdxArray header copied from `queues`) to point at `kept`, the allowed entries.
// False when nothing is left out, or on any fault: the game then gets its own list.
bool ReinforceFilter(const uint8_t* queues, uint8_t* header, std::vector<uint8_t>& kept) {
    using namespace shipyardreinforce;
    std::vector<uint32_t> excluded;
    if (!ReinforceExcludedQueues(excluded) || excluded.empty())
        return false;
    if (!starbasetemplates::CopyMemoryChecked(header, queues, kQueuesHeader))
        return false;
    uint8_t* data;
    int32_t count;
    memcpy(&data, header + kQueuesData, sizeof(data));
    memcpy(&count, header + kQueuesCount, sizeof(count));
    if (count <= 0 || count > 65536 || !data)
        return false;
    std::vector<uint8_t> source(size_t(count) * kQueueEntrySize);
    if (!starbasetemplates::CopyMemoryChecked(source.data(), data, source.size()))
        return false;
    kept.resize(source.size());
    const auto left = KeepAllowed(source.data(), size_t(count), excluded.data(), excluded.size(), kept.data());
    if (left == size_t(count))
        return false;
    auto* keptData = kept.data();
    const int32_t keptCount = static_cast<int32_t>(left);
    memcpy(header + kQueuesData, &keptData, sizeof(keptData));
    memcpy(header + kQueuesCount, &keptCount, sizeof(keptCount));
    memcpy(header + kQueuesCount - 4, &keptCount, sizeof(keptCount)); // capacity, never read
    if (!g_reinforceFilterLogged.exchange(true))
        Log(LogLevel::Info, "shipyard_reinforce", "reinforcement: %d of %d shipyards left out (logged once)",
            count - keptCount, count);
    return true;
}

void CalcBuildLocationsHook(void* self, void* out, const uint8_t* queues, void* fleet, const void* designs) {
    if constexpr (devstats::kOn)
        if (GetCurrentThreadId() != g_mainThread)
            g_perfCalcOffMain.fetch_add(1, std::memory_order_relaxed);
    if (!g_reinforceOn || !g_starbaseTemplatesOn || g_noReinforceCount.load() == 0 || StoreMultiplayer()) {
        devstats::Scope perfGame(g_perfCalcGame);
        g_calcBuildLocations(self, out, queues, fleet, designs);
        return;
    }
    alignas(8) uint8_t header[shipyardreinforce::kQueuesHeader];
    std::vector<uint8_t> kept;
    bool filtered = false;
    const LONGLONG perfStart = devstats::Ticks();
    try {
        filtered = ReinforceFilter(queues, header, kept);
    } catch (...) {
        filtered = false;
    }
    g_perfCalcFilter.AddSince(perfStart);
    if (filtered)
        g_perfCalcFiltered.fetch_add(1, std::memory_order_relaxed);
    devstats::Scope perfGame(g_perfCalcGame);
    g_calcBuildLocations(self, out, filtered ? header : queues, fleet, designs);
}

// ---- The checkbox ----
StarbaseControl g_reinforceCheck;     // .button is the CCheckBox (CNavigable at +0, CGuiObject at +8)
uint8_t* g_reinforceLabel = nullptr;  // CInstantTextBox beside it
uint8_t* g_reinforceTab = nullptr;    // ship_build_queue_tab
uint8_t* g_reinforceWindow = nullptr; // its capacity_info, which holds both
constexpr size_t kCheckBoxGuiObject = 8, kGuiObjectShown = 9;

uint8_t* ReinforceBoxObject() {
    return g_reinforceCheck.button ? g_reinforceCheck.button + kCheckBoxGuiObject : nullptr;
}
bool ReinforceChecked() {
    return uinavigation::Field<uint32_t>(g_reinforceCheck.button, g_reinforce.checkBoxState) == 1;
}
void ReinforceSetChecked(bool checked) {
    reinterpret_cast<void (*)(uint8_t*, bool)>(g_reinforce.checkBoxSetState)(g_reinforceCheck.button, checked);
}

// Why the selected starbase's box can't be changed, or nullptr.
const char* ReinforceUnavailable(uint32_t owner) {
    if (!g_reinforceOn)
        return "turned off after an error (see the log)";
    if (StoreMultiplayer())
        return "multiplayer";
    if (owner == UINT32_MAX)
        return "no starbase selected";
    if (owner != StarbaseLocalCountry())
        return "not your starbase";
    return nullptr;
}
bool ReinforceAllowed(uint32_t id, uint32_t owner) {
    std::lock_guard<std::mutex> lock(g_noReinforceLock);
    return g_noReinforce.Allowed(id, owner);
}

bool ReinforceTooltipCpp(uint8_t* tooltip) {
    auto* view = g_reinforceCheck.view;
    if (!view || !g_starbaseTemplatesOn)
        return false;
    const auto id = starbasetemplates::ReadField<uint32_t>(view, g_starbaseTemplateReader.selection.selectedReference);
    const uint32_t owner = StarbaseOwner(id);
    const auto text = shipyardreinforce::TooltipText(ReinforceUnavailable(owner), ReinforceAllowed(id, owner));
    StarbaseSetGameString(reinterpret_cast<game::String*>(tooltip), text); // CToolTip::_Instant (+0)
    return true;
}
bool ReinforceTooltipGuarded(uint8_t* tooltip) {
    try {
        return ReinforceTooltipCpp(tooltip);
    } catch (...) {
        return false;
    }
}
bool ReinforceTooltip(void*, const void*, uint8_t* tooltip) {
    __try {
        return ReinforceTooltipGuarded(tooltip);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
const void* const kReinforceTooltipVtable[] = {reinterpret_cast<const void*>(&ReinforceTooltip),
                                               reinterpret_cast<const void*>(&StarbaseTooltipDestroy)};
struct {
    const void* const* vtable;
} g_reinforceTooltip{kReinforceTooltipVtable};

// Called from StarbaseClickImpl, after the box has toggled itself: the box shows the new state.
void ReinforceClick(uint32_t id) {
    if (!g_reinforceCheck.button)
        return;
    const bool allowed = ReinforceChecked();
    const uint32_t owner = StarbaseOwner(id);
    const char* problem = ReinforceUnavailable(owner);
    bool changed = false;
    if (!problem) {
        std::lock_guard<std::mutex> lock(g_noReinforceLock);
        changed = g_noReinforce.Set(id, owner, allowed);
        g_noReinforceCount = g_noReinforce.bases.size();
        if (!changed)
            problem = "too many shipyards unchecked";
    }
    if (!changed)
        ReinforceSetChecked(!allowed);
    Log(LogLevel::Info, "shipyard_reinforce", "starbase %08X: %s", id,
        problem   ? problem
        : allowed ? "reinforcement allowed"
                  : "reinforcement not allowed");
}

// Every starbase view update: the box shows the selected starbase's setting, only on the player's own
// starbases, and keeps our tooltip (a tab controller can hand its children its own).
void ReinforceRefresh(uint8_t* view) {
    using namespace uinavigation;
    auto* box = ReinforceBoxObject();
    if (!box || g_reinforceCheck.view != view)
        return;
    const auto id = Field<uint32_t>(view, g_starbaseTemplateReader.selection.selectedReference);
    const uint32_t owner = StarbaseOwner(id);
    const bool available = !ReinforceUnavailable(owner);
    const bool shown = Field<uint8_t>(box, kGuiObjectShown) != 0;
    if (!available && shown) {
        Method<void (*)(uint8_t*)>(box, starbasetemplates::kButtonHideSlot)(box);
        if (g_reinforceLabel)
            Method<void (*)(uint8_t*)>(g_reinforceLabel, starbasetemplates::kButtonHideSlot)(g_reinforceLabel);
    } else if (available && !shown && Field<uint8_t>(g_reinforceTab, starbasetemplates::kWindowShown) &&
               Field<uint8_t>(g_reinforceWindow, starbasetemplates::kWindowShown)) {
        // Only undoes our own Hide: the tab is showing, so its other children are too.
        Method<void (*)(uint8_t*)>(box, starbasetemplates::kButtonShowSlot)(box);
        if (g_reinforceLabel)
            Method<void (*)(uint8_t*)>(g_reinforceLabel, starbasetemplates::kButtonShowSlot)(g_reinforceLabel);
    }
    if (available && ReinforceChecked() != ReinforceAllowed(id, owner))
        ReinforceSetChecked(ReinforceAllowed(id, owner));
    auto* handler = reinterpret_cast<uint8_t*>(&g_reinforceTooltip);
    Method<PointerMethod>(box, 0x220)(box, handler);
    if (g_reinforceLabel)
        Method<PointerMethod>(g_reinforceLabel, 0x220)(g_reinforceLabel, handler);
}

// Caller holds the template lock; every 10 s from the frame pass. Drops the entries of starbases that
// are gone or have another owner: they are allowed anyway.
void ReinforcePrune() {
    static ULONGLONG last = 0;
    const auto now = GetTickCount64();
    if (!g_reinforceOn || now - last < 10000)
        return;
    last = now;
    std::vector<std::pair<uint32_t, uint32_t>> bases;
    {
        std::lock_guard<std::mutex> lock(g_noReinforceLock);
        bases.assign(g_noReinforce.bases.begin(), g_noReinforce.bases.end());
    }
    for (const auto& [id, owner] : bases) {
        if (StarbaseOwner(id) == owner)
            continue;
        std::lock_guard<std::mutex> lock(g_noReinforceLock);
        g_noReinforce.bases.erase(id);
        g_noReinforceCount = g_noReinforce.bases.size();
        Log(LogLevel::Info, "shipyard_reinforce", "starbase %08X lost or destroyed; its entry removed", id);
    }
}

// True when the CPdxArray<CString> at `list` holds `name`.
bool ReinforceListed(uint8_t* list, std::string_view name) {
    using namespace uinavigation;
    auto* names = Field<game::String*>(list, 8);
    const int count = Field<int>(list, 0x14);
    if (!names || count < 0 || count > 4096)
        return false;
    for (int i = 0; i < count; ++i)
        if (std::string_view(names[i].data(), names[i].size) == name)
            return true;
    return false;
}

// The code a vftable slot points at, if it is a call to `target` at `offset` (a rel32 E8).
bool ReinforceCallsAt(uint8_t* object, size_t slot, size_t offset, uint8_t* target) {
    uint8_t* code = nullptr;
    uint8_t* table = nullptr;
    if (!starbasetemplates::CopyMemoryChecked(&table, object, sizeof(table)) || !table ||
        !starbasetemplates::CopyMemoryChecked(&code, table + slot, sizeof(code)) || !code)
        return false;
    uint8_t call[5];
    if (!starbasetemplates::CopyMemoryChecked(call, code + offset, sizeof(call)) || call[0] != 0xE8)
        return false;
    return locate::RelTarget(code + offset + 1, code + offset + 5) == target;
}

// The types are the game's own, shared with every window made from them; their placement fields
// are changed for the one instantiation and put back whatever happens.
uint8_t* ReinforceMakeBox(uint8_t* type, uint8_t* graphics, uint8_t* gui, uint8_t* workspace,
                          uinavigation::Point window, uinavigation::Point at) {
    using namespace shipyardreinforce;
    auto& position = *reinterpret_cast<uinavigation::Point*>(type + kButtonTypePosition);
    auto& orientation = *reinterpret_cast<int*>(type + kButtonTypeOrientation);
    auto& centred = *reinterpret_cast<uint8_t*>(type + kButtonTypeCentred);
    const auto oldPosition = position;
    const int oldOrientation = orientation;
    const uint8_t oldCentred = centred;
    uint8_t* box = nullptr;
    __try {
        position = at;
        orientation = 0;
        centred = 0;
        using Factory = uint8_t* (*)(uint8_t*, uint8_t*, uint8_t*, uint8_t*, uinavigation::Point);
        box = reinterpret_cast<Factory>(g_reinforce.checkBoxFactory)(type, graphics, gui, workspace, window);
    } __finally {
        position = oldPosition;
        orientation = oldOrientation;
        centred = oldCentred;
    }
    return box;
}
uint8_t* ReinforceMakeLabel(uint8_t* type, uint8_t* gui, uint8_t* workspace, uinavigation::Point window,
                            uinavigation::Point at, int width) {
    using namespace shipyardreinforce;
    auto& position = *reinterpret_cast<uinavigation::Point*>(type + kTextTypePosition);
    auto& maxWidth = *reinterpret_cast<uint16_t*>(type + kTextTypeMaxWidth);
    auto& orientation = *reinterpret_cast<int*>(type + kTextTypeOrientation);
    const auto oldPosition = position;
    const uint16_t oldWidth = maxWidth;
    const int oldOrientation = orientation;
    uint8_t* label = nullptr;
    __try {
        position = at;
        maxWidth = static_cast<uint16_t>(width);
        orientation = 0;
        using Instantiate = uint8_t* (*)(uint8_t*, uint8_t*, uint8_t*, uinavigation::Point);
        label = reinterpret_cast<Instantiate>(g_reinforce.textBoxInstantiate)(type, gui, workspace, window);
    } __finally {
        position = oldPosition;
        maxWidth = oldWidth;
        orientation = oldOrientation;
    }
    return label;
}

// Adds `object` to one of the window's child arrays and its name array, as
// CContainerWindow::InitGuiObjects does for the window's own checkboxes and text boxes, so the window
// shows, hides and frees it with them. (It doesn't attach them or list them in _PrioObjects.)
void ReinforceRegister(uint8_t* window, size_t objects, size_t names, uint8_t* object, const char* name) {
    using namespace uinavigation;
    const auto& l = g_navigation.locations;
    reinterpret_cast<void (*)(uint8_t*, int, uint8_t* const*)>(l.insertButton)(
        window + objects, Field<int>(window + objects, 0x14), &object);
    const auto key = Text(name);
    reinterpret_cast<void (*)(uint8_t*, int, const game::String*)>(l.insertButtonName)(
        window + names, Field<int>(window + names, 0x14), &key);
}

// Shipyard tab: the checkbox and its label on the bottom row of the capacity box ("Shipyards: N"),
// copied from the fleet view's automation list item (checkbox "checkbox", text "name"), which vanilla
// and UI Overhaul Dynamic both have. The label keeps that item's offset from its checkbox.
void AddReinforceCheckbox(uint8_t* root, uint8_t* view) {
    using namespace uinavigation;
    using namespace shipyardreinforce;
    g_reinforceCheck = {};
    g_reinforceLabel = g_reinforceTab = g_reinforceWindow = nullptr;
    const auto tabName = Text("ship_build_queue_tab");
    const auto capacityName = Text("capacity_info");
    auto* tab = reinterpret_cast<NamedLookup>(g_navigation.locations.window)(root, &tabName);
    auto* capacity = tab ? reinterpret_cast<NamedLookup>(g_navigation.locations.window)(tab, &capacityName) : nullptr;
    auto* gui = capacity ? Field<uint8_t*>(capacity, 0xb10) : nullptr;
    auto* parentType = capacity ? Field<uint8_t*>(capacity, 0xb18) : nullptr;
    auto* workspace = capacity ? Field<uint8_t*>(capacity, 0xb58) : nullptr;
    if (!gui || !parentType || !workspace) {
        Log(LogLevel::Warn, "shipyard_reinforce", "the Shipyard tab's capacity box wasn't found; no checkbox");
        return;
    }
    auto* item = GuiType(gui + 0x110, "fleet_automation_action_item");
    auto* boxType =
        item && ReinforceListed(item + kWindowTypeCheckBoxes, "checkbox") ? GuiType(item + 0x2d0, "checkbox") : nullptr;
    auto* textType =
        item && ReinforceListed(item + kWindowTypeTextBoxes, "name") ? GuiType(item + 0x2d0, "name") : nullptr;
    if (!boxType || !textType ||
        Method<void (*)()>(boxType, 0x68) != reinterpret_cast<void (*)()>(g_navigation.locations.buttonFactory) ||
        Method<uint8_t*>(boxType, kCheckBoxFactorySlot) != g_reinforce.checkBoxFactory ||
        !ReinforceCallsAt(gui, kGuiCreateTextBoxSlot, kGuiCreateTextBoxCall, g_reinforce.textBoxInstantiate)) {
        Log(LogLevel::Warn, "shipyard_reinforce", "the fleet view's checkbox types weren't found; no checkbox");
        return;
    }
    Point size{270, 74};
    if (g_windowGetSize)
        g_windowGetSize(capacity, &size);
    if (size.x < 150 || size.x > 2000 || size.y < 40 || size.y > 2000)
        size = {270, 74};
    const Point boxAt{70, size.y - 24};
    const Point boxType0 = Field<Point>(boxType, kButtonTypePosition),
                text0 = Field<Point>(textType, kTextTypePosition);
    const int dx = (std::clamp)(text0.x - boxType0.x, 22, 60), dy = (std::clamp)(text0.y - boxType0.y, -10, 20);
    const Point textAt{boxAt.x + dx, boxAt.y + dy};
    const int width = size.x - textAt.x - 4;
    auto* box = ReinforceMakeBox(boxType, Field<uint8_t*>(parentType, 0x5b0), gui, workspace, size, boxAt);
    if (!box) {
        Log(LogLevel::Warn, "shipyard_reinforce", "could not create the checkbox");
        return;
    }
    auto* boxObject = box + kCheckBoxGuiObject;
    Method<PointerMethod>(gui, 0x98)(gui, boxObject); // CGui::AddGuiObject; the factory attached it
    ReinforceRegister(capacity, kWindowCheckBoxes, kWindowCheckBoxNames, box, "sef_reinforce_allowed");
    g_reinforceCheck = {kStarbaseObserver, view, root, box, kReinforceAction};
    Method<void (*)(uint8_t*, StarbaseControl*)>(box + kCheckBoxObservable, 8)(box + kCheckBoxObservable,
                                                                               &g_reinforceCheck);
    ReinforceSetChecked(true);
    g_reinforceTab = tab;
    g_reinforceWindow = capacity;
    auto* label = ReinforceMakeLabel(textType, gui, workspace, size, textAt, width);
    if (label) {
        Method<PointerMethod>(gui, 0x98)(gui, label); // as CGui::CreateInstantTextBox does
        ReinforceRegister(capacity, kWindowTextBoxes, kWindowTextBoxNames, label, "sef_reinforce_allowed_label");
        g_textChange(label, Label(width));
        g_reinforceLabel = label;
    }
    auto* handler = reinterpret_cast<uint8_t*>(&g_reinforceTooltip);
    Method<PointerMethod>(boxObject, 0x220)(boxObject, handler);
    if (label)
        Method<PointerMethod>(label, 0x220)(label, handler);
    Log(LogLevel::Info, "shipyard_reinforce", "checkbox at %d,%d in a %dx%d box, label \"%s\"%s", boxAt.x, boxAt.y,
        size.x, size.y, Label(width), label ? "" : " (label not created)");
}
