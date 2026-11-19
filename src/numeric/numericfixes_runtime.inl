// The [numeric_fixes] features (re/numeric_overflow.md): the hooks and installers for the wide
// arithmetic in resourceoverflow.h, fixedpointoverflow.h, fleetoverflow.h, resourcearithmetic.h,
// resourceledger.h, numericdisplay.h, diplomacyoverflow.h, fleetstats.h, designcache.h,
// shipdamage.h, shipstats.h, componentmath.h, shipformula.h, incomelifetime.h, diplodisplay.h,
// diplovotes.h, federationsums.h, diploscores.h, countrytotals.h, incomeoverflow.h and
// forecastoverflow.h. Included by dllmain.cpp inside its anonymous namespace, after IniOn, Rva,
// AllocNear, WriteCode and InstallJumpHook.
//
// Every hook follows one pattern: compute the wide result under SEH; on a fault or an invalid
// state, switch the feature off from the next call and let the game's own code answer, unless the
// hook has already written part of the result (`writing`, `started`, `effectsStarted`).
//
// This file has the shared install helpers, fixed_point_conversion and military_power_display; the
// rest is in one part per folder, included at the end: economy/economy_runtime.inl,
// fleets/fleets_runtime.inl and diplomacy/diplomacy_runtime.inl.

// Whether a setting of [numeric_fixes] is on; they are all off unless set to 1.
bool NumericFix(HMODULE self, const char* key) {
    return IniOn(self, "numeric_fixes", key, false);
}
// The same for an installer's own setting: when it is off, the log says so under `component`.
bool NumericFix(HMODULE self, const char* key, const char* component) {
    return IniOnLogged(self, "numeric_fixes", key, false, component);
}

// Numeric blocks (numericpatch.h) that go in together. Prepare writes every block's stub and
// changes no game code; Write puts in every entry jump and, if one fails, takes back those it
// wrote; Restore takes back all it wrote. A feature installs its other hooks between the two.
class BlockInstall {
  public:
    void Add(numericpatch::Block& block) { blocks.push_back(&block); }
    template <class Range> void AddAll(Range& range) {
        for (auto& block : range)
            Add(block);
    }
    bool Prepare() {
        jumps.assign(blocks.size(), {});
        originals.assign(blocks.size(), {});
        for (size_t i = 0; i < blocks.size(); ++i) {
            numericpatch::Block& b = *blocks[i];
            uint8_t* stub = AllocNear(b.at, kStubSize);
            if (!stub || !numericpatch::Prepare(b, stub, kStubSize, jumps[i]))
                return false;
            originals[i].assign(b.at, b.at + b.stolen);
        }
        return true;
    }
    bool Write() {
        for (written = 0; written < blocks.size(); ++written) {
            if (!WriteCode(blocks[written]->at, jumps[written].data(), jumps[written].size())) {
                Restore();
                return false;
            }
        }
        return true;
    }
    void Restore() {
        for (; written > 0; --written) {
            WriteCode(blocks[written - 1]->at, originals[written - 1].data(), originals[written - 1].size());
        }
    }

  private:
    static constexpr size_t kStubSize = 1024;
    std::vector<numericpatch::Block*> blocks;
    std::vector<std::vector<uint8_t>> jumps, originals;
    size_t written = 0;
};

// Prepares and writes `install`, saying for `component` what failed; true once it is in.
bool InstallBlocks(BlockInstall& install, const char* component) {
    if (!install.Prepare()) {
        Log(LogLevel::Error, component, "cannot prepare stubs; off");
        return false;
    }
    if (!install.Write()) {
        Log(LogLevel::Error, component, "installation failed; off");
        return false;
    }
    return true;
}

// A jump hook that goes in with others: `stolen` bytes at `target` are replaced by a jump to `hook`.
struct JumpHook {
    uint8_t* target;
    size_t stolen;
    void* hook;
};

// Takes back the first `count` hooks. A trampoline begins with the bytes its hook replaced.
void RemoveJumpHooks(const JumpHook* hooks, size_t count, uint8_t* const* trampolines) {
    for (size_t i = 0; i < count; ++i) {
        WriteCode(hooks[i].target, trampolines[i], hooks[i].stolen);
    }
}

// Installs the hooks in order and stores each one's trampoline. If one fails, takes back those
// before it and returns false.
bool InstallJumpHooks(const JumpHook* hooks, size_t count, uint8_t** trampolines) {
    for (size_t i = 0; i < count; ++i) {
        trampolines[i] = InstallJumpHook(hooks[i].target, hooks[i].stolen, hooks[i].hook);
        if (!trampolines[i]) {
            RemoveJumpHooks(hooks, i, trampolines);
            return false;
        }
    }
    return true;
}

// ---- fixed_point_conversion ----

bool g_fixedConversionInstalled = false;

// `required`: diplomatic_weight_arithmetic needs the conversion whatever its own setting says.
bool InstallFixedPointConversion(HMODULE self, const locate::Image& img, bool required = false) {
    if (g_fixedConversionInstalled)
        return true;
    if (!required && !NumericFix(self, "fixed_point_conversion")) {
        Log(LogLevel::Info, "fixed_point_conversion", "off (fixed_point_conversion=0)");
        return false;
    }
    fixedpointoverflow::Locations loc{};
    std::string error;
    if (!fixedpointoverflow::Locate(img, loc, error)) {
        Log(LogLevel::Error, "fixed_point_conversion", "%s; off", error.c_str());
        return false;
    }
    auto function = fixedpointoverflow::FunctionPatch();
    auto inlined = fixedpointoverflow::InlinePatch();
    uint8_t* sites[] = {loc.function, loc.fleetWeight, loc.techWeight};
    const uint8_t* patches[] = {function.data(), inlined.data(), inlined.data()};
    size_t sizes[] = {function.size(), inlined.size(), inlined.size()};
    DWORD previous[3];
    int protectedCount = 0;
    for (; protectedCount < 3; ++protectedCount) {
        if (VirtualProtect(sites[protectedCount], sizes[protectedCount], PAGE_EXECUTE_READWRITE,
                           &previous[protectedCount]))
            continue;
        break;
    }
    // Acquire write access to all three ranges before changing any code.
    if (protectedCount == 3) {
        for (int i = 0; i < 3; ++i) {
            memcpy(sites[i], patches[i], sizes[i]);
            FlushInstructionCache(GetCurrentProcess(), sites[i], sizes[i]);
        }
    }
    for (int i = protectedCount - 1; i >= 0; --i) {
        DWORD ignored;
        VirtualProtect(sites[i], sizes[i], previous[i], &ignored);
    }
    if (protectedCount != 3) {
        Log(LogLevel::Error, "fixed_point_conversion", "cannot make all three ranges writable; off");
        return false;
    }
    Log(LogLevel::Info, "fixed_point_conversion",
        "checked int64 output at +%llX, fleet diplomatic weight +%llX, tech weight +%llX", Rva(loc.function),
        Rva(loc.fleetWeight), Rva(loc.techWeight));
    g_fixedConversionInstalled = true;
    return true;
}

// ---- military_power_display ----

numericdisplay::Locations g_numericDisplay;
numericdisplay::FormatFn g_originalMilitaryFormat = nullptr;
std::atomic<bool> g_numericDisplayFailed{false};

void* __fastcall MilitaryFormat64(void* out, int64_t value) {
    if (!g_numericDisplayFailed.load()) {
        __try {
            return numericdisplay::Format(g_numericDisplay, g_originalMilitaryFormat, out, value);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (!g_numericDisplayFailed.exchange(true))
                Log(LogLevel::Error, "military_power_display", "memory fault; native formatter from next call");
        }
    }
    return g_originalMilitaryFormat(out, value);
}

void InstallNumericDisplay(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "military_power_display")) {
        Log(LogLevel::Info, "military_power_display", "off (military_power_display=0)");
        return;
    }
    std::string error;
    if (!numericdisplay::Locate(img, g_numericDisplay, error)) {
        Log(LogLevel::Error, "military_power_display", "%s; off", error.c_str());
        return;
    }
    BlockInstall rounds;
    rounds.AddAll(g_numericDisplay.rounds);
    if (!rounds.Prepare()) {
        Log(LogLevel::Error, "military_power_display", "cannot prepare rounding stubs; off");
        return;
    }
    constexpr size_t kFormatStolen = 15; // the whole instructions the jump replaces at the formatter's start
    uint8_t* original = rounds.Write() ? InstallJumpHook(g_numericDisplay.format, kFormatStolen,
                                                         reinterpret_cast<void*>(&MilitaryFormat64))
                                       : nullptr;
    if (!original) {
        rounds.Restore();
        Log(LogLevel::Error, "military_power_display", "installation failed; off");
        return;
    }
    g_originalMilitaryFormat = reinterpret_cast<numericdisplay::FormatFn>(original);
    Log(LogLevel::Info, "military_power_display",
        "64-bit SI localization; six overflow-safe caller roundings; boss/color wrappers retained");
}

// The settings of each folder (economy/, fleets/, diplomacy/).
#include "economy/economy_runtime.inl"
#include "fleets/fleets_runtime.inl"
#include "diplomacy/diplomacy_runtime.inl"
