// Text selection in the game's edit boxes ([interface] text_selection): the patterns and the
// layouts it reads. The game's CTextBuffer already keeps a selection: Shift with the arrow keys,
// Home and End extends it, Ctrl+C/X copy and cut it, typing replaces it, and CEditBox draws it
// (_pTextSelectionBox). What it lacks is Ctrl+A and the mouse: a click only moves the cursor, on
// the button's release. See re/text_selection.md.
#pragma once
#include <iterator>
#include "../core/locate.h"

namespace textselect {
// CEditBox::HandleInput(this, CInputEvent&, CMouse*), virtual (CEditBox vftable +0x270 in the
// GOG 4.5.2 PDB). It starts: disabled (+0x2a9), the event's type (+0xd8, 0 mouse), the mouse
// over the box (+0x10), the mouse button (+0x3c, 1 left), the mouse event's type (+0x38, 4 up),
// then sets the box's +0x0b and moves the cursor to the click. The literal offsets are the
// layouts below; the pattern matches once in the Steam and GOG 4.5.2, 4.5.3 and 4.5.4 exes.
constexpr char kHandleInput[] =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50 48 8B F2 48 8B F9 80 B9 A9 02 00 00 00 0F 85 ?? ?? ?? ?? "
    "8B 82 D8 00 00 00 85 C0 0F 85 ?? ?? ?? ?? 38 41 10 0F 84 ?? ?? ?? ?? 83 7A 3C 01 0F 85 ?? ?? ?? ?? "
    "83 7A 38 04 0F 85 ?? ?? ?? ?? 66 C7 41 0B 01 00";
constexpr size_t kHandleInputLength = 0x1b7; // GOG 4.5.2's, from its PDB
// In it, after the click moved the cursor: mov rcx, rdi; call CEditBox::UpdateCursorPosition
// (+4); mov byte [rsi+0xdc], 1 (the event is taken).
constexpr char kUpdateCursorCall[] = "48 8B CF E8 ?? ?? ?? ?? C6 86 DC 00 00 00 01";
// CTextBuffer::Select(this, 0, end) (the start is always 0, so MSVC folded it): the cursor to 0
// (call CalculateCursorPosition +18), _StartSelection (+0x96) = _CursorPosition (+0x7c), the
// cursor to `end` (+37, the same function), SetSelection (+45), _SelectionMode (+0x94) = 1.
constexpr char kSelect[] = "48 89 5C 24 08 57 48 83 EC 20 33 D2 41 8B D8 48 8B F9 E8 ?? ?? ?? ?? 8B 47 7C 8B D3 "
                           "48 8B CF 89 87 96 00 00 00 E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B 5C 24 30 "
                           "C6 87 94 00 00 00 01";

// CEditBox (GOG 4.5.2 PDB; the pattern above carries the same offsets in every build).
constexpr size_t kBoxShown = 0x09;     // CGuiObject: shown
constexpr size_t kBoxMouseOver = 0x10; // CGuiObject: the mouse is over it
constexpr size_t kBoxBuffer = 0xd8;    // CTextBuffer base
constexpr size_t kBoxFocus = 0x2a8;    // _bHasFocus
constexpr size_t kBoxDisabled = 0x2a9; // _bDisabled
constexpr size_t kBoxIme = 0x2aa;      // _bShowIME: the IME has the keys
constexpr size_t kBoxPassword = 0x2ab; // _bPassword
// CTextBuffer.
constexpr size_t kBufferTextSize = 0x48;  // _Text (CUTF32String, +0x38): its length in characters
constexpr size_t kBufferCursor = 0x7c;    // _CursorPosition (CCursorPosition: column, row; 16 bits each)
constexpr size_t kBufferSelecting = 0x94; // _SelectionMode
constexpr size_t kBufferStart = 0x96;     // _StartSelection (CCursorPosition)
// CInputEvent (0xe0 bytes): the key event at +0, the mouse event at +0x38.
constexpr size_t kEventSize = 0xe0;
constexpr size_t kEventKeyCode = 0x00, kEventKeyMods = 0x04, kEventKeyType = 0x08;
constexpr size_t kEventMouseType = 0x38, kEventMouseButton = 0x3c;
constexpr size_t kEventType = 0xd8, kEventTaken = 0xdc;
// Enum values (GOG 4.5.2 PDB).
enum EventType { kMouseEvent = 0, kKeyEvent = 1 };
enum MouseType { kMouseMoved = 1, kMouseDown = 3, kMouseUp = 4, kMouseDoubleClick = 5 };
constexpr int kMouseLeft = 1;
constexpr int kKeyDown = 0;
constexpr int kModCtrl = 1; // EModKey; Ctrl alone is exactly 1, as the game's own Ctrl+C test requires
constexpr int kKeyA = 'a';  // the game's key codes are SDL's: CTextBuffer compares Ctrl+C with 'c'

struct Locations {
    uint8_t* handleInput = nullptr;
    // Its vftable entries: CEditBox's and CNullEditBox's, which doesn't override it (GOG 4.5.2 PDB).
    uintptr_t* handleInputSlots[4] = {};
    size_t slotCount = 0;
    uint8_t* updateCursor = nullptr;      // CEditBox::UpdateCursorPosition(this)
    uint8_t* select = nullptr;            // CTextBuffer::Select(this, 0, end)
    uint8_t* setSelection = nullptr;      // CTextBuffer::SetSelection(this)
};

inline bool Locate(const locate::Image& img, Locations& out, std::string& error) {
    out = {};
    out.handleInput = locate::FindOne(img, "CEditBox::HandleInput", kHandleInput, error);
    out.select = locate::FindOne(img, "CTextBuffer::Select", kSelect, error);
    if (!out.handleInput || !out.select)
        return false;
    auto hits = locate::FindAll(out.handleInput, out.handleInput + kHandleInputLength, locate::Parse(kUpdateCursorCall), 2);
    if (hits.size() != 1) {
        error = "CEditBox::UpdateCursorPosition call: " + std::to_string(hits.size()) + " matches in HandleInput";
        return false;
    }
    out.updateCursor = locate::RelTarget(hits[0] + 4, hits[0] + 8);
    // Select calls the same CalculateCursorPosition twice, then SetSelection.
    if (locate::RelTarget(out.select + 19, out.select + 23) != locate::RelTarget(out.select + 38, out.select + 42)) {
        error = "CTextBuffer::Select: its two cursor calls differ";
        return false;
    }
    out.setSelection = locate::RelTarget(out.select + 46, out.select + 50);
    const uintptr_t want = img.pointerBase + static_cast<uintptr_t>(out.handleInput - img.base);
    size_t count = 0;
    for (const auto& s : img.data) {
        auto* p = reinterpret_cast<uintptr_t*>((reinterpret_cast<uintptr_t>(s.begin) + 7) & ~uintptr_t(7));
        for (; reinterpret_cast<uint8_t*>(p + 1) <= s.end; ++p)
            if (*p == want && count++ < std::size(out.handleInputSlots))
                out.handleInputSlots[count - 1] = p;
    }
    if (count < 1 || count > std::size(out.handleInputSlots)) {
        error = "CEditBox::HandleInput: " + std::to_string(count) + " vftable entries";
        return false;
    }
    out.slotCount = count;
    for (auto* target : {out.updateCursor, out.setSelection}) {
        bool code = false;
        for (const auto& s : img.code)
            code |= target >= s.begin && target < s.end;
        if (!code) {
            error = "text selection call target outside the code";
            return false;
        }
    }
    return true;
}
} // namespace textselect
