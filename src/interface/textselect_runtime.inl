// Included by dllmain.cpp. Text selection in every edit box of the game ([interface]
// text_selection; textselect.h, re/text_selection.md): Ctrl+A selects all, and dragging with the
// left button selects. The game's own text buffer keeps and draws the selection, and its Ctrl+C,
// Ctrl+X, typing and Delete act on it; this only sets it. Everything runs on the main thread.

using EditBoxInputFn = void (*)(uint8_t* box, uint8_t* event, void* mouse);
textselect::Locations g_textSelect;
EditBoxInputFn g_editBoxInput = nullptr;
std::atomic<bool> g_textSelectOn{false};
// The box a left-button drag started in, and the cursor where it started. Only compared with the
// box a call is for, never followed: the box may be gone.
const uint8_t* g_dragBox = nullptr;
uint32_t g_dragAnchor = 0;

template <class T> T& BoxField(uint8_t* box, size_t offset) {
    return *reinterpret_cast<T*>(box + offset);
}
uint8_t* TextBuffer(uint8_t* box) {
    return box + textselect::kBoxBuffer;
}
uint32_t TextCursor(uint8_t* box) {
    return BoxField<uint32_t>(TextBuffer(box), textselect::kBufferCursor);
}
void TextUpdateCursor(uint8_t* box) {
    reinterpret_cast<void (*)(uint8_t*)>(g_textSelect.updateCursor)(box);
}
// The selection runs from `anchor` to the cursor; none when they are the same place. This is what
// the buffer's own Shift+arrow moves do: the start, SetSelection (which copies the selected text),
// then the mode.
void TextSelectFrom(uint8_t* box, uint32_t anchor) {
    uint8_t* buffer = TextBuffer(box);
    BoxField<uint32_t>(buffer, textselect::kBufferStart) = anchor;
    reinterpret_cast<void (*)(uint8_t*)>(g_textSelect.setSelection)(buffer);
    BoxField<uint8_t>(buffer, textselect::kBufferSelecting) = TextCursor(box) != anchor;
    TextUpdateCursor(box);
}
// The game's own click: the cursor goes where `event` points. A copy of the event, made a left
// button release, which is when the game moves the cursor.
void TextClickAt(uint8_t* box, const uint8_t* event, void* mouse) {
    uint8_t click[textselect::kEventSize];
    memcpy(click, event, sizeof(click));
    BoxField<int32_t>(click, textselect::kEventMouseType) = textselect::kMouseUp;
    BoxField<int32_t>(click, textselect::kEventMouseButton) = textselect::kMouseLeft;
    BoxField<uint8_t>(click, textselect::kEventTaken) = 0;
    g_editBoxInput(box, click, mouse);
}
bool TextLeftHeld() {
    // The physical button: the primary one is the right when the user swapped them.
    return GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) < 0;
}

// True when the event was handled here; the game's handler then doesn't see it.
bool TextSelectBefore(uint8_t* box, uint8_t* event, void* mouse) {
    using namespace textselect;
    if (BoxField<uint8_t>(box, kBoxDisabled))
        return false;
    const int type = BoxField<int32_t>(event, kEventType);
    if (type == kKeyEvent) {
        // Ctrl+A, as the game tests Ctrl+C: key down, Ctrl alone, while the box has the keys.
        if (BoxField<int32_t>(event, kEventKeyType) != kKeyDown || BoxField<int32_t>(event, kEventKeyMods) != kModCtrl ||
            BoxField<int32_t>(event, kEventKeyCode) != kKeyA || !BoxField<uint8_t>(box, kBoxShown) ||
            !BoxField<uint8_t>(box, kBoxFocus) || BoxField<uint8_t>(box, kBoxIme))
            return false;
        const size_t length = BoxField<size_t>(TextBuffer(box), kBufferTextSize);
        if (length > 0x10000)
            return false;
        reinterpret_cast<void (*)(uint8_t*, int, int)>(g_textSelect.select)(TextBuffer(box), 0,
                                                                            static_cast<int>(length));
        TextUpdateCursor(box);
        BoxField<uint8_t>(event, kEventTaken) = 1;
        return true;
    }
    // A password's characters aren't shown, so the mouse can't pick them; the keys still can.
    if (type != kMouseEvent || BoxField<uint8_t>(box, kBoxPassword))
        return false;
    const int mouseType = BoxField<int32_t>(event, kEventMouseType);
    const bool left = BoxField<int32_t>(event, kEventMouseButton) == kMouseLeft;
    const bool over = BoxField<uint8_t>(box, kBoxMouseOver) != 0;
    if (mouseType == kMouseDown && left && over) {
        // The game does nothing on the press but take it. Put the cursor there, as a click
        // would, and start the selection from it; a plain click clears an old selection.
        g_editBoxInput(box, event, mouse);
        TextClickAt(box, event, mouse);
        g_dragBox = box;
        g_dragAnchor = TextCursor(box);
        TextSelectFrom(box, g_dragAnchor);
        BoxField<uint8_t>(event, kEventTaken) = 1;
        return true;
    }
    if (box != g_dragBox)
        return false;
    if (mouseType == kMouseMoved && over) {
        if (!TextLeftHeld()) {
            g_dragBox = nullptr; // released somewhere the box didn't hear of
            return false;
        }
        TextClickAt(box, event, mouse);
        TextSelectFrom(box, g_dragAnchor);
        BoxField<uint8_t>(event, kEventTaken) = 1;
        return true;
    }
    if (mouseType == kMouseUp && left) {
        // The game's own release moves the cursor to it; the selection then ends there.
        g_editBoxInput(box, event, mouse);
        g_dragBox = nullptr;
        TextSelectFrom(box, g_dragAnchor);
        return true;
    }
    return false;
}

bool TextSelectBeforeGuarded(uint8_t* box, uint8_t* event, void* mouse) {
    __try {
        return TextSelectBefore(box, event, mouse);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // From the next event on, every edit box is the game's own again. A drag that faulted
        // halfway leaves at most a selection the keys can change.
        g_textSelectOn = false;
        g_dragBox = nullptr;
        return false;
    }
}

void EditBoxInputHook(uint8_t* box, uint8_t* event, void* mouse) {
    if (g_textSelectOn && box && event) {
        if (TextSelectBeforeGuarded(box, event, mouse))
            return;
        if (!g_textSelectOn)
            Log(LogLevel::Error, "text_selection", "fault in an edit box; text selection is off");
    }
    g_editBoxInput(box, event, mouse);
}

void InstallTextSelection(HMODULE self, const locate::Image& img) {
    if (!IniOnLogged(self, "interface", "text_selection", true, "text_selection"))
        return;
    std::string error;
    if (!textselect::Locate(img, g_textSelect, error)) {
        Log(LogLevel::Error, "text_selection", "%s; edit boxes stay as they are", error.c_str());
        return;
    }
    g_editBoxInput = reinterpret_cast<EditBoxInputFn>(g_textSelect.handleInput);
    // The hook is the same for every entry: a vftable left unswapped only misses the selection.
    size_t hooked = 0;
    for (size_t i = 0; i < g_textSelect.slotCount; ++i)
        hooked += SwapVtableSlot(g_textSelect.handleInputSlots[i], g_textSelect.handleInput,
                                 reinterpret_cast<void*>(&EditBoxInputHook));
    if (hooked != g_textSelect.slotCount) {
        Log(LogLevel::Error, "text_selection", "could hook only %zu of %zu CEditBox::HandleInput entries", hooked,
            g_textSelect.slotCount);
        if (!hooked)
            return;
    }
    g_textSelectOn = true;
    Log(LogLevel::Info, "text_selection",
        "HandleInput +%llX (%zu vftables), Select +%llX, SetSelection +%llX, UpdateCursorPosition +%llX; "
        "Ctrl+A and mouse selection in edit boxes",
        Rva(g_textSelect.handleInput), hooked, Rva(g_textSelect.select), Rva(g_textSelect.setSelection),
        Rva(g_textSelect.updateCursor));
}
