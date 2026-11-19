#include <cstdio>

#include "../src/interface/textselect.h"
#include "../src/interface/uishortcuts.h"
#include "../src/storage/storage_patterns.h"

int main(int argc, char** argv) {
    struct Input {
        std::string_view text;
        int expected;
    };
    constexpr Input cases[] = {
        {"0", 0},
        {"12345", 12345},
        {"2147483647", INT_MAX},
        {"2147483648", -1},
        {"9999999999999", -1},
        {"", -1},
        {"-1", -1},
        {"1.5", -1},
        {"1,000", -1},
        {std::string_view("1\0"
                          "2",
                          3),
         -1},
    };
    for (const auto& input : cases) {
        int actual = -1;
        if (uishortcuts::ParseTradeAmount(input.text, actual) != (input.expected >= 0) || actual != input.expected) {
            printf("FAIL parsing %.*s\n", static_cast<int>(input.text.size()), input.text.data());
            return 1;
        }
    }
    for (int i = 1; i < argc; ++i) {
        HMODULE image = LoadLibraryExA(argv[i], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!image) {
            printf("FAIL mapping %s (%lu)\n", argv[i], GetLastError());
            return 1;
        }
        auto* base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(image) & ~uintptr_t(3));
        auto img = locate::Describe(base);
        uishortcuts::TradeLocations trade;
        uishortcuts::TradeControlLocations controls;
        uishortcuts::MarketAmountBackgroundLocations marketBackground;
        uishortcuts::QueueLocations queue;
        locate::MultiplayerLocations multiplayer;
        std::string error;
        bool ok = uishortcuts::LocateTrade(img, trade, error) &&
                  uishortcuts::LocateTradeControls(img, trade, controls, error) &&
                  uishortcuts::LocateMarketAmountBackground(img, controls, marketBackground, error) &&
                  uishortcuts::LocateQueue(img, queue, error) && locate::LocateMultiplayer(img, multiplayer, error) &&
                  locate::FindOne(img, "PostCommandToSession", uishortcuts::kPostCommand, error);
        if (ok && queue.arrayToType != 0x34) {
            error = "unexpected queue array-to-type offset";
            ok = false;
        }
        if (ok && (controls.marketAmount != 0x608 || controls.marketLabel != 0x5f0 ||
                   controls.marketDecrease != 0x5d8 || controls.marketIncrease != 0x5e0 ||
                   controls.marketResource != 0x5c0 || controls.marketMode != 0x5b8 || controls.marketId != 0x610 ||
                   controls.eventType != 0xd8 || controls.mouseAction != 0x38 || controls.mouseButton != 0x3c ||
                   controls.eventTaken != 0xdc || controls.dialogRoot != 0x78 || controls.findEditBox != 0xd8 ||
                   controls.rowRoot != 0x70 || controls.findTextBox != 0xd0 || marketBackground.marketRoot != 0x78)) {
            error = "unexpected trade control offsets";
            ok = false;
        }
        // Text selection: the edit box handler, its vftable entry and the text buffer's selection calls.
        textselect::Locations text;
        if (ok && !textselect::Locate(img, text, error))
            ok = false;
        if (ok && text.slotCount != 2)
            error = "expected CEditBox's and CNullEditBox's HandleInput entries", ok = false;
        if (ok && (text.updateCursor == text.handleInput || text.setSelection == text.select))
            error = "text selection targets coincide", ok = false;
        if (ok)
            printf("text selection: HandleInput %zx slot %zx Select %zx SetSelection %zx UpdateCursor %zx\n",
                   static_cast<size_t>(text.handleInput - base),
                   static_cast<size_t>(reinterpret_cast<uint8_t*>(text.handleInputSlots[0]) - base),
                   static_cast<size_t>(text.select - base), static_cast<size_t>(text.setSelection - base),
                   static_cast<size_t>(text.updateCursor - base));
        FreeLibrary(image);
        printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", argv[i], ok ? "" : ": ", error.c_str());
        if (!ok)
            return 1;
    }
    puts("PASS trade amount parsing");
}
