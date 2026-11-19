// Install the real UI/template hooks into relocated private copies. No game code runs.
#include "../src/dllmain.cpp"

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    auto mod = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod)
        return 2;
    auto* mapped = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod) & ~uintptr_t(3));
    const auto* sourceNt =
        reinterpret_cast<IMAGE_NT_HEADERS64*>(mapped + reinterpret_cast<IMAGE_DOS_HEADER*>(mapped)->e_lfanew);
    const auto self = GetModuleHandleW(nullptr);
    const auto ini = IniPath(self);
    int failures = 0;
    InitializeCriticalSection(&g_logLock);
    g_log = stdout;
    for (int otherUi : {0, 1}) {
        auto* base = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, sourceNt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!base)
            return 2;
        memcpy(base, mapped, sourceNt->OptionalHeader.SizeOfHeaders);
        const auto* section = IMAGE_FIRST_SECTION(sourceNt);
        for (unsigned i = 0; i < sourceNt->FileHeader.NumberOfSections; ++i)
            memcpy(base + section[i].VirtualAddress, mapped + section[i].VirtualAddress, section[i].Misc.VirtualSize);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto delta = reinterpret_cast<uintptr_t>(base) - nt->OptionalHeader.ImageBase;
        const auto& reloc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        size_t offset = 0;
        while (offset < reloc.Size) {
            auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(base + reloc.VirtualAddress + offset);
            if (block->SizeOfBlock < sizeof(*block) || block->SizeOfBlock > reloc.Size - offset)
                return 2;
            auto* entries = reinterpret_cast<uint16_t*>(block + 1);
            for (size_t j = 0; j < (block->SizeOfBlock - sizeof(*block)) / 2; ++j)
                if ((entries[j] >> 12) == IMAGE_REL_BASED_DIR64)
                    *reinterpret_cast<uintptr_t*>(base + block->VirtualAddress + (entries[j] & 0xfff)) += delta;
            offset += block->SizeOfBlock;
        }
        nt->OptionalHeader.ImageBase = reinterpret_cast<uintptr_t>(base);
        g_base = reinterpret_cast<uintptr_t>(base);
        const auto image = locate::Describe(base);
        for (auto key : {"view_navigation", "trade_amount_entry", "planet_queue_first"})
            if (!WritePrivateProfileStringA("interface", key, otherUi ? "1" : "0", ini.c_str()))
                return 2;
        if (!WritePrivateProfileStringA("interface", "starbase_templates", "1", ini.c_str()))
            return 2;
        g_navigation = {};
        g_origNavigationReload = nullptr;
        g_tradeUi = {};
        g_tradeControls = {};
        g_tradeControlsReady = false;
        g_starbaseTemplatesOn = false;
        g_starbaseTemplateReader = {};
        g_starbaseDialogUi = {};
        g_uiPostCommand = nullptr;
        // Presence prerequisites are supplied; save callback behavior has its own runtime harness.
        g_storeOn = true;
        g_globalLoaded = true;
        g_mpStoreOn = false;
        g_origLoad = reinterpret_cast<GameStateLoadFn>(uintptr_t(1));
        g_origStartSave = reinterpret_cast<StartSaveFn>(uintptr_t(1));
        g_origZipAndWrite = reinterpret_cast<decltype(g_origZipAndWrite)>(uintptr_t(1));
        g_origCheckTick = reinterpret_cast<game::CheckTickFn>(uintptr_t(1));
        InstallViewNavigation(self, image);
        InstallTradeAmount(self, image);
        g_sideShowForSlot = nullptr;
        g_sideHide = nullptr;
        g_textChange = nullptr;
        g_buttonMoveTo = nullptr;
        g_windowGetSize = nullptr;
        g_waystationViewUpdate = nullptr;
        g_starbaseIcons = {};
        g_bench = {};
        g_platformUpgrade = {};
        std::string ticksError;
        if (!locate::LocateTicks(image, g_bench, ticksError)) {
            printf("ticks: %s\n", ticksError.c_str());
            return 2;
        }
        InstallStarbaseTemplates(self, image);
        // The ship design library builds on the starbase templates' buttons and name dialog.
        g_designLibOn = false;
        g_designLib = {};
        g_designLibrary.clear();
        g_str = {};
        std::string stringsError;
        if (!locate::LocateStrings(image, g_str, stringsError)) {
            printf("strings: %s\n", stringsError.c_str());
            return 2;
        }
        InstallShipDesignLibrary(self, image);
        InstallUiCommandShortcuts(self, image);
        const bool valid =
            g_starbaseTemplatesOn && g_starbaseViewUpdate && g_waystationViewUpdate && g_origNavigationReload &&
            g_sideShowForSlot && g_sideHide && g_textChange && g_buttonMoveTo && g_windowGetSize &&
            g_starbaseIcons.apply && g_autoUpgradeOn && g_platformUpgrade.vtable && g_reinforceOn &&
            g_calcBuildLocations && g_reinforce.shipsQueueRef &&
            *g_starbaseDialogUi.dialogAcceptSlot == reinterpret_cast<uintptr_t>(&StarbaseDialogAcceptHook) &&
            *g_starbaseDialogUi.dialogDestroySlot == reinterpret_cast<uintptr_t>(&StarbaseDialogDestroyHook) &&
            g_designLibOn && g_designLib.loadDesign && g_designLib.popupOk && g_designLib.templates &&
            (!otherUi || g_uiPostCommand);
        printf("starbase installation with other UI %d: %s\n", otherUi, valid ? "PASS" : "FAIL");
        failures += !valid;
        g_starbaseTemplatesOn = false;
        g_autoUpgradeOn = false;
        g_reinforceOn = false;
        g_designLibOn = false;
        g_reinforce = {};
        g_calcBuildLocations = nullptr;
        VirtualFree(base, 0, MEM_RELEASE);
    }
    g_log = nullptr;
    DeleteCriticalSection(&g_logLock);
    FreeLibrary(mod);
    return failures ? 1 : 0;
}
