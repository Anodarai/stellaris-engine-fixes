// Part of starbasetemplates_runtime.inl: building the panel out of the game's own GUI types. Placing,
// sizing and giving icons to cloned buttons, opening and closing the side view, and the defenses
// tab's auto-upgrade icon.

// Puts the button's top-left corner at rect, whatever the cloned type's orientation and
// centerPosition say (UI mods centre it: UI Overhaul Dynamic's retrofit is 180x64, centred).
// Resize first, because MoveTo centres on the sprite's current size.
void StarbasePlaceButton(uint8_t* button, const starbasetemplates::PanelRect& rect) {
    using namespace uinavigation;
    using namespace starbasetemplates;
    const Point size{rect.w, rect.h};
    reinterpret_cast<void (*)(uint8_t*, const Point*)>(g_starbaseButtonSize)(button, &size);
    Point position{rect.x, rect.y};
    // Only a CButtonStandard has these fields; its MoveTo is the one the patterns checked.
    if (g_buttonMoveTo &&
        Field<uintptr_t*>(button, 0)[kButtonMoveToSlot / 8] == reinterpret_cast<uintptr_t>(g_buttonMoveTo)) {
        Field<int>(button, kButtonOrientation) = 0; // relative to the parent's upper left
        auto* type = Field<uint8_t*>(button, kButtonType);
        if (type && Field<uint8_t>(type, kButtonTypeCentred)) {
            position.x += rect.w / 2;
            position.y += rect.h / 2;
        }
    }
    Field<Point>(button, kButtonBasePosition) = position; // SetWindowSize moves back to this
    Method<void (*)(uint8_t*, Point)>(button, kButtonMoveToSlot)(button, position);
}

bool StarbaseButtonSize(uint8_t* button, uinavigation::Point& size);

// Gives a cloned button the icon of the module or building `definition`, through the game's
// ApplyComponentToButton, and fits it into `box`, centred.
// A plain sprite can't be resized (CSprite::SetSize does nothing), so the button is scaled; its
// GetSize is the texture's frame size times the scale. `natural` gets the size at scale 1.
bool StarbaseSetIcon(uint8_t* button, uint8_t* definition, const starbasetemplates::PanelRect& box,
                     uinavigation::Point& natural) {
    using namespace uinavigation;
    using namespace starbasetemplates;
    const auto& icons = g_starbaseIcons;
    if (!button || !icons.apply || !g_buttonMoveTo ||
        Field<uintptr_t*>(button, 0)[kButtonMoveToSlot / 8] != reinterpret_cast<uintptr_t>(g_buttonMoveTo))
        return false;
    auto scale = [&](float value) { Method<void (*)(uint8_t*, float)>(button, icons.setScaleSlot)(button, value); };
    scale(1.0f);
    reinterpret_cast<void (*)(uint8_t*, uint8_t*)>(icons.apply)(button, definition);
    if (!StarbaseButtonSize(button, natural))
        return false;
    const float fit = (std::min)(box.w / static_cast<float>(natural.x), box.h / static_cast<float>(natural.y));
    scale(fit);
    Point shown{};
    if (!StarbaseButtonSize(button, shown) || shown.x > box.w + 1 || shown.y > box.h + 1)
        shown = {static_cast<int>(natural.x * fit), static_cast<int>(natural.y * fit)};
    Point position{box.x + (box.w - shown.x) / 2, box.y + (box.h - shown.y) / 2};
    Field<int>(button, kButtonOrientation) = 0; // relative to the parent's upper left
    auto* type = Field<uint8_t*>(button, kButtonType);
    if (type && Field<uint8_t>(type, kButtonTypeCentred)) { // MoveTo subtracts half the shown size
        position.x += shown.x / 2;
        position.y += shown.y / 2;
    }
    Field<Point>(button, kButtonBasePosition) = position;
    Method<void (*)(uint8_t*, Point)>(button, kButtonMoveToSlot)(button, position);
    return true;
}

// Row `row`'s icon: `definition`'s, or hidden without one. The sprite changes only when the
// definition does.
void StarbaseRowIcon(int row, uint8_t* definition) {
    using namespace starbasetemplates;
    auto* slot = g_starbaseView;
    auto* button = slot->icons[row].button;
    if (definition && definition != slot->iconDef[row]) {
        const auto r = PanelLayout(kPanelRow0 + row, slot->panel.geometry, true);
        uinavigation::Point natural{};
        static bool logged = false, failed = false;
        if (StarbaseSetIcon(button, definition, {r.x + 3, r.y + 1, r.h - 2, r.h - 2}, natural)) {
            slot->iconDef[row] = definition;
            if (!logged)
                Log(LogLevel::Info, "starbase_templates", "row icons: %dx%d sprites in %dx%d", natural.x, natural.y,
                    r.h - 2, r.h - 2);
            logged = true;
        } else {
            if (!failed)
                Log(LogLevel::Warn, "starbase_templates", "row icons: sprite size unknown (%dx%d); hidden", natural.x,
                    natural.y);
            failed = true;
            definition = nullptr;
        }
    }
    if (!definition)
        slot->iconDef[row] = nullptr;
    StarbaseShowButton(button, definition != nullptr);
}

// Clones `type` (a standard button) into the container window `parent`, as the game's own
// GUI loader does, and routes its clicks to `control`. `name` must have static storage.
uint8_t* StarbaseCreateButton(uint8_t* type, uint8_t* parent, uinavigation::Point window,
                              const starbasetemplates::PanelRect& rect, const char* name, const char* label,
                              StarbaseControl& control, uint8_t* view, uint8_t* root, int action,
                              uint8_t* beside = nullptr, int left = 0,
                              const StarbaseControl::Callback* observer = nullptr) {
    using namespace uinavigation;
    using namespace starbasetemplates;
    auto* gui = Field<uint8_t*>(parent, 0xb10);
    auto* parentType = Field<uint8_t*>(parent, 0xb18);
    auto* workspace = Field<uint8_t*>(parent, 0xb58);
    if (!gui || !parentType)
        return nullptr;
    using Factory = uint8_t* (*)(uint8_t*, uint8_t*, uint8_t*, uint8_t*, Point);
    auto* button = Method<Factory>(type, 0x68)(type, Field<uint8_t*>(parentType, 0x5b0), gui, workspace, window);
    if (!button)
        return nullptr;
    Method<PointerMethod>(gui, 0x98)(gui, button);
    Method<PointerMethod>(button, 0xf0)(button, workspace);
    Method<PointerMethod>(button, 0x220)(button, Method<PtrMethod>(parent + 0x38, 0x228)(parent + 0x38));
    if (beside) {
        // Same type, so same size and centring as `beside`: copy its orientation and original
        // position, `left` pixels further left. The factory got its window size.
        Field<int>(button, kButtonOrientation) = Field<int>(beside, kButtonOrientation);
        Point position = Field<Point>(beside, kButtonBasePosition);
        position.x -= left;
        Field<Point>(button, kButtonBasePosition) = position;
        Method<void (*)(uint8_t*, Point)>(button, kButtonMoveToSlot)(button, position);
    } else {
        StarbasePlaceButton(button, rect);
    }
    auto key = Text(name);
    for (size_t offset : {size_t(0x660), size_t(0x630)})
        reinterpret_cast<void (*)(uint8_t*, int, uint8_t* const*)>(g_navigation.locations.insertButton)(
            parent + offset, Field<int>(parent + offset, 0x14), &button);
    reinterpret_cast<void (*)(uint8_t*, int, const game::String*)>(g_navigation.locations.insertButtonName)(
        parent + 0x678, Field<int>(parent + 0x678, 0x14), &key);
    control = {observer ? observer : kStarbaseObserver, view, root, button, action};
    Method<void (*)(uint8_t*, StarbaseControl*)>(button + 0xd8, 8)(button + 0xd8, &control);
    StarbaseText(button, label);
    return button;
}

bool StarbaseBuildPanel(uint8_t* view, uint8_t* side, uint8_t* window) {
    using namespace starbasetemplates;
    static const char* const names[kPanelControls] = {
        "sef_tpl_save",   "sef_tpl_save_global", "sef_tpl_prev",   "sef_tpl_page",  "sef_tpl_next",  "sef_tpl_apply",
        "sef_tpl_delete", "sef_tpl_stop",        "sef_tpl_status", "sef_tpl_new",   "sef_tpl_edit",
        "sef_tpl_copy_code", "sef_tpl_paste_code", "sef_tpl_row00",
        "sef_tpl_row01",  "sef_tpl_row02",       "sef_tpl_row03",  "sef_tpl_row04", "sef_tpl_row05", "sef_tpl_row06",
        "sef_tpl_row07",  "sef_tpl_row08",       "sef_tpl_row09",  "sef_tpl_row10", "sef_tpl_row11", "sef_tpl_row12",
        "sef_tpl_row13",  "sef_tpl_row14",       "sef_tpl_row15",  "sef_tpl_row16", "sef_tpl_row17", "sef_tpl_row18",
        "sef_tpl_row19",  "sef_tpl_row20",       "sef_tpl_row21",  "sef_tpl_row22", "sef_tpl_row23"};
    static const char* const labels[kPanelRow0] = {"Save",   "Save global", "<",   "",     ">",         "Apply",
                                                   "Delete", "Stop plan",   "",    "New",  "Edit",      "Copy code",
                                                   "Paste code"};
    static_assert(kPanelRow0 == 13 && kPanelMaxRows == 24, "names and labels follow PanelControl");
    auto& p = g_starbaseView->panel;
    p.built = false;
    p.slotRows = false;
    p.side = side;
    p.window = window;
    auto* root = uinavigation::Field<uint8_t*>(view, 0x78);
    // UI mods resize the side view (UI Overhaul Dynamic: 450x871); lay out for the real one.
    uinavigation::Point size{0, 0};
    if (g_windowGetSize)
        g_windowGetSize(window, &size);
    p.geometry = MakeGeometry(size.x, size.y);
    Log(LogLevel::Info, "starbase_templates", "template panel %dx%d (window %dx%d), %d list rows, %d editor rows",
        p.geometry.width, p.geometry.height, size.x, size.y, p.geometry.rows, p.geometry.slotRows);
    // The backdrop first, so everything else draws over it: from above the first row to above
    // the pager, whatever the rows' size.
    g_starbaseView->background = {};
    if (auto* type = g_starbaseSkin.types[kSkinBackground]) {
        const auto first = PanelLayout(kPanelRow0, p.geometry), pager = PanelLayout(kPanelPrev, p.geometry);
        const PanelRect r{first.x - 4, first.y - 4, first.w + 8, pager.y - first.y};
        if (auto* back =
                StarbaseCreateButton(type, window, {p.geometry.width, p.geometry.height}, r, "sef_tpl_background", "",
                                     g_starbaseView->background, view, root, -1, nullptr, 0, kStarbaseInertObserver))
            StarbaseShowButton(back, false);
        else
            g_starbaseView->background = {}; // the panel works without it
    }
    auto roleOf = [](int control) {
        switch (control) {
        case kPanelApply:
            return kSkinPrimary;
        case kPanelPrev:
        case kPanelNext:
            return kSkinPager;
        case kPanelPage:
        case kPanelStatus:
            return kSkinLabel;
        default:
            return control >= kPanelRow0 ? kSkinRow : kSkinAction;
        }
    };
    for (int i = 0; i < kPanelControls; ++i) {
        auto* button = StarbaseCreateButton(
            g_starbaseSkin.types[roleOf(i)], window, {p.geometry.width, p.geometry.height}, PanelLayout(i, p.geometry),
            names[i], i < kPanelRow0 ? labels[i] : "", g_starbaseView->controls[i], view, root, i);
        if (!button)
            return false;
        StarbaseShowButton(button, false);
    }
    // Made after the row buttons, so drawn over them. Each has its row's action.
    static const char* const iconNames[kPanelMaxRows] = {
        "sef_tpl_icon00", "sef_tpl_icon01", "sef_tpl_icon02", "sef_tpl_icon03", "sef_tpl_icon04", "sef_tpl_icon05",
        "sef_tpl_icon06", "sef_tpl_icon07", "sef_tpl_icon08", "sef_tpl_icon09", "sef_tpl_icon10", "sef_tpl_icon11",
        "sef_tpl_icon12", "sef_tpl_icon13", "sef_tpl_icon14", "sef_tpl_icon15", "sef_tpl_icon16", "sef_tpl_icon17",
        "sef_tpl_icon18", "sef_tpl_icon19", "sef_tpl_icon20", "sef_tpl_icon21", "sef_tpl_icon22", "sef_tpl_icon23"};
    for (int i = 0; i < kPanelMaxRows; ++i) {
        g_starbaseView->icons[i] = {};
        g_starbaseView->iconDef[i] = nullptr;
        if (!g_starbaseIcons.apply)
            continue;
        const auto r = PanelLayout(kPanelRow0 + i, p.geometry, true);
        auto* icon = StarbaseCreateButton(g_starbaseSkin.types[kSkinIcon], window,
                                          {p.geometry.width, p.geometry.height}, {r.x + 3, r.y + 1, r.h - 2, r.h - 2},
                                          iconNames[i], "", g_starbaseView->icons[i], view, root, kPanelRow0 + i);
        if (!icon) {
            g_starbaseView->icons[i] = {};
            break;
        } // the panel works without them
        StarbaseShowButton(icon, false);
    }
    p.built = true;
    return true;
}

void StarbaseOpenPanel(uint8_t* view) {
    using namespace uinavigation;
    using namespace starbasetemplates;
    auto* side = Field<uint8_t*>(view, kSideView);
    if (!side || !g_starbaseButtonType)
        return;
    // The native opener: resets the list when the mode changes, then shows the window.
    g_sideShowForSlot(side, kSideTemplateMode, -1);
    auto* window = Field<uint8_t*>(side, kSideWindow);
    if (!window)
        return;
    if (auto* list = Field<uint8_t*>(side, kSideList))
        Method<void (*)(uint8_t*)>(list, kListHideSlot)(list);
    auto& p = g_starbaseView->panel;
    p.title.clear(); // the opener reset the title; the refresh writes the current mode's
    // A draft left open comes back as it was, with the definitions read again: a load in
    // between may have changed them. Without them, back to the list.
    auto& e = g_starbaseView->editor;
    if (e.mode != StarbaseEditorState::Manager)
        e.mode = StarbaseEditorLoad(e.draft.kind) ? StarbaseEditorState::Editor : StarbaseEditorState::Manager;
    if ((!p.built || p.side != side || p.window != window) && !StarbaseBuildPanel(view, side, window)) {
        g_sideHide(side);
        Log(LogLevel::Warn, "starbase_templates", "could not create the template panel");
        return;
    }
    p.open = true;
    p.follow = true;
    p.dirty = true;
    for (int i = 0; i < kPanelRow0; ++i)
        StarbaseShowButton(g_starbaseView->controls[i].button, true);
    StarbaseShowButton(g_starbaseView->background.button, true);
}

void StarbaseClosePanel(uint8_t* view) {
    if (auto* side = uinavigation::Field<uint8_t*>(view, starbasetemplates::kSideView))
        g_sideHide(side);
    StarbaseHidePanelButtons();
}

// The button called `name` among the container window's own buttons, or nullptr.
uint8_t* StarbaseWindowButton(uint8_t* window, std::string_view name) {
    using namespace uinavigation;
    auto* names = Field<game::String*>(window + 0x678, 8);
    auto* buttons = Field<uint8_t**>(window + 0x660, 8);
    const int count = Field<int>(window + 0x678, 0x14);
    if (!names || !buttons || count < 0 || count > 4096 || count != Field<int>(window + 0x660, 0x14))
        return nullptr;
    for (int i = 0; i < count; ++i)
        if (std::string_view(names[i].data(), names[i].size) == name)
            return buttons[i];
    return nullptr;
}

// A CButtonStandard whose primary vftable has the MoveTo, SetAlpha and SetModulateColor we located.
bool StarbaseIconButton(uint8_t* button) {
    using namespace starbasetemplates;
    if (!button || !g_buttonMoveTo || !g_buttonSetAlpha || !g_buttonSetModulate)
        return false;
    const auto* table = uinavigation::Field<uintptr_t*>(button, 0);
    return table[kButtonMoveToSlot / 8] == reinterpret_cast<uintptr_t>(g_buttonMoveTo) &&
           table[kButtonSetAlphaSlot / 8] == reinterpret_cast<uintptr_t>(g_buttonSetAlpha) &&
           table[kButtonSetModulateSlot / 8] == reinterpret_cast<uintptr_t>(g_buttonSetModulate);
}

// The button's sprite size (CButtonStandard::GetSize), once its vftable slot is the expected code.
bool StarbaseButtonSize(uint8_t* button, uinavigation::Point& size) {
    using namespace starbasetemplates;
    auto* code = reinterpret_cast<const uint8_t*>(uinavigation::Field<uintptr_t*>(button, 0)[kButtonGetSizeSlot / 8]);
    uint8_t bytes[sizeof(kButtonGetSizeCode)];
    if (!CopyMemoryChecked(bytes, code, sizeof(bytes)) || memcmp(bytes, kButtonGetSizeCode, sizeof(bytes)) != 0)
        return false;
    reinterpret_cast<uinavigation::Point* (*)(uint8_t*, uinavigation::Point*)>(code)(button, &size);
    return size.x > 0 && size.y > 0 && size.x <= 256 && size.y <= 256;
}

// The planet view's automation toggle (GFX_button_automation_small), the game's own automation
// icon. Its top-level window type is laid out the same in vanilla and UI Overhaul Dynamic.
uint8_t* StarbaseAutomationType(uint8_t* gui) {
    using namespace uinavigation;
    auto* window = gui ? GuiType(gui + 0x110, "planet_automation_settings_main_title") : nullptr;
    auto* type = window ? GuiType(window + 0x2d0, "automated_development_button") : nullptr;
    return type && Method<void (*)()>(type, 0x68) == reinterpret_cast<void (*)()>(g_navigation.locations.buttonFactory)
               ? type
               : nullptr;
}

// Defenses tab: an automation icon in the slot just left of the platform capacity bar's Upgrade icon.
// The slot is the Upgrade icon's place moved left by the Upgrade-to-Disband spacing, so it lines up
// in vanilla and in UI mods alike; the automation icon is centred in it. Without the automation
// type (or the size check) it is a copy of the Upgrade icon itself. Its look shows the state
// (StarbaseAutoUpgradeLook).
void AddAutoUpgradeIcon(uint8_t* root, uint8_t* view) {
    using namespace uinavigation;
    using namespace starbasetemplates;
    g_autoUpgradeToggle = {};
    const auto defensesName = Text("defenses_tab");
    const auto capacityName = Text("capacity_info");
    auto* defenses = reinterpret_cast<NamedLookup>(g_navigation.locations.window)(root, &defensesName);
    auto* capacity =
        defenses ? reinterpret_cast<NamedLookup>(g_navigation.locations.window)(defenses, &capacityName) : nullptr;
    auto* upgrade = capacity ? StarbaseWindowButton(capacity, "upgrade") : nullptr;
    auto* disband = capacity ? StarbaseWindowButton(capacity, "disband_all") : nullptr;
    auto* type = StarbaseIconButton(upgrade) ? Field<uint8_t*>(upgrade, kButtonType) : nullptr;
    if (!type || Method<void (*)()>(type, 0x68) != reinterpret_cast<void (*)()>(g_navigation.locations.buttonFactory)) {
        Log(LogLevel::Warn, "starbase_autoupgrade",
            "the Defenses tab's Upgrade icon wasn't found; no Auto-upgrade toggle");
        return;
    }
    int spacing = 36;
    if (StarbaseIconButton(disband)) {
        const int d = Field<Point>(disband, kButtonPosition).x - Field<Point>(upgrade, kButtonPosition).x;
        if (d >= 20 && d <= 80)
            spacing = d;
    }
    // Created in the Upgrade icon's place (its orientation, spacing to the left), then centred.
    Point slot{};
    const bool sized = StarbaseButtonSize(upgrade, slot);
    auto* automation = sized ? StarbaseAutomationType(Field<uint8_t*>(capacity, 0xb10)) : nullptr;
    auto* button = StarbaseCreateButton(
        automation ? automation : type, capacity, Field<Point>(upgrade, kButtonWindowSize), {0, 0, 0, 0},
        "sef_platform_autoupgrade", "", g_autoUpgradeToggle, view, root, kAutoUpgradeAction, upgrade, spacing);
    if (!button) {
        Log(LogLevel::Warn, "starbase_autoupgrade", "could not create the Auto-upgrade toggle");
        return;
    }
    Point icon{};
    if (automation && StarbaseIconButton(button) && StarbaseButtonSize(button, icon)) {
        const Point corner = Field<Point>(upgrade, kButtonPosition);
        StarbasePlaceButton(
            button, {corner.x - spacing + (slot.x - icon.x) / 2, corner.y + (slot.y - icon.y) / 2, icon.x, icon.y});
        Log(LogLevel::Info, "starbase_autoupgrade", "toggle: automation icon %dx%d in a %dx%d slot", icon.x, icon.y,
            slot.x, slot.y);
    } else {
        Log(LogLevel::Info, "starbase_autoupgrade", "toggle: copy of the Upgrade icon (automation icon %s)",
            !sized       ? "skipped: Upgrade icon size unknown"
            : automation ? "size unknown"
                         : "type not found");
    }
    // The cloned type brings the native icon's U shortcut; an empty name is no key.
    const auto noKey = Text("");
    reinterpret_cast<void (*)(uint8_t*, const game::String*)>(g_navigation.locations.setShortcut)(button, &noKey);
    // Our own tooltip (CGuiObject::SetTooltipHandler) instead of the parent's.
    Method<PointerMethod>(button, 0x220)(button, reinterpret_cast<uint8_t*>(&g_autoUpgradeTooltip));
}
