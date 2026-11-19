// Part of numericfixes_runtime.inl: the economy's [numeric_fixes] settings. resource_amount_rounding
// (resourceoverflow.h), resource_multiplication (resourcearithmetic.h), stockpile_arithmetic
// (resourceledger.h), income_production_arithmetic (incomeatomic.h, incomeproducers.h,
// incomelifetime.h), income_target_arithmetic and income_forecast_arithmetic (incomeoverflow.h,
// forecastoverflow.h).

// ---- resource_amount_rounding ----

// Keep AddResource's whole-unit quotient in int64 (issue #3; re/numeric_overflow.md).
void InstallResourceRounding(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "resource_amount_rounding")) {
        Log(LogLevel::Info, "resource_rounding", "off (resource_amount_rounding=0)");
        return;
    }
    std::string error;
    uint8_t* at = resourceoverflow::Locate(img, error);
    if (!at) {
        Log(LogLevel::Error, "resource_rounding", "%s; off", error.c_str());
        return;
    }
    uint8_t patched[resourceoverflow::kSize];
    if (!resourceoverflow::Prepare(at, patched) || !WriteCode(at, patched, sizeof(patched))) {
        Log(LogLevel::Error, "resource_rounding", "could not patch both conversions; off");
        return;
    }
    Log(LogLevel::Info, "resource_rounding", "int64 whole units at +%llX (stockpile and related situations)", Rva(at));
}

// ---- resource_multiplication ----

resourcearithmetic::Locations g_resourceArithmetic;
resourcearithmetic::MultiplyFn g_originalResourceMultiply = nullptr;
std::atomic<bool> g_resourceMultiplyFailed{false};

void __fastcall ResourceMultiplyHook(uint8_t* table, const uint8_t* resource, const int64_t* multiplier) {
    if (!g_resourceMultiplyFailed.load(std::memory_order_relaxed)) {
        __try {
            resourcearithmetic::Multiply(g_resourceArithmetic, table, resource, multiplier);
            return;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!g_resourceMultiplyFailed.exchange(true))
            Log(LogLevel::Error, "resource_multiplication", "memory fault; off from next call");
    }
    g_originalResourceMultiply(table, resource, multiplier);
}

void InstallResourceMultiplication(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "resource_multiplication")) {
        Log(LogLevel::Info, "resource_multiplication", "off (resource_multiplication=0)");
        return;
    }
    std::string error;
    if (!resourcearithmetic::Locate(img, g_resourceArithmetic, error)) {
        Log(LogLevel::Error, "resource_multiplication", "%s; off", error.c_str());
        return;
    }
    auto original = InstallJumpHook(g_resourceArithmetic.multiply, resourcearithmetic::kStolen,
                                    reinterpret_cast<void*>(&ResourceMultiplyHook));
    if (!original) {
        Log(LogLevel::Error, "resource_multiplication", "cannot install hook; off");
        return;
    }
    g_originalResourceMultiply = reinterpret_cast<resourcearithmetic::MultiplyFn>(original);
    Log(LogLevel::Info, "resource_multiplication", "wide signed product at +%llX (CResourceTable::MultResource only)",
        Rva(g_resourceArithmetic.multiply));
}

// ---- stockpile_arithmetic ----

resourceledger::Locations g_resourceLedger;
resourceledger::ChangeFn g_ledgerOriginalAdd = nullptr, g_ledgerOriginalRemove = nullptr;
resourceledger::ConvertFn g_ledgerOriginalConvert = nullptr;
std::atomic<bool> g_ledgerFailed{false};

// SEH filter of the three stockpile hooks: logs the first fault in full and switches them off.
int LedgerFault(const char* operation, EXCEPTION_POINTERS* fault, const resourceledger::ChangeProgress& progress,
                void* economy, void* converter, void* table, void* resource, int64_t amount) {
    if (g_ledgerFailed.exchange(true))
        return EXCEPTION_EXECUTE_HANDLER;

    const auto& record = *fault->ExceptionRecord;
    MEMORY_BASIC_INFORMATION region{};
    char modulePath[MAX_PATH]{};
    const char* module = "unknown";
    uintptr_t offset = 0;
    if (VirtualQuery(record.ExceptionAddress, &region, sizeof(region)) && region.AllocationBase) {
        offset =
            reinterpret_cast<uintptr_t>(record.ExceptionAddress) - reinterpret_cast<uintptr_t>(region.AllocationBase);
        DWORD length = GetModuleFileNameA(static_cast<HMODULE>(region.AllocationBase), modulePath, MAX_PATH);
        if (length && length < MAX_PATH) {
            const char* slash = strrchr(modulePath, '\\');
            module = slash ? slash + 1 : modulePath;
        }
    }
    Log(LogLevel::Error, "stockpile",
        "fault: operation=%s stage=%s code=%08lX instruction=%p (%s+0x%llX) "
        "access=%llu address=%p economy=%p converter=%p table=%p resource=%p amount=%lld effects_started=%d",
        operation, progress.stage, record.ExceptionCode, record.ExceptionAddress, module,
        static_cast<unsigned long long>(offset),
        record.NumberParameters > 0 ? static_cast<unsigned long long>(record.ExceptionInformation[0]) : 0,
        record.NumberParameters > 1 ? reinterpret_cast<void*>(record.ExceptionInformation[1]) : nullptr, economy,
        converter, table, resource, static_cast<long long>(amount), int(progress.effectsStarted));
    Log(LogLevel::Error, "stockpile",
        "wide single-resource arithmetic off from next call; native fallback requires a valid slot");
    return EXCEPTION_EXECUTE_HANDLER;
}

// A change for a resource without a slot in the stockpile table is skipped, not handed to the
// game, whose code would write through that slot (resourceledger::skipped). The first is logged.
std::atomic<bool> g_ledgerSkipLogged{false};

void LedgerSkipped(const char* operation, const uint8_t* resource, int64_t amount) {
    if (g_ledgerSkipLogged.exchange(true))
        return;
    int index = -1;
    if (resource)
        ReadInt(resource + g_resourceLedger.resourceIndex, &index);
    Log(LogLevel::Error, "stockpile",
        "%s skipped: resource %p has no verified table entry (slot %d, amount %lld); "
        "later skips are only counted",
        operation, resource, index, static_cast<long long>(amount));
}

// Whether the game's own function may run for `resource`: only with a slot in the table.
bool LedgerCanReplay(const char* operation, const uint8_t* resource, int64_t amount, void* conversionTable = nullptr) {
    __try {
        int count =
            conversionTable ? resourceledger::Count(conversionTable) : resourceledger::StockpileCount(g_resourceLedger);
        if (resource && resourceledger::InTable(g_resourceLedger, resource, count))
            return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    resourceledger::skipped.fetch_add(1, std::memory_order_relaxed);
    LedgerSkipped(operation, resource, amount);
    return false;
}

void __fastcall LedgerAdd(uint8_t* economy, uint8_t* resource, int64_t amount) {
    resourceledger::ChangeProgress progress{};
    if (!g_ledgerFailed.load()) {
        bool done = false;
        __try {
            resourceledger::Change(g_resourceLedger, economy, resource, amount, false, progress);
            done = true;
        } __except (
            LedgerFault("add", GetExceptionInformation(), progress, economy, nullptr, nullptr, resource, amount)) {
        }
        if (done) {
            if (progress.skipped)
                LedgerSkipped("add", resource, amount);
            return;
        }
    }
    if (!progress.effectsStarted && LedgerCanReplay("add", resource, amount))
        g_ledgerOriginalAdd(economy, resource, amount);
}

void __fastcall LedgerRemove(uint8_t* economy, uint8_t* resource, int64_t amount) {
    resourceledger::ChangeProgress progress{};
    if (!g_ledgerFailed.load()) {
        bool done = false;
        __try {
            resourceledger::Change(g_resourceLedger, economy, resource, amount, true, progress);
            done = true;
        } __except (
            LedgerFault("remove", GetExceptionInformation(), progress, economy, nullptr, nullptr, resource, amount)) {
        }
        if (done) {
            if (progress.skipped)
                LedgerSkipped("remove", resource, amount);
            return;
        }
    }
    if (!progress.effectsStarted && LedgerCanReplay("remove", resource, amount))
        g_ledgerOriginalRemove(economy, resource, amount);
}

// Converts into a copy of the table and writes the copy back, so a fault leaves the table whole.
void LedgerConvertCopy(uint8_t* converter, void* table, uint8_t* resource, int64_t amount,
                       resourceledger::ChangeProgress& progress) {
    progress.stage = "copy conversion table";
    int count = resourceledger::Count(table);
    if (!resourceledger::InTable(g_resourceLedger, resource, count)) {
        progress.stage = "resource outside the conversion table; skipped";
        progress.skipped = true;
        resourceledger::skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    int64_t* destination = resourceledger::Values(table);
    std::vector<int64_t> values(destination, destination + count);
    resourceledger::Table copy(values.data(), count, g_resourceLedger.allocatorVtable);
    progress.stage = "calculate conversion";
    resourceledger::Convert(g_resourceLedger, converter, &copy, resource, amount);
    progress.stage = "write conversion table";
    progress.effectsStarted = true;
    memcpy(destination, values.data(), size_t(count) * sizeof(int64_t));
}

void __fastcall LedgerConvert(uint8_t* converter, void* table, uint8_t* resource, int64_t amount) {
    resourceledger::ChangeProgress progress{};
    if (!g_ledgerFailed.load()) {
        bool done = false;
        __try {
            LedgerConvertCopy(converter, table, resource, amount, progress);
            done = true;
        } __except (
            LedgerFault("convert", GetExceptionInformation(), progress, nullptr, converter, table, resource, amount)) {
        }
        if (done) {
            if (progress.skipped)
                LedgerSkipped("conversion", resource, amount);
            return;
        }
    }
    if (!progress.effectsStarted && table && LedgerCanReplay("conversion", resource, amount, table))
        g_ledgerOriginalConvert(converter, table, resource, amount);
}

void InstallResourceLedger(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "stockpile_arithmetic")) {
        Log(LogLevel::Info, "stockpile", "off (stockpile_arithmetic=0)");
        return;
    }
    std::string error;
    if (!resourceledger::Locate(img, g_resourceLedger, error)) {
        Log(LogLevel::Error, "stockpile", "%s; off", error.c_str());
        return;
    }
    BlockInstall batches;
    batches.AddAll(g_resourceLedger.batches);
    if (!batches.Prepare()) {
        Log(LogLevel::Error, "stockpile", "cannot prepare batch hooks; off");
        return;
    }
    const JumpHook hooks[] = {
        {g_resourceLedger.add, resourceledger::kStolen, reinterpret_cast<void*>(&LedgerAdd)},
        {g_resourceLedger.remove, resourceledger::kStolen, reinterpret_cast<void*>(&LedgerRemove)},
        {g_resourceLedger.convert, resourceledger::kStolen, reinterpret_cast<void*>(&LedgerConvert)},
    };
    uint8_t* originals[3]{};
    if (!InstallJumpHooks(hooks, 3, originals)) {
        g_ledgerFailed.store(true);
        Log(LogLevel::Error, "stockpile", "hook installation failed; off");
        return;
    }
    g_ledgerOriginalAdd = reinterpret_cast<resourceledger::ChangeFn>(originals[0]);
    g_ledgerOriginalRemove = reinterpret_cast<resourceledger::ChangeFn>(originals[1]);
    g_ledgerOriginalConvert = reinterpret_cast<resourceledger::ConvertFn>(originals[2]);
    if (!batches.Write()) {
        RemoveJumpHooks(hooks, 3, originals);
        Log(LogLevel::Error, "stockpile", "batch installation failed; off");
        return;
    }
    Log(LogLevel::Info, "stockpile",
        "checked single/batch add/remove/conversion; native capacity and situation handling retained");
}

#ifdef SEF_DEV_STATS
void LogStockpileStats() {
    uint64_t skipped = resourceledger::skipped.load();
    if (!skipped && !g_resourceLedger.batchChecksOnly.load())
        return;
    Log(LogLevel::Stats, "stockpile", "out_of_table_skipped=%llu batch_checks_only=%d skipped_batches=%llu",
        static_cast<unsigned long long>(skipped), int(g_resourceLedger.batchChecksOnly.load()),
        static_cast<unsigned long long>(resourceledger::skippedBatches.load()));
}
#endif

// ---- income_production_arithmetic ----

incomelifetime::Locations g_incomeProduction;
std::atomic<bool> g_incomeProductionFailed{false};
using IncomePassFn = void(__fastcall*)();
using IncomeDrainFn = void(__fastcall*)(uint8_t*, uint8_t*);
using IncomeReadFn = int64_t*(__fastcall*)(uint8_t*, int64_t*, uint8_t*, void*);

// One of the two production passes (originals[Kind]); the outermost one ends the wide pass.
template <int Kind> void __fastcall IncomeProductionPass() {
    auto& l = g_incomeProduction;
    if (!l.depth && g_incomeProductionFailed.load()) {
        for (auto& b : l.boundaries)
            b.failed = true;
        for (auto& b : l.producers.blocks)
            b.failed = true;
    }
    ++l.depth;
    reinterpret_cast<IncomePassFn>(l.originals[Kind])();
    if (--l.depth == 0 && l.pass.Active() && !l.pass.End())
        Log(LogLevel::Error, "income_production", "joined pass retained an unrepresentable undrained value");
}

bool DrainIncome(uint8_t* source, uint8_t* destination, bool categorized, bool& writing) {
    try {
        auto plan = incomeatomic::Plan(g_incomeProduction.tables, source, destination, categorized);
        incomeatomic::Drain(g_incomeProduction.pass, plan, writing);
        return true;
    } catch (...) {
        return false;
    }
}

// The plain (Kind 0) or categorized (Kind 1) drain, originals[2 + Kind].
template <int Kind> void __fastcall IncomeProductionDrain(uint8_t* source, uint8_t* destination) {
    if (g_incomeProductionFailed.load() && !g_incomeProduction.pass.Active()) {
        reinterpret_cast<IncomeDrainFn>(g_incomeProduction.originals[2 + Kind])(source, destination);
        return;
    }
    bool writing = false;
    __try {
        if (DrainIncome(source, destination, Kind != 0, writing))
            return;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_incomeProductionFailed = true;
    if (!writing)
        reinterpret_cast<IncomeDrainFn>(g_incomeProduction.originals[2 + Kind])(source, destination);
}

bool ReadIncome(uint8_t* source, int64_t* output, uint8_t* resource, void* tooltip, bool& writing) {
    try {
        auto value = (tooltip ? incomeatomic::FormattedResource(g_incomeProduction.tables, g_incomeProduction.pass,
                                                                source, resource, static_cast<uint8_t*>(tooltip))
                              : incomeatomic::ReadResource(g_incomeProduction.pass, source, resource))
                         .Clamp();
        // The result first: a fault in the native tooltip formatter must not leave *output unwritten.
        writing = true;
        *output = value;
        if (tooltip) {
            int64_t ignored;
            reinterpret_cast<IncomeReadFn>(g_incomeProduction.originals[4])(source, &ignored, resource, tooltip);
        }
        return true;
    } catch (...) {
        return false;
    }
}

int64_t* __fastcall IncomeProductionRead(uint8_t* source, int64_t* output, uint8_t* resource, void* tooltip) {
    if (g_incomeProductionFailed.load() && !g_incomeProduction.pass.Active())
        return reinterpret_cast<IncomeReadFn>(g_incomeProduction.originals[4])(source, output, resource, tooltip);
    bool writing = false;
    __try {
        if (ReadIncome(source, output, resource, tooltip, writing))
            return output;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_incomeProductionFailed = true;
    if (!writing)
        return reinterpret_cast<IncomeReadFn>(g_incomeProduction.originals[4])(source, output, resource, tooltip);
    return output;
}

void InstallIncomeProduction(HMODULE self, const locate::Image& image) {
    if (!NumericFix(self, "income_production_arithmetic", "income_production"))
        return;
    auto& l = g_incomeProduction;
    std::string error;
    if (!incomelifetime::Locate(image, l, error)) {
        Log(LogLevel::Error, "income_production", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.AddAll(l.producers.blocks);
    blocks.AddAll(l.boundaries);
    if (!blocks.Prepare()) {
        Log(LogLevel::Error, "income_production", "cannot prepare stubs; off");
        return;
    }
    // The second number of each entry is how many bytes of whole instructions the jump replaces
    // at that function's start.
    const JumpHook hooks[] = {
        {l.tables.roots[0], 14, reinterpret_cast<void*>(&IncomeProductionPass<0>)},
        {l.tables.roots[1], 18, reinterpret_cast<void*>(&IncomeProductionPass<1>)},
        {l.tables.drain, 14, reinterpret_cast<void*>(&IncomeProductionDrain<0>)},
        {l.tables.categorizedDrain, 14, reinterpret_cast<void*>(&IncomeProductionDrain<1>)},
        {l.tables.readResource, 14, reinterpret_cast<void*>(&IncomeProductionRead)},
    };
    if (!InstallJumpHooks(hooks, 5, l.originals)) {
        Log(LogLevel::Error, "income_production", "entry installation failed; off");
        return;
    }
    if (!blocks.Write()) {
        RemoveJumpHooks(hooks, 5, l.originals);
        Log(LogLevel::Error, "income_production", "atomic installation failed; off");
        return;
    }
    Log(LogLevel::Info, "income_production",
        "116 atomic additions, reads, stores, compare/subtract and drains within joined pass lifetimes");
}

// ---- income_target_arithmetic and income_forecast_arithmetic ----

forecastoverflow::Locations g_forecastOverflow;
incomeoverflow::Locations g_incomeArithmetic;
incomeoverflow::CalculateFn g_originalIncomeCalculation = nullptr;
std::atomic<bool> g_incomeArithmeticFailed{false};
bool g_incomeArithmeticInstalled = false; // InstallIncomeTarget stays out when this is in

bool CalculateIncomeExact(uint8_t* plan, incomeoverflow::Table* out, void* scope, const incomeoverflow::Table* target,
                          int mode, bool& writing) {
    try {
        incomeoverflow::Calculate(g_incomeArithmetic, plan, out, scope, target, mode, writing);
        return true;
    } catch (const std::exception& e) {
        Log(LogLevel::Error, "income_target_arithmetic", "%s", e.what());
        return false;
    }
}

incomeoverflow::Table* __fastcall IncomeCalculationHook(uint8_t* plan, incomeoverflow::Table* out, void* scope,
                                                        const incomeoverflow::Table* target, int mode) {
    bool writing = false;
    if (!g_incomeArithmeticFailed.load()) {
        __try {
            if (CalculateIncomeExact(plan, out, scope, target, mode, writing))
                return out;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!g_incomeArithmeticFailed.exchange(true))
            Log(LogLevel::Error, "income_target_arithmetic", "invalid state or memory fault; off from next call");
    }
    // Never allocate/write a second result over a partially constructed native table.
    return writing ? out : g_originalIncomeCalculation(plan, out, scope, target, mode);
}

void InstallIncomeArithmetic(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "income_target_arithmetic", "income_target_arithmetic"))
        return;
    std::string error;
    if (!incomeoverflow::Locate(img, g_incomeArithmetic, error)) {
        Log(LogLevel::Error, "income_target_arithmetic", "%s; off", error.c_str());
        return;
    }
    auto original = InstallJumpHook(g_incomeArithmetic.calculate, incomeoverflow::kStolen,
                                    reinterpret_cast<void*>(&IncomeCalculationHook));
    if (!original) {
        Log(LogLevel::Error, "income_target_arithmetic", "installation failed; off");
        return;
    }
    g_originalIncomeCalculation = reinterpret_cast<incomeoverflow::CalculateFn>(original);
    g_incomeArithmeticInstalled = true;
    Log(LogLevel::Info, "income_target_arithmetic",
        "exact recursive totals/counts; unreachable scaling targets stop after one round");
}

void InstallForecastArithmetic(HMODULE self, const locate::Image& img) {
    if (!NumericFix(self, "income_forecast_arithmetic", "income_forecast"))
        return;
    std::string error;
    if (!forecastoverflow::Locate(img, g_forecastOverflow, error)) {
        Log(LogLevel::Error, "income_forecast", "%s; off", error.c_str());
        return;
    }
    BlockInstall blocks;
    blocks.AddAll(g_forecastOverflow.blocks);
    if (!InstallBlocks(blocks, "income_forecast"))
        return;
    Log(LogLevel::Info, "income_forecast", "wide income + production - upkeep; checked target differences");
}
