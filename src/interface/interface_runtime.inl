// Interface additions of [interface] in stellaris-engine-fixes.ini that keep no data of their own:
// typing a trade amount, putting planet construction first in its queue, and the Contacts list.
// Their patterns are in uishortcuts.h and uinavigation.h.
//
// dllmain.cpp includes this file inside its anonymous namespace, after scriptstorage_runtime.inl.
// It ends with the state of the GUI reload hook, which the starbase and ship designer files
// included after it use; the hook itself is in interface_navigation.inl.

// ---- Trade amounts: Alt-click to type one (trade_amount_entry) ----

using ChangeTradeAmountFn = void (*)(void*, int, bool);
using TradeUiAction = void (*)(void*);
using TradeUiDestroy = void* (*)(void*, unsigned);
using TradeDialogSetTitle = void (*)(void*, const game::String*);

// CTextInputDialog copies the 32-byte Boost function buffer when the vtable's low bit is set.
// The invoker needs no captured state and the tagged vtable skips manager calls on destruction.
struct TradeTextCallback {
    uintptr_t taggedVtable;
    uint64_t storage[4]{};
};
static_assert(sizeof(TradeTextCallback) == 40 && offsetof(TradeTextCallback, storage) == 8);

uishortcuts::TradeLocations g_tradeUi;
uishortcuts::TradeControlLocations g_tradeControls;
uishortcuts::MarketAmountBackgroundLocations g_marketAmountBackground;
bool g_tradeControlsReady = false;

enum class TradeAmountKind { LumpSum, EmpireMonthly, MarketMonthly };
struct MarketTradeContext {
    const void* resource = nullptr;
    int mode = 0;
    int id = 0;
};
struct TradeAmountEdit {
    uint8_t* dialog = nullptr;
    void* row = nullptr;
    TradeAmountKind kind = TradeAmountKind::LumpSum;
    MarketTradeContext market;
    bool invalid = false;
};
TradeAmountEdit g_tradeEdit;
struct TradeRowLabels {
    void* container = nullptr;
    void* lump = nullptr;
    void* monthly = nullptr;
};
std::unordered_map<void*, TradeRowLabels> g_tradeLabels;
uint8_t* g_marketTradeView = nullptr;
using TradeInputFn = void (*)(void*, uint8_t*, void*);
using MarketAmountClickFn = void (*)(uint8_t*, void*);
TradeInputFn g_tradeTextInput = nullptr;
TradeInputFn g_tradeSpriteInput = nullptr;
MarketAmountClickFn g_marketAmountClick = nullptr;

bool ParseTradeDialogText(const game::String* text, int& amount) {
    __try {
        return text->size < 32 && uishortcuts::ParseTradeAmount({text->data(), text->size}, amount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* TradeGuiChild(void* container, size_t slot, const game::String* name) {
    using FindChild = void* (*)(void*, const game::String*);
    __try {
        if (!container)
            return nullptr;
        auto* vtable = *static_cast<uint8_t**>(container);
        return (*reinterpret_cast<FindChild*>(vtable + slot))(container, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* TradeGuiPointer(const void* object, size_t offset) {
    __try {
        return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ReadMarketTrade(uint8_t* view, MarketTradeContext& context) {
    __try {
        if (!view || !reinterpret_cast<bool (*)(void*)>(g_tradeControls.marketShown)(view))
            return false;
        context.resource = *reinterpret_cast<void**>(view + g_tradeControls.marketResource);
        context.mode = *reinterpret_cast<int*>(view + g_tradeControls.marketMode);
        context.id = *reinterpret_cast<int*>(view + g_tradeControls.marketId);
        return context.resource != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SetMarketTradeAmount(uint8_t* view, const MarketTradeContext& expected, int amount) {
    MarketTradeContext current;
    if (view != g_marketTradeView || !ReadMarketTrade(view, current) || current.resource != expected.resource ||
        current.mode != expected.mode || current.id != expected.id)
        return;
    __try {
        // OnAmountChange writes this nonnegative draft field. Update and AddTrade still
        // handle the display, eligibility checks and eventual submission.
        *reinterpret_cast<int*>(view + g_tradeControls.marketAmount) = amount;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_marketTradeView = nullptr;
    }
}

void AcceptTradeAmount(void*, const game::String* text) {
    if (!g_tradeEdit.row)
        return;
    int amount;
    if (!ParseTradeDialogText(text, amount)) {
        g_tradeEdit.invalid = true;
        return;
    }
    auto* row = g_tradeEdit.row;
    g_tradeEdit.row = nullptr;
    if (g_tradeEdit.kind == TradeAmountKind::MarketMonthly) {
        SetMarketTradeAmount(static_cast<uint8_t*>(row), g_tradeEdit.market, amount);
        return;
    }
    reinterpret_cast<ChangeTradeAmountFn>(g_tradeUi.changeValue)(row, amount,
                                                                 g_tradeEdit.kind == TradeAmountKind::EmpireMonthly);
    reinterpret_cast<TradeUiAction>(g_tradeUi.refreshRow)(row);
}

void FinishTradeDialog(uint8_t* dialog, bool finished) {
    __try {
        dialog[g_tradeUi.dialogFinishedOffset] = finished;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tradeEdit = {};
    }
}

void TradeDialogAcceptHook(uint8_t* dialog) {
    reinterpret_cast<TradeUiAction>(g_tradeUi.dialogAccept)(dialog);
    if (dialog == g_tradeEdit.dialog && g_tradeEdit.invalid) {
        g_tradeEdit.invalid = false;
        FinishTradeDialog(dialog, false);
        if (dialog == g_tradeEdit.dialog) {
            auto title = game::Borrow("Enter a whole number");
            reinterpret_cast<TradeDialogSetTitle>(g_tradeUi.setTitle)(dialog, &title);
        }
    }
}

void* TradeDialogDestroyHook(void* dialog, unsigned flags) {
    if (dialog == g_tradeEdit.dialog)
        g_tradeEdit = {};
    return reinterpret_cast<TradeUiDestroy>(g_tradeUi.dialogDestroy)(dialog, flags);
}

void CancelTradeAmountEdit(void* row) {
    if (!g_tradeEdit.dialog || row != g_tradeEdit.row)
        return;
    g_tradeEdit.row = nullptr;
    FinishTradeDialog(g_tradeEdit.dialog, true);
}

void TradeRowClearHook(void* row) {
    CancelTradeAmountEdit(row);
    g_tradeLabels.erase(row);
    reinterpret_cast<TradeUiAction>(g_tradeUi.rowClear)(row);
}

void* TradeRowDestroyHook(uint8_t* subobject, unsigned flags) {
    auto* row = subobject - g_tradeUi.rowDestroyThisOffset;
    CancelTradeAmountEdit(row);
    g_tradeLabels.erase(row);
    return reinterpret_cast<TradeUiDestroy>(g_tradeUi.rowDestroy)(subobject, flags);
}

void* TradeDialogIdler() {
    __try {
        return *g_tradeUi.idler;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool OpenTradeAmountDialog(void* row, TradeAmountKind kind, const MarketTradeContext& market = {}) {
    if (g_tradeEdit.dialog)
        return false;
    auto* idler = TradeDialogIdler();
    if (!idler)
        return false;
    using Allocate = void* (*)(size_t);
    using Construct = uint8_t* (*)(void*, const game::String*, const game::String*, const TradeTextCallback*);
    using AddUpdateable = int (*)(void*, void*);
    static const struct {
        uintptr_t manager;
        void (*invoke)(void*, const game::String*);
    } callbackVtable{0, &AcceptTradeAmount};

    auto title = game::Borrow(kind == TradeAmountKind::LumpSum ? "Trade amount" : "Monthly trade amount");
    auto initialText = game::Borrow("");
    TradeTextCallback callback{reinterpret_cast<uintptr_t>(&callbackVtable) | 1};
    auto* storage = reinterpret_cast<Allocate>(g_tradeUi.allocate)(g_tradeUi.dialogSize);
    auto* dialog = reinterpret_cast<Construct>(g_tradeUi.dialogConstructor)(storage, &title, &initialText, &callback);
    g_tradeEdit = {dialog, row, kind, market};
    reinterpret_cast<AddUpdateable>(g_tradeUi.addUpdateable)(idler, dialog);
    if (g_tradeEdit.dialog != dialog)
        return false;
    if (g_tradeControlsReady) {
        auto editName = game::Borrow("editbox");
        auto* container = TradeGuiPointer(dialog, g_tradeControls.dialogRoot);
        auto* edit = TradeGuiChild(container, g_tradeControls.findEditBox, &editName);
        if (edit)
            reinterpret_cast<TradeUiAction>(g_tradeControls.giveFocus)(edit);
    }
    return true;
}

void ChangeTradeAmountHook(void* row, int amount, bool monthly) {
    if (GetKeyState(VK_MENU) < 0) {
        OpenTradeAmountDialog(row, monthly ? TradeAmountKind::EmpireMonthly : TradeAmountKind::LumpSum);
        return;
    }
    reinterpret_cast<ChangeTradeAmountFn>(g_tradeUi.changeValue)(row, amount, monthly);
}

void TradeRowRefreshHook(void* row) {
    reinterpret_cast<TradeUiAction>(g_tradeUi.refreshRow)(row);
    if (!g_tradeControlsReady)
        return;
    auto* container = TradeGuiPointer(row, g_tradeControls.rowRoot);
    auto existing = g_tradeLabels.find(row);
    if (!container || (existing != g_tradeLabels.end() && existing->second.container == container))
        return;
    auto lump = game::Borrow("value");
    auto monthly = game::Borrow("value_monthly");
    g_tradeLabels[row] = {container, TradeGuiChild(container, g_tradeControls.findTextBox, &lump),
                          TradeGuiChild(container, g_tradeControls.findTextBox, &monthly)};
}

void MarketTradeUpdateHook(uint8_t* view) {
    reinterpret_cast<TradeUiAction>(g_tradeControls.marketUpdate)(view);
    g_marketTradeView = view;
}

void ForgetMarketTradeView(uint8_t* view) {
    if (g_marketTradeView == view)
        g_marketTradeView = nullptr;
    CancelTradeAmountEdit(view);
}

void MarketTradeReloadHook(uint8_t* view) {
    ForgetMarketTradeView(view);
    reinterpret_cast<TradeUiAction>(g_tradeControls.marketReload)(view);
}

void* MarketTradeDestroyHook(uint8_t* view, unsigned flags) {
    ForgetMarketTradeView(view);
    return reinterpret_cast<TradeUiDestroy>(g_tradeControls.marketDestroy)(view, flags);
}

bool OpenMarketAmount(uint8_t* view) {
    MarketTradeContext market;
    if (!ReadMarketTrade(view, market))
        return false;
    g_marketTradeView = view;
    return OpenTradeAmountDialog(view, TradeAmountKind::MarketMonthly, market);
}

void MarketAmountClickHook(uint8_t* view, void* button) {
    if (g_tradeControlsReady && button && GetKeyState(VK_MENU) < 0 &&
        (button == TradeGuiPointer(view, g_tradeControls.marketDecrease) ||
         button == TradeGuiPointer(view, g_tradeControls.marketIncrease))) {
        OpenMarketAmount(view);
        return;
    }
    g_marketAmountClick(view, button);
}

bool IsTradeAmountClick(const uint8_t* event) {
    __try {
        return !event[g_tradeControls.eventTaken] &&
               *reinterpret_cast<const int*>(event + g_tradeControls.eventType) == 0 &&   // mouse input
               *reinterpret_cast<const int*>(event + g_tradeControls.mouseAction) == 4 && // release
               *reinterpret_cast<const int*>(event + g_tradeControls.mouseButton) == 1;   // left button
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool OpenTradeAmountFromLabel(void* label) {
    if (g_marketTradeView && label == TradeGuiPointer(g_marketTradeView, g_tradeControls.marketLabel)) {
        return OpenMarketAmount(g_marketTradeView);
    }
    for (const auto& [row, labels] : g_tradeLabels) {
        if (label == labels.lump || label == labels.monthly) {
            // A reload can replace the row's GUI before the next refresh.
            if (labels.container != TradeGuiPointer(row, g_tradeControls.rowRoot))
                return false;
            return OpenTradeAmountDialog(row, label == labels.lump ? TradeAmountKind::LumpSum
                                                                   : TradeAmountKind::EmpireMonthly);
        }
    }
    return false;
}

void TradeTextInputHook(void* label, uint8_t* event, void* gui) {
    if (g_tradeControlsReady && IsTradeAmountClick(event) && GetKeyState(VK_MENU) < 0 &&
        OpenTradeAmountFromLabel(label)) {
        event[g_tradeControls.eventTaken] = 1;
        return;
    }
    g_tradeTextInput(label, event, gui);
}

bool IsMarketAmountBackground(void* sprite, uint8_t* view) {
    using FindChild = void* (*)(void*, const game::String*);
    const auto amount = game::Borrow("editable_amount");
    const auto background = game::Borrow("value_bg");
    __try {
        auto* root = TradeGuiPointer(view, g_marketAmountBackground.marketRoot);
        if (!root)
            return false;
        auto* window = reinterpret_cast<FindChild>(g_marketAmountBackground.findWindow)(root, &amount);
        return window && sprite == reinterpret_cast<FindChild>(g_marketAmountBackground.findIcon)(window, &background);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TradeSpriteInputHook(void* sprite, uint8_t* event, void* gui) {
    // The market amount's background can receive clicks outside the rendered text.
    auto* view = g_marketTradeView;
    if (view && IsTradeAmountClick(event) && GetKeyState(VK_MENU) < 0 && IsMarketAmountBackground(sprite, view) &&
        OpenMarketAmount(view)) {
        event[g_tradeControls.eventTaken] = 1;
        return;
    }
    g_tradeSpriteInput(sprite, event, gui);
}

bool InstallTradeControls(const locate::Image& img, std::string& error) {
    if (!uishortcuts::LocateTradeControls(img, g_tradeUi, g_tradeControls, error))
        return false;
    auto* market = g_tradeControls.marketVtable;
    if (!SwapVtableSlot(g_tradeUi.rowRefreshSlot, g_tradeUi.refreshRow,
                        reinterpret_cast<void*>(&TradeRowRefreshHook)) ||
        !SwapVtableSlot(market, g_tradeControls.marketDestroy, reinterpret_cast<void*>(&MarketTradeDestroyHook)) ||
        !SwapVtableSlot(market + 2, g_tradeControls.marketReload, reinterpret_cast<void*>(&MarketTradeReloadHook)) ||
        !SwapVtableSlot(market + 4, g_tradeControls.marketUpdate, reinterpret_cast<void*>(&MarketTradeUpdateHook))) {
        error = "could not hook trade view lifetimes";
        return false;
    }
    // Each starts with a stack store, push rdi, sub rsp and two register moves. No relative operands.
    constexpr size_t stolen = 16;
    g_marketAmountClick = reinterpret_cast<MarketAmountClickFn>(
        InstallJumpHook(g_tradeControls.marketClick, stolen, reinterpret_cast<void*>(&MarketAmountClickHook)));
    g_tradeTextInput = reinterpret_cast<TradeInputFn>(
        InstallJumpHook(g_tradeControls.textInput, stolen, reinterpret_cast<void*>(&TradeTextInputHook)));
    if (!g_marketAmountClick || !g_tradeTextInput) {
        error = "could not hook the market amount button or trade amount label";
        return false;
    }
    g_tradeControlsReady = true;
    return true;
}

void InstallTradeAmount(HMODULE self, const locate::Image& img) {
    if (!IniOnLogged(self, "interface", "trade_amount_entry", true, "trade_amount"))
        return;
    std::string error;
    if (!uishortcuts::LocateTrade(img, g_tradeUi, error)) {
        Log(LogLevel::Error, "trade_amount", "%s", error.c_str());
        return;
    }
    if (!SwapVtableSlot(g_tradeUi.dialogAcceptSlot, g_tradeUi.dialogAccept,
                        reinterpret_cast<void*>(&TradeDialogAcceptHook)) ||
        !SwapVtableSlot(g_tradeUi.dialogDestroySlot, g_tradeUi.dialogDestroy,
                        reinterpret_cast<void*>(&TradeDialogDestroyHook)) ||
        !SwapVtableSlot(g_tradeUi.rowClearSlot, g_tradeUi.rowClear, reinterpret_cast<void*>(&TradeRowClearHook)) ||
        !SwapVtableSlot(g_tradeUi.rowDestroySlot, g_tradeUi.rowDestroy,
                        reinterpret_cast<void*>(&TradeRowDestroyHook)) ||
        !RedirectCall(g_tradeUi.call, reinterpret_cast<const void*>(&ChangeTradeAmountHook))) {
        Log(LogLevel::Error, "trade_amount", "could not install the native text dialog hooks");
        return;
    }
    Log(LogLevel::Info, "trade_amount", "installed native text dialog");
    if (!InstallTradeControls(img, error)) {
        Log(LogLevel::Warn, "trade_amount", "%s", error.c_str());
        return;
    }
    Log(LogLevel::Info, "trade_amount", "installed market, label clicks and keyboard focus");
    if (!uishortcuts::LocateMarketAmountBackground(img, g_tradeControls, g_marketAmountBackground, error)) {
        Log(LogLevel::Warn, "trade_amount", "%s", error.c_str());
        return;
    }
    // A stack store, push rdi, sub rsp and two register moves. No relative operands.
    constexpr size_t stolen = 16;
    g_tradeSpriteInput = reinterpret_cast<TradeInputFn>(
        InstallJumpHook(g_marketAmountBackground.spriteInput, stolen, reinterpret_cast<void*>(&TradeSpriteInputHook)));
    if (g_tradeSpriteInput)
        Log(LogLevel::Info, "trade_amount", "installed market amount background clicks");
    else
        Log(LogLevel::Warn, "trade_amount", "could not hook market amount background clicks");
}

// ---- Planet construction: Ctrl-click puts it first in the queue (planet_queue_first) ----

using UiPostCommandFn = void (*)(void*, bool);
using UiExecuteCommandFn = void (*)(void*);
using UiDestroyFn = void* (*)(void*, unsigned);
using UiQueueInsertFn = void* (*)(uint8_t*, int, const uint32_t*);
using UiCreateCommandFn = void* (*)(const void*, int);

UiPostCommandFn g_uiPostCommand = nullptr;
locate::MultiplayerLocations g_uiMultiplayer;
uishortcuts::QueueLocations g_queueUi;
UiExecuteCommandFn g_queueExecute = nullptr;
UiDestroyFn g_queueDestroy = nullptr;
UiQueueInsertFn g_queueInsert = nullptr;
UiCreateCommandFn g_queueCreate = nullptr;
std::mutex g_queueCommandsLock;
std::unordered_set<const void*> g_queueCommands;
thread_local bool g_queuePostingFirst = false;
thread_local bool g_queueFirst = false;

bool UiLocalSession() {
    __try {
        const auto* session = *g_uiMultiplayer.session;
        return session && session[g_uiMultiplayer.flag] == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsQueueCommand(const void* command) {
    __try {
        return command && *static_cast<uintptr_t* const*>(command) == g_queueUi.vtable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TakeQueueCommand(const void* command) {
    std::lock_guard<std::mutex> lock(g_queueCommandsLock);
    return g_queueCommands.erase(command) != 0;
}

void UiPostCommandHook(void* command, bool validate) {
    bool previous = g_queuePostingFirst;
    g_queuePostingFirst = IsQueueCommand(command) && GetKeyState(VK_CONTROL) < 0 && UiLocalSession();
    g_uiPostCommand(command, validate);
    g_queuePostingFirst = previous;
}

void* QueueCreateCommandHook(const void* bytes, int size) {
    // The single-player server serializes the posted command and queues a fresh copy.
    // Keep the input modifier on that copy locally, outside the command's serialized fields.
    void* command = g_queueCreate(bytes, size);
    if (g_queuePostingFirst && IsQueueCommand(command)) {
        std::lock_guard<std::mutex> lock(g_queueCommandsLock);
        g_queueCommands.insert(command);
    }
    return command;
}

void QueueExecuteHook(void* command) {
    bool previous = g_queueFirst;
    g_queueFirst = TakeQueueCommand(command) && UiLocalSession();
    g_queueExecute(command);
    g_queueFirst = previous;
}

void* QueueDestroyHook(void* command, unsigned flags) {
    TakeQueueCommand(command);
    return g_queueDestroy(command, flags);
}

bool IsPlanetQueue(const uint8_t* array) {
    constexpr uint8_t kPlanetQueue = 1; // EConstructionQueueTypeEnum::Planet
    __try {
        return array[g_queueUi.arrayToType] == kPlanetQueue;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* QueueInsertHook(uint8_t* array, int index, const uint32_t* item) {
    if (g_queueFirst) {
        g_queueFirst = false;
        if (IsPlanetQueue(array))
            index = 0;
    }
    return g_queueInsert(array, index, item);
}

bool InstallQueueShortcut(const locate::Image& img, std::string& error) {
    if (!uishortcuts::LocateQueue(img, g_queueUi, error))
        return false;
    g_queueExecute = reinterpret_cast<UiExecuteCommandFn>(g_queueUi.execute);
    g_queueDestroy = reinterpret_cast<UiDestroyFn>(g_queueUi.destroy);
    g_queueInsert = reinterpret_cast<UiQueueInsertFn>(g_queueUi.insert);
    g_queueCreate = reinterpret_cast<UiCreateCommandFn>(g_queueUi.create);
    if (!RedirectCall(g_queueUi.insertCall, reinterpret_cast<const void*>(&QueueInsertHook)) ||
        !RedirectCall(g_queueUi.createCall, reinterpret_cast<const void*>(&QueueCreateCommandHook)) ||
        !SwapVtableSlot(g_queueUi.vtable, g_queueUi.destroy, reinterpret_cast<void*>(&QueueDestroyHook)) ||
        !SwapVtableSlot(g_queueUi.vtable + 9, g_queueUi.execute, reinterpret_cast<void*>(&QueueExecuteHook))) {
        error = "could not install the construction queue hooks";
        return false;
    }
    return true;
}

void InstallUiCommandShortcuts(HMODULE self, const locate::Image& img) {
    if (!IniOnLogged(self, "interface", "planet_queue_first", true, "command_shortcuts"))
        return;

    std::string error;
    if (!locate::LocateMultiplayer(img, g_uiMultiplayer, error)) {
        Log(LogLevel::Error, "command_shortcuts", "%s", error.c_str());
        return;
    }
    auto* post = locate::FindOne(img, "PostCommandToSession", uishortcuts::kPostCommand, error);
    if (!post) {
        Log(LogLevel::Error, "command_shortcuts", "%s", error.c_str());
        return;
    }
    if (!InstallQueueShortcut(img, error)) {
        Log(LogLevel::Warn, "planet_queue", "%s", error.c_str());
        return;
    }
    constexpr size_t stolen = 15; // three stack stores; no relative operands
    auto* trampoline = InstallJumpHook(post, stolen, reinterpret_cast<void*>(&UiPostCommandHook));
    if (!trampoline) {
        Log(LogLevel::Error, "command_shortcuts", "could not hook command posting");
        return;
    }
    g_uiPostCommand = reinterpret_cast<UiPostCommandFn>(trampoline);
    Log(LogLevel::Info, "planet_queue", "installed for single player");
}

// ---- Contacts: keeps its scroll position, and Diplomacy closes back to it (remember_contacts_scroll) ----

uinavigation::ContactsReturn g_contactsReturn;

void DiplomacyCloseButtonHook(uint8_t* view) {
    uinavigation::CloseDiplomacyFromButton(g_contactsReturn, view);
}

void InstallContactsScroll(HMODULE self, const locate::Image& image) {
    if (!IniOnLogged(self, "interface", "remember_contacts_scroll", true, "contacts_scroll"))
        return;

    std::string error;
    auto* reset = uinavigation::LocateContactsScrollReset(image, error);
    constexpr uint8_t skipCall[] = {0x90, 0x90, 0x90, 0x90, 0x90};
    if (!reset) {
        Log(LogLevel::Warn, "contacts_scroll", "%s", error.c_str());
    } else if (!WriteCode(reset, skipCall, sizeof(skipCall))) {
        Log(LogLevel::Warn, "contacts_scroll", "could not skip the scrollbar reset");
    } else {
        Log(LogLevel::Info, "contacts_scroll", "preserving the Contacts scrollbar on close");
    }

    if (!uinavigation::LocateContactsReturn(image, g_contactsReturn, error)) {
        Log(LogLevel::Warn, "contacts_scroll", "%s", error.c_str());
        return;
    }
    Code code;
    code.JumpTo(reinterpret_cast<const void*>(&DiplomacyCloseButtonHook));
    auto* load = g_contactsReturn.closeCallbackLoad;
    auto* stub = PlaceStub(code, load);
    int32_t rel = 0;
    if (!stub || !Rel32(load + 7, stub, &rel) || !WriteCode(load + 3, &rel, sizeof(rel))) {
        Log(LogLevel::Warn, "contacts_scroll", "could not bind the Diplomacy close button");
        return;
    }
    Log(LogLevel::Info, "contacts_scroll", "returning to Contacts when closing Diplomacy opened from it");
}

// ---- The GUI reload hook's state (the hook is in interface_navigation.inl) ----

using GuiReloadFn = bool (*)(uint8_t*);
GuiReloadFn g_origNavigationReload = nullptr;
uinavigation::Runtime g_navigation;
uinavigation::ViewControls g_navigationControls{};
