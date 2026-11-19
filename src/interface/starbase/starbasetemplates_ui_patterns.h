#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "../../core/locate.h"
namespace starbasetemplates {
inline constexpr char kButtonText[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B F0 48 8B EA 48 8B F9 4D 85 C0 75 07 48 8B B1 88 02 00 00 48 8D 5A 10 48 89 B1 88 02 00 00 48 83 7B 18 10 48 8B D3 72 03 48 8B 13 4C 8B 81 78 02 00 00 48 81 C1 68 02 00 00 48 83 79 18 10 72 03 48 8B 09 4C 3B 45 20 75 0D E8";
inline constexpr char kButtonSize[] =
    "48 89 5C 24 10 57 48 83 EC 20 8B 02 48 8B FA 89 44 24 30 48 8B D9 8B 42 04 48 8D 54 24 30 89 44 24 34 48 8B 01 FF 90 80 03 00 00 48 8B 8B 90 01 00 00 48 85 C9 74 0C 48 8B 01 48 8B 17 FF 90 08 02 00 00 48 8B 8B B0 01 00 00 48 85 C9 74 3B 48 8B 01 48 8B 17 FF 90 08 02 00 00 8B 07 48 8B 8B";
inline constexpr char kViewUpdate[] =
    "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 48 8B D9 48 8B 15 ?? ?? ?? ?? 48 85 D2 74 2D 44 8B 81 A8 00 00 00 41 8B C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 03 C9 48 8B 42 18 48 8B 74 C8 08 48 85 F6 74 06 44 39 46 08 74 07 48 8B 35 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";
// CWaystationView::UpdateSpecialized (GOG PDB): the waystation view's refresh. Like kViewUpdate
// it starts by resolving the selected starbase (+0xa8) in the starbase database (operand +42).
// Its first 15 bytes are three stack stores.
inline constexpr char kWaystationViewUpdate[] =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C 24 C9 48 81 EC 90 00 00 00 48 8B F9 48 8B 15 ?? ?? ?? ?? 48 85 D2 74 2D 44 8B 81 A8 00 00 00 41 8B C0 25 FF FF FF 00 3B 42 20 73 19 8B C8 48 03 C9 48 8B 42 18 48 8B 74 C8 08 48 85 F6 74 06 44 39 46 08 74 07 48 8B 35 ?? ?? ?? ?? 8B 86 9C 00 00 00";
inline constexpr size_t kWaystationViewUpdateStolen = 15;
inline constexpr char kIsDesignable[] =
    "40 53 48 83 EC 20 48 8D 99 A0 00 00 00 48 8B 03 48 8B CB FF 50 10 48 8B D0 48 63 83 2C 05 00 00 85 C0 78 12 3B 42 2C 7D 0D 48 69 C0 10 01 00 00 48 03 42 20 EB 04 48 8B 42 20 48 8B 40 08 0F B6 80 C8 00 00 00 D0 E8 24 01 48 83 C4 20 5B C3";
}
namespace starbasetemplates {
// CStarbaseSideView::ShowForSlot(side, EStarbaseSideViewControllerType, slot): resets the
// controllers and DeleteAndRemoveAll (+0xd0)s the list (+0xc8) when the mode (+0xdc) changes,
// stores the slot (+0xd8), then tail-calls Show (+0x30). The literal offsets pin the layout.
inline constexpr char kSideShowForSlot[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B F0 0F B6 FA 48 8B D9 3A 91 DC 00 00 00 74 ?? 33 C0 48 89 81 98 00 00 00 48 89 81 A0 00 00 00 48 89 81 A8 00 00 00 48 89 81 B0 00 00 00 89 81 B8 00 00 00 48 8B 89 C8 00 00 00 48 85 C9 74 ?? 48 8B 01 FF 90 D0 00 00 00 40 88 BB DC 00 00 00 48 8B 03 48 8B CB 89 B3 D8 00 00 00 48 8B 5C 24 30 48 8B 74 24 38 48 83 C4 20 5F 48 FF 60 30";
// CStarbaseSideView::Hide: CSmoothListbox::Hide (+0xb8) on the list, then hides the window
// (+0x78) if its CGuiObject::_bIsShown (+0x41 from the window pointer) is set.
inline constexpr char kSideHide[] =
    "40 53 48 83 EC 20 48 8B D9 48 8B 89 C8 00 00 00 48 85 C9 74 ?? 48 8B 01 FF 90 B8 00 00 00 48 8B 4B 78 C7 83 D8 00 00 00 FF FF FF FF 48 85 C9 74 ?? 80 79 41 00 74 ?? 48 8B 01 FF 50 48 C6 83 90 00 00 00 00 48 83 C4 20 5B C3";
// CAbstractStarbaseView::Clear with CStarbaseSideView::Clear inlined: side view at view+0xa0,
// title text box +0xd0, parent +0xc0, idle mode 6.
inline constexpr char kSideClear[] =
    "48 8B 81 A0 00 00 00 33 C9 48 89 88 C8 00 00 00 48 89 88 D0 00 00 00 48 8B 88 C0 00 00 00 C7 80 D8 00 00 00 FF FF FF FF C6 80 DC 00 00 00 06";
// CInstantTextBox::ChangeString(const char*) (GOG PDB): strlen, then ChangeStringInternal.
inline constexpr char kTextChange[] =
    "48 83 EC 38 48 89 54 24 20 48 85 D2 75 06 89 54 24 28 EB 19 48 C7 C0 FF FF FF FF 0F 1F 44 00 00 48 FF C0 80 3C 02 00 75 F7 89 44 24 28 48 85 D2 48 8D 54 24 20 0F 94 44 24 2C 0F 28 44 24 20 66 0F 7F 44 24 20 E8 ?? ?? ?? ?? 48 83 C4 38 C3";
inline constexpr size_t kSideView = 0xa0, kSideWindow = 0x78, kSideList = 0xc8, kSideTitle = 0xd0;
inline constexpr size_t kSideMode = 0xdc, kWindowShown = 0x41, kListHideSlot = 0xb8;
// Out of range of GetTitleString (blank title) and UpdateWithData (no list refresh).
inline constexpr uint8_t kSideTemplateMode = 6;
// CButtonStandard's primary vftable (GOG PDB): Show +0x70, Hide +0x78.
inline constexpr size_t kButtonShowSlot = 0x70, kButtonHideSlot = 0x78;
// CButtonStandard::MoveTo(CVector2<int>) (GOG PDB): orientation +0x2a0 (0 upper left, 1-8 add
// the parent size at +0x2a4/+0x2a8, or half of it), result stored at +0x1a0.
inline constexpr char kButtonMoveTo[] =
    "48 89 54 24 10 48 83 EC 28 8B 81 A0 02 00 00 44 8B 81 A4 02 00 00 FF C8 44 8B 89 A8 02 00 00 83 F8 07 77 ?? 4C 8D 15 ?? ?? ?? ?? 48 98 41 8B 94 82 ?? ?? ?? ?? 49 03 D2 FF E2";
// MoveTo's placement helper: with the type's (+0x198) centerPosition flag (+0x64b) set it
// subtracts half the sprite's (+0x190) current size from the position.
inline constexpr char kButtonCentre[] =
    "40 56 57 41 56 48 83 EC 20 48 8B 81 98 01 00 00 48 8B F9 48 89 5C 24 48 48 8B 99 A0 01 00 00 48 8B F3 48 89 6C 24 58 48 C1 EE 20 0F B6 EA 80 B8 4B 06 00 00 00 48 89 5C 24 40 74 ?? 48 8B 89 90 01 00 00 48 8D 54 24 50 48 8B 01 FF 90 B8 01 00 00";
// CContainerWindow::GetSize (GOG PDB): evaluates the window's CGuiSize into an out CVector2<int>.
inline constexpr char kWindowGetSize[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 81 60 0B 00 00 4C 8B FA 4C 8B F1 48 81 C1 98 09 00 00 48 8B B0 0C 01 00 00 48 8B DE 48 C1 EB 20 8B D3 E8 ?? ?? ?? ?? 44 8B C0 49 8D 8E 90 0A 00 00 8B D3 E8 ?? ?? ?? ?? 8B D6 49 8D 8E 98 09 00 00 8B D8 E8 ?? ?? ?? ?? 44 8B C0 49 8D 8E 90 0A 00 00 8B D6 E8 ?? ?? ?? ?? 48 8B 6C 24 48 48 8B 74 24 50 41 89 07 41 8B 86 C0 0A 00 00 03 C3";
inline constexpr size_t kButtonOrientation = 0x2a0, kButtonBasePosition = 0x1a8, kButtonType = 0x198;
inline constexpr size_t kButtonTypeCentred = 0x64b, kButtonMoveToSlot = 0x2b0;
}
namespace starbasetemplates {
// CButtonStandard::SetAlpha(float) and SetModulateColor(CColor) (GOG PDB; the colour is four floats,
// passed by address). Both forward to the button's sprite (+0x190). Their slots in the primary
// vftable are checked against these at run time.
inline constexpr char kButtonSetAlpha[] =
    "40 53 48 83 EC 30 48 8B D9 0F 29 74 24 20 48 8B 89 90 01 00 00 0F 28 F1 48 8B 01 FF 50 08 48 8B 83 B0 01 00 00 48 85 C0 74 08 F3 0F 11 B0 A4 01 00 00";
inline constexpr char kButtonSetModulate[] =
    "48 83 EC 28 48 8B 89 90 01 00 00 41 B0 01 48 8B 01 FF 90 28 02 00 00 48 83 C4 28 C3";
inline constexpr size_t kButtonSetAlphaSlot = 0x330, kButtonSetModulateSlot = 0xf8;
// CButtonStandard::_position (after orientation and centring) and _WindowSize (the parent size
// MoveTo orients against).
inline constexpr size_t kButtonPosition = 0x1a0, kButtonWindowSize = 0x2a4;
}
namespace starbasetemplates {
// CButtonStandard::GetSize(CPoint<int>* out) (primary vftable +0x328; the point comes back through a
// hidden pointer): asks the sprite (+0x190). Its code, which every button vftable's slot holds in
// all six tested builds; the runtime compares the slot's target with it before calling.
inline constexpr size_t kButtonGetSizeSlot = 0x328;
inline constexpr uint8_t kButtonGetSizeCode[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x89, 0x90, 0x01, 0x00,
                                                 0x00, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0xB0, 0x01, 0x00,
                                                 0x00, 0x48, 0x8B, 0xC3, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
}
namespace starbasetemplates {
// NStarbaseView::ApplyComponentToButton<T>(button, component) (GOG PDB; one body serves modules and
// buildings): what the native module and building lists call for an entry's icon. It gets the
// component's icon key, calls ChangeSpriteKeepScale(button, key) (the call at +36) and tail-calls
// the button's SetFrame (+0xb0) with the component's icon frame.
inline constexpr char kApplyComponentToButton[] =
    "48 89 5C 24 08 57 48 83 EC 50 48 8B FA 48 8B D9 48 8D 54 24 20 48 8B CF E8 ?? ?? ?? ?? 90 48 8B D0 48 8B CB E8 ?? ?? ?? ?? 90 48 83 7C 24 48 10 72 11 83 7C 24 20 01 74 0A 48 8B 4C 24 30 E8 ?? ?? ?? ?? 48 8B 03 8B 97 ?? ?? 00 00 48 8B CB 48 8B 5C 24 60 48 83 C4 50 5F 48 FF A0 B0 00 00 00";
// ChangeSpriteKeepScale(TButton*, const CString& key) (GOG PDB): GetScale (+0x348), ChangeSprite
// (+0x2e8), SetScale(float) (+0x340, operand +77), then MoveTo its own position. The exe holds two
// identical copies, so it is found as kApplyComponentToButton's call target and compared here.
inline constexpr char kChangeSpriteKeepScale[] =
    "48 85 C9 0F 84 87 00 00 00 48 89 5C 24 18 56 48 83 EC 30 48 8B 01 48 8B DA 48 89 7C 24 48 48 8B F1 0F B6 79 09 0F 29 74 24 20 FF 90 ?? ?? 00 00 48 8B 06 48 8B D3 48 8B CE 0F 28 F0 FF 90 ?? ?? 00 00 48 8B 06 0F 28 CE 48 8B CE FF 90 ?? ?? 00 00";
inline constexpr size_t kApplyKeepScaleCall = 37, kKeepScaleSetScale = 77, kKeepScaleSize = 81;
struct IconLocations {
    uint8_t* apply = nullptr;     // ApplyComponentToButton(button, component)
    uint8_t* keepScale = nullptr; // ChangeSpriteKeepScale(button, const CString*)
    size_t setScaleSlot = 0;      // CButtonStandard::SetScale(float) in the primary vftable
};
inline bool LocateIcons(const locate::Image& image, IconLocations& out, std::string& error) {
    auto* apply = locate::FindOne(image, "ApplyComponentToButton", kApplyComponentToButton, error);
    if (!apply)
        return false;
    auto* keep = locate::RelTarget(apply + kApplyKeepScaleCall, apply + kApplyKeepScaleCall + 4);
    bool code = false;
    for (const auto& section : image.code)
        code |= keep >= section.begin && keep + kKeepScaleSize <= section.end;
    const int32_t slot = code && locate::MatchAt(keep, locate::Parse(kChangeSpriteKeepScale))
                             ? locate::Disp32(keep + kKeepScaleSetScale)
                             : 0;
    if (slot <= 0 || slot >= 0x800 || slot % 8) {
        error = "ChangeSpriteKeepScale cross-check failed";
        return false;
    }
    out = {apply, keep, static_cast<size_t>(slot)};
    return true;
}
}
