#pragma once

#include <climits>
#include <string_view>

#include "locate.h"

namespace uishortcuts {

struct TradeLocations {
    uint8_t* call = nullptr;
    uint8_t* changeValue = nullptr;
    uintptr_t* rowRefreshSlot = nullptr;
    uint8_t* refreshRow = nullptr;
    uintptr_t* rowDestroySlot = nullptr;
    uint8_t* rowDestroy = nullptr;
    size_t rowDestroyThisOffset = 0;
    uintptr_t* rowClearSlot = nullptr;
    uint8_t* rowClear = nullptr;
    void** idler = nullptr;
    uint8_t* allocate = nullptr;
    size_t dialogSize = 0;
    uint8_t* dialogConstructor = nullptr;
    uint8_t* addUpdateable = nullptr;
    uint8_t* setTitle = nullptr;
    uintptr_t* dialogDestroySlot = nullptr;
    uint8_t* dialogDestroy = nullptr;
    uintptr_t* dialogAcceptSlot = nullptr;
    uint8_t* dialogAccept = nullptr;
    size_t dialogFinishedOffset = 0;
};

inline uint8_t* InFunction(uint8_t* function, size_t length, const char* pattern) {
    auto hits = locate::FindAll(function, function + length, locate::Parse(pattern), 2);
    return hits.size() == 1 ? hits.front() : nullptr;
}

inline bool InCode(const locate::Image& img, const uint8_t* address, size_t length) {
    for (const auto& section : img.code) {
        if (address >= section.begin && address < section.end &&
            length <= static_cast<size_t>(section.end - address)) return true;
    }
    return false;
}

inline uint8_t* SlotTarget(const locate::Image& img, const uintptr_t* slot) {
    for (const auto& section : img.data) {
        auto* at = reinterpret_cast<const uint8_t*>(slot);
        if (at < section.begin || at + sizeof(*slot) > section.end) continue;
        auto* code = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(img.base) + (*slot - img.pointerBase));
        if (InCode(img, code, 1)) return code;
    }
    return nullptr;
}

constexpr char kPostCommand[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 0F B6 EA 48 8B F9 "
    "48 8D 05 ?? ?? ?? ?? 48 89 44 24 20";

struct QueueLocations {
    uintptr_t* vtable = nullptr;
    uint8_t* execute = nullptr;
    uint8_t* destroy = nullptr;
    uint8_t* insertCall = nullptr;
    uint8_t* insert = nullptr;
    uint8_t* createCall = nullptr;
    uint8_t* create = nullptr;
    size_t arrayToType = 0;
};

inline bool LocateQueue(const locate::Image& img, QueueLocations& out, std::string& error) {
    constexpr char constructor[] =
        "40 53 48 83 EC 20 48 8B D9 C7 41 08 FF FF FF FF 33 C9 48 8D 05 ?? ?? ?? ?? 89 4B 0C "
        "C7 43 10 00 00 FF FF 66 89 4B 14 88 4B 16 89 4B 18 48 89 03 49 8B 01 49 89 09 "
        "48 89 43 20 89 53 28 44 89 43 2C";
    constexpr char execute[] = "48 83 EC 28 48 8D 54 24 38 E8 ?? ?? ?? ?? 48 83 C4 28 C3";
    constexpr char executeLocal[] =
        "48 89 5C 24 20 55 56 57 48 83 EC 30 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? F6 40 38 01 74 ?? "
        "48 8B 4B 20 48 8B 01 FF 90 80 00 00 00";
    constexpr char addItem[] =
        "48 89 5C 24 18 4C 89 4C 24 20 55 56 57 41 56 41 57 48 83 EC 30 4D 8B F1 "
        "4D 8B F8 48 8B FA 48 8B D9 48 8D 69 ?? 80 3D";
    constexpr char append[] =
        "4C 8D 44 24 60 8B 55 14 48 8B CD E8 ?? ?? ?? ?? 8B 10 89 17 4C 8B 05 ?? ?? ?? ?? 4D 85 C0";
    constexpr char insert[] =
        "48 89 5C 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 63 41 14 4C 8B F1 8B 49 10 49 8B D8 48 63 FA";
    constexpr char callAdd[] = "4C 8D 4C 24 58 48 8B D7 49 8B CA E8 ?? ?? ?? ?? 90 48 8B 4C 24 50";
    constexpr char queueType[] = "88 4A ?? 44 88 4A 4D 44 88 4A 50";
    constexpr char dummyPost[] =
        "48 85 D2 0F 84 ?? ?? ?? ?? 48 89 54 24 10 48 89 4C 24 08 53 56 57 "
        "41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 48 8B FA";
    constexpr char createCall[] =
        "8B 54 24 ?? 48 8B 4C 24 ?? E8 ?? ?? ?? ?? 48 89 84 24 ?? ?? ?? ?? "
        "4C 89 A4 24 ?? ?? ?? ?? 49 8B CC FF 15 ?? ?? ?? ??";
    constexpr char create[] =
        "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC D0 05 00 00 44 8B C2 48 8B D1";

    auto* ctor = locate::FindOne(img, "add buildable command constructor", constructor, error);
    auto* local = locate::FindOne(img, "add buildable command execution", executeLocal, error);
    auto* add = locate::FindOne(img, "construction queue add", addItem, error);
    auto* type = locate::FindOne(img, "construction queue type initialization", queueType, error);
    auto* post = locate::FindOne(img, "CDummyServer::Post", dummyPost, error);
    out.create = locate::FindOne(img, "CreateCommand from buffer", create, error);
    if (!ctor || !local || !add || !type || !post || !out.create) return false;

    out.vtable = reinterpret_cast<uintptr_t*>(locate::RelTarget(ctor + 21, ctor + 25));
    out.destroy = SlotTarget(img, out.vtable);
    out.execute = SlotTarget(img, out.vtable + 9);
    auto* insertion = InFunction(add, 0x1ba, append);
    auto* addCall = InFunction(local, 0x165 + 4, callAdd);
    auto* recreate = InFunction(post, 0x900, createCall);
    auto executePattern = locate::Parse(execute);
    if (!out.destroy || !InCode(img, out.execute, executePattern.size()) || !locate::MatchAt(out.execute, executePattern) ||
        locate::RelTarget(out.execute + 10, out.execute + 14) != local || !insertion || !addCall ||
        locate::RelTarget(addCall + 12, addCall + 16) != add || type[2] <= add[36] ||
        !recreate || locate::RelTarget(recreate + 10, recreate + 14) != out.create) {
        error = "construction queue command or layout cross-check failed";
        return false;
    }
    out.arrayToType = type[2] - add[36];
    out.insertCall = insertion + 11;
    out.insert = locate::RelTarget(out.insertCall + 1, out.insertCall + 5);
    out.createCall = recreate + 9;
    auto insertPattern = locate::Parse(insert);
    if (!InCode(img, out.insert, insertPattern.size()) || !locate::MatchAt(out.insert, insertPattern)) {
        error = "construction queue insertion targets a different array routine";
        return false;
    }
    return true;
}

inline bool LocateTrade(const locate::Image& img, TradeLocations& out, std::string& error) {
    // OnChange converts the adjusted amount to whole units, calls ChangeValue, then updates the row.
    constexpr char tail[] =
        "48 B8 09 E1 D1 C6 11 6B F1 29 48 F7 E9 48 C1 FA 0E 48 8B C2 48 C1 E8 3F "
        "48 03 D0 49 8B CE E8 ?? ?? ?? ?? 49 8B 06 49 8B CE 4C 8D 5C 24 50";
    constexpr char setter[] =
        "40 55 41 54 41 55 41 56 41 57 48 83 EC 60 48 8D 6C 24 20 48 89 5D 70 "
        "48 89 75 78 48 89 BD 80 00 00 00 45 0F B6 E0";
    constexpr char rowConstructor[] =
        "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 48 89 4C 24 08 55 41 56 41 57 "
        "48 8D 6C 24 80 48 81 EC 80 01 00 00 48 8B F1 48 8B 85 C0 00 00 00 48 89 44 24 20 E8";
    constexpr char rowVtables[] =
        "90 48 8D 05 ?? ?? ?? ?? 48 89 06 48 8D 05 ?? ?? ?? ?? 48 89 46 08 "
        "48 8D 05 ?? ?? ?? ?? 48 89 46 ?? 48 8D 05 ?? ?? ?? ?? 48 89 86 80 00 00 00";
    constexpr char rowClear[] = "48 8B 81 98 00 00 00 45 33 D2 4C 8B 48 08 48 8B 81 90 00 00 00";
    constexpr char rowRefresh[] = "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC";

    // CCombatDebugView::RenameScenario allocates this dialog and registers it with the idler.
    constexpr char nativeAllocation[] =
        "44 89 75 ?? 48 8B 35 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F8 48 89 45 ?? "
        "48 8D 05 ?? ?? ?? ?? 48 89 45 ?? 44 89 75 ??";
    constexpr char nativeRegistration[] =
        "4C 8D 83 ?? ?? ?? ?? 4C 8D 4D ?? 48 8D 55 ?? 48 8B CF E8 ?? ?? ?? ?? "
        "90 48 8B D0 48 8B CE E8 ?? ?? ?? ?? 90 48 83 7D";
    constexpr char dialogConstructor[] =
        "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 48 89 4C 24 08 55 41 56 41 57 "
        "48 8B EC 48 83 EC 70 49 8B D9 4D 8B F0 48 8B FA 48 8B F1";
    constexpr char dialogVtable[] =
        "48 8D 05 ?? ?? ?? ?? 48 89 06 48 8D 05 ?? ?? ?? ?? 48 89 46 38 "
        "48 8D 05 ?? ?? ?? ?? 48 89 46 40 4C 89 BE ?? ?? ?? ??";
    constexpr char callbackCopy[] =
        "A8 01 74 10 0F 10 01 0F 11 02 0F 10 49 10 0F 11 4A 10 EB 0D 48 83 E0 FE";
    constexpr char dialogAccept[] = "40 53 48 81 EC 80 00 00 00 48 8B D9 48 8B 49 78 48 8B 01 4C 8B 80 D8 00 00 00";
    constexpr char acceptFinish[] =
        "48 83 E0 FE 48 8B 40 08 48 8D 8B ?? ?? ?? ?? 48 8D 54 24 50 FF D0 "
        "C6 83 ?? ?? ?? ?? 01 48 83 7C 24 78 10";
    constexpr char hasFinished[] = "0F B6 81 ?? ?? ?? ?? C3";
    constexpr char allocate[] = "40 53 48 83 EC 20 48 8B D9 EB ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0";
    constexpr char addUpdateable[] =
        "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 48 89 54 24 10 57 41 56 41 57 "
        "48 83 EC 20 48 8B 02 48 8B E9";
    constexpr char setTitle[] = "40 53 48 83 EC 60 48 8B DA 48 8B 49 78 48 8B 01 4C 8B 80 D0 00 00 00";

    auto matches = [&](const uint8_t* address, const char* signature) {
        auto pattern = locate::Parse(signature);
        return InCode(img, address, pattern.size()) && locate::MatchAt(address, pattern);
    };
    auto* tailAt = locate::FindOne(img, "trade resource amount call", tail, error);
    if (!tailAt) return false;
    out.call = tailAt + 30;
    out.changeValue = locate::FindOne(img, "CTradeOfferResourceItem::ChangeValue", setter, error);
    auto* rowCtor = locate::FindOne(img, "CTradeOfferResourceItem constructor", rowConstructor, error);
    auto* native = locate::FindOne(img, "native text dialog allocation", nativeAllocation, error);
    out.dialogConstructor = locate::FindOne(img, "CTextInputDialog constructor", dialogConstructor, error);
    out.addUpdateable = locate::FindOne(img, "CGameIdler::AddUpdateable", addUpdateable, error);
    out.setTitle = locate::FindOne(img, "CGameDialog::SetTitle", setTitle, error);
    if (!out.changeValue || !rowCtor || !native || !out.dialogConstructor || !out.addUpdateable || !out.setTitle) return false;
    if (locate::RelTarget(out.call + 1, out.call + 5) != out.changeValue) {
        error = "trade resource amount call targets a different setter";
        return false;
    }
    auto* row = InFunction(rowCtor, 0x90, rowVtables);
    if (!row) {
        error = "trade resource row vtables not found";
        return false;
    }
    auto* rowTable = reinterpret_cast<uintptr_t*>(locate::RelTarget(row + 4, row + 8));
    out.rowDestroySlot = reinterpret_cast<uintptr_t*>(locate::RelTarget(row + 25, row + 29));
    out.rowDestroyThisOffset = row[32];
    out.rowDestroy = SlotTarget(img, out.rowDestroySlot);
    out.rowClearSlot = rowTable + 0xb8 / sizeof(uintptr_t);
    out.rowClear = SlotTarget(img, out.rowClearSlot);
    out.rowRefreshSlot = rowTable + 0xa8 / sizeof(uintptr_t);
    out.refreshRow = SlotTarget(img, out.rowRefreshSlot);
    if (!matches(out.rowDestroy, "48 83 E9 ?? E9") || out.rowDestroy[3] != out.rowDestroyThisOffset ||
        !matches(out.rowClear, rowClear) || !matches(out.refreshRow, rowRefresh)) {
        error = "trade resource row lifecycle or refresh slot did not match";
        return false;
    }
    auto* registration = InFunction(native, 0x180, nativeRegistration);
    auto* dialog = InFunction(out.dialogConstructor, 0x180, dialogVtable);
    out.allocate = locate::RelTarget(native + 17, native + 21);
    out.dialogSize = locate::Disp32(native + 12);
    out.idler = reinterpret_cast<void**>(locate::RelTarget(native + 7, native + 11));
    if (!registration || locate::RelTarget(registration + 19, registration + 23) != out.dialogConstructor ||
        locate::RelTarget(registration + 31, registration + 35) != out.addUpdateable ||
        !matches(out.allocate, allocate) || !dialog || !InFunction(dialog, 0x80, callbackCopy)) {
        error = "native text dialog construction or registration did not match";
        return false;
    }
    out.dialogDestroySlot = reinterpret_cast<uintptr_t*>(locate::RelTarget(dialog + 3, dialog + 7));
    out.dialogDestroy = SlotTarget(img, out.dialogDestroySlot);
    out.dialogAcceptSlot = out.dialogDestroySlot + 0x110 / sizeof(uintptr_t);
    out.dialogAccept = SlotTarget(img, out.dialogAcceptSlot);
    auto* finished = SlotTarget(img, out.dialogDestroySlot + 0xd8 / sizeof(uintptr_t));
    auto* finish = matches(out.dialogAccept, dialogAccept) ? InFunction(out.dialogAccept, 0x108, acceptFinish) : nullptr;
    auto* deletion = InCode(img, out.dialogDestroy, 0x92) ? InFunction(out.dialogDestroy, 0x92,
        "BA ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ??") : nullptr;
    if (!deletion || locate::Disp32(deletion + 1) != out.dialogSize || !finish || !matches(finished, hasFinished) ||
        locate::Disp32(finished + 3) != locate::Disp32(finish + 24)) {
        error = "native text dialog lifetime or accept callback did not match";
        return false;
    }
    out.dialogFinishedOffset = locate::Disp32(finished + 3);
    return out.dialogFinishedOffset < out.dialogSize;
}

inline bool ParseTradeAmount(std::string_view text, int& amount) {
    if (text.empty()) return false;
    int value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        int digit = c - '0';
        if (value > (INT_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    amount = value;
    return true;
}

struct TradeControlLocations {
    uint8_t* giveFocus = nullptr;
    size_t dialogRoot = 0;
    size_t findEditBox = 0;
    uint8_t* textInput = nullptr;
    size_t eventType = 0;
    size_t mouseAction = 0;
    size_t mouseButton = 0;
    size_t eventTaken = 0;
    size_t rowRoot = 0;
    size_t findTextBox = 0;
    uint8_t* marketClick = nullptr;
    uintptr_t* marketVtable = nullptr;
    uint8_t* marketDestroy = nullptr;
    uint8_t* marketReload = nullptr;
    uint8_t* marketUpdate = nullptr;
    uint8_t* marketShown = nullptr;
    size_t marketDecrease = 0;
    size_t marketIncrease = 0;
    size_t marketAmount = 0;
    size_t marketLabel = 0;
    size_t marketResource = 0;
    size_t marketMode = 0;
    size_t marketId = 0;
};

inline bool LocateTradeControls(const locate::Image& img, const TradeLocations& trade, TradeControlLocations& out,
                                std::string& error) {
    constexpr char focus[] = "40 57 48 83 EC 30 80 B9 A9 02 00 00 00 48 8B F9 0F 85 ?? ?? ?? ?? "
        "C6 81 A8 02 00 00 01 48 8B 89 20 02 00 00";
    constexpr char textInput[] = "48 89 5C 24 10 57 48 83 EC 40 48 8B D9 48 8B FA 48 8B 89 D8 01 00 00 "
        "48 85 C9 0F 84 ?? ?? ?? ?? 80 BB 18 02 00 00 00 0F 85 ?? ?? ?? ?? 83 BA ?? ?? ?? ?? 00";
    constexpr char buttonUp[] = "83 F8 04 0F 85 ?? ?? ?? ?? 83 7E ?? 01 0F 85 ?? ?? ?? ?? 4C 8D 46 ?? "
        "33 D2 48 8B CF E8 ?? ?? ?? ?? 48 8B 5C 24 40 C6 86 ?? ?? ?? ?? 01 48 8B 74 24 58";
    constexpr char readAction[] = "8B 46 ?? 48 89 5C 24 40 83 F8 03 74 ?? 80 BF 82 01 00 00 00 "
        "75 ?? 83 F8 05 74 ?? 32 D2 83 F8 04";
    constexpr char lookupLabel[] = "49 8B 4D ?? 48 8B 01 4C 8B 80 ?? ?? ?? ?? 33 FF 89 7D A0 0F 57 C0 "
        "F3 0F 7F 45 A8 48 C7 45 C8 0F 00 00 00 48 C7 45 C0 05 00 00 00";
    constexpr char marketCtor[] = "48 89 5C 24 08 55 48 8D 6C 24 A9 48 81 EC E0 00 00 00 48 8D 15 ?? ?? ?? ?? "
        "48 8B D9 E8 ?? ?? ?? ?? 33 D2 48 8D 05 ?? ?? ?? ?? 48 89 03 48 8D 0D ?? ?? ?? ?? 48 8D 05";
    constexpr char marketClick[] = "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? "
        "48 3B BB ?? ?? ?? ?? 75 ?? 29 83 ?? ?? ?? ?? 79 ?? 33 C9 89 8B ?? ?? ?? ?? 48 8B 5C 24 30";
    constexpr char marketUp[] = "48 3B BB ?? ?? ?? ?? 75 ?? 01 83 08 06 00 00 B9 00 00 00 00 8B 83 ?? ?? ?? ?? "
        "0F 48 C1 89 83 ?? ?? ?? ?? 48 8B 5C 24 30 48 83 C4 20 5F C3";
    constexpr char marketLabel[] = "44 8B B6 ?? ?? ?? ?? 48 8B 9E ?? ?? ?? ?? 45 85 F6 7E ?? 41 8B D6 "
        "48 8D 4D 78 E8 ?? ?? ?? ?? 90 83 CF 01";
    constexpr char marketContext[] = "48 8B 81 ?? ?? ?? ?? 48 89 42 10 8B 81 ?? ?? ?? ?? "
        "48 8B 0D ?? ?? ?? ?? 89 42 18 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 83 C0 20";
    constexpr char marketData[] = "8B 87 ?? ?? ?? ?? 89 43 20 8B 87 ?? ?? ?? ?? 89 43 24 "
        "8B 87 ?? ?? ?? ?? 89 43 28 48 8B C3 48 8B 5C 24 30";

    out.giveFocus = locate::FindOne(img, "CEditBox::GiveFocus", focus, error);
    out.textInput = locate::FindOne(img, "CInstantTextBox::HandleInput", textInput, error);
    auto* up = locate::FindOne(img, "left mouse release", buttonUp, error);
    auto* ctor = locate::FindOne(img, "CMonthlyTradeView constructor", marketCtor, error);
    auto* plus = locate::FindOne(img, "monthly market amount increase", marketUp, error);
    auto* data = locate::FindOne(img, "monthly market trade data", marketData, error);
    if (!out.giveFocus || !out.textInput || !up || !ctor || !plus || !data) return false;
    auto* action = InFunction(up - 32, 32 + 3, readAction);
    auto* labelLookup = InFunction(trade.refreshRow, 0x280, lookupLabel);
    // The two native button callbacks are adjacent and differ only in the field they update.
    auto* click = InFunction(plus - 0x50, 0x50, marketClick);
    auto* context = InFunction(data - 0x45, 0x45, marketContext);
    if (!action || !labelLookup || !click || !context) {
        error = "trade control call sites did not match";
        return false;
    }
    out.marketVtable = reinterpret_cast<uintptr_t*>(locate::RelTarget(ctor + 38, ctor + 42));
    out.marketDestroy = SlotTarget(img, out.marketVtable);
    out.marketReload = SlotTarget(img, out.marketVtable + 2);
    out.marketUpdate = SlotTarget(img, out.marketVtable + 4);
    out.marketShown = SlotTarget(img, out.marketVtable + 0x70 / sizeof(uintptr_t));
    auto* amountLabel = out.marketUpdate ? InFunction(out.marketUpdate, 0x887, marketLabel) : nullptr;
    out.marketAmount = locate::Disp32(click + 32);
    if (!out.marketDestroy || !out.marketReload || !out.marketShown || !amountLabel ||
        out.marketAmount != locate::Disp32(plus + 11) || out.marketAmount != locate::Disp32(data + 2) ||
        out.marketAmount != locate::Disp32(amountLabel + 3)) {
        error = "monthly market amount field did not match across the view and callbacks";
        return false;
    }
    out.marketClick = click;
    out.marketDecrease = locate::Disp32(click + 24);
    out.marketIncrease = locate::Disp32(plus + 3);
    out.marketLabel = locate::Disp32(amountLabel + 10);
    out.marketResource = locate::Disp32(context + 3);
    out.marketMode = locate::Disp32(context + 13);
    out.marketId = locate::Disp32(data + 20);
    out.dialogRoot = trade.dialogAccept[15];
    out.findEditBox = locate::Disp32(trade.dialogAccept + 22);
    out.rowRoot = labelLookup[3];
    out.findTextBox = locate::Disp32(labelLookup + 10);
    out.eventType = locate::Disp32(out.textInput + 47);
    out.mouseAction = action[2];
    out.mouseButton = up[11];
    out.eventTaken = locate::Disp32(up + 40);
    return true;
}

struct MarketAmountBackgroundLocations {
    uint8_t* spriteInput = nullptr;
    uint8_t* findWindow = nullptr;
    uint8_t* findIcon = nullptr;
    size_t marketRoot = 0;
};

inline bool LocateMarketAmountBackground(const locate::Image& img, const TradeControlLocations& trade,
                                         MarketAmountBackgroundLocations& out, std::string& error) {
    constexpr char spriteInput[] =
        "48 89 74 24 20 57 48 83 EC 30 48 8B F9 48 8B F2 48 8B 89 ?? ?? ?? ?? 48 85 C9 74 ?? "
        "48 8B 01 FF 10 80 BE ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 BF ?? ?? ?? ?? 03";
    constexpr char findWindow[] =
        "48 89 74 24 10 57 48 83 EC 20 48 8B F2 4C 8D 81 70 08 00 00 48 8D 91 88 08 00 00 "
        "48 8B F9 48 8B CE E8 ?? ?? ?? ?? 48 85 C0 75 ??";
    constexpr char findIcon[] =
        "48 89 74 24 10 57 48 83 EC 20 48 8B F2 4C 8D 81 90 06 00 00 48 8D 91 A8 06 00 00 "
        "48 8B F9 48 8B CE E8 ?? ?? ?? ?? 48 85 C0 75 ??";
    auto shown = locate::Parse("48 8B 41 ?? 48 85 C0 74 ?? 80 78 41 00");
    out.spriteInput = locate::FindOne(img, "TGuiSprite::HandleInput", spriteInput, error);
    out.findWindow = locate::FindOne(img, "non-asserting container lookup", findWindow, error);
    out.findIcon = locate::FindOne(img, "non-asserting icon lookup", findIcon, error);
    if (!out.spriteInput || !out.findWindow || !out.findIcon) return false;
    if (!InCode(img, trade.marketShown, shown.size()) || !locate::MatchAt(trade.marketShown, shown) ||
        locate::RelTarget(out.findWindow + 34, out.findWindow + 38) !=
        locate::RelTarget(out.findIcon + 34, out.findIcon + 38)) {
        error = "market amount background lookups did not match";
        return false;
    }
    out.marketRoot = trade.marketShown[3];
    return true;
}

}  // namespace uishortcuts
