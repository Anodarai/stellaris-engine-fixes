// Exercise the numeric installers and file-lookup import hooks against a private
// copy of the mapped game image. No game code runs.
#include "../src/dllmain.cpp"
int main(int argc,char** argv){
    if(argc!=2)return 2;
    HMODULE self=GetModuleHandleW(nullptr);
    const char* options[]={"resource_amount_rounding","fixed_point_conversion","fleet_power_arithmetic",
        "resource_multiplication","stockpile_arithmetic","military_power_display","diplomatic_weight_arithmetic",
        "fleet_stat_arithmetic","income_forecast_arithmetic","income_target_arithmetic","income_production_arithmetic"};
    auto ini=IniPath(self);
    for(auto option:options)if(!WritePrivateProfileStringA("numeric_fixes",option,"1",ini.c_str()))return 2;
    if(!WritePrivateProfileStringA("speed","fast_file_lookups","1",ini.c_str()))return 2;
    auto mod=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!mod)return 2;
    auto mapped=reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod)&~uintptr_t(3));
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(mapped);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(mapped+dos->e_lfanew);
    // IMAGE_RESOURCE mappings are read-only file views. Install into a private image copy,
    // so protection changes work and no game executable on disk can be modified.
    auto base=static_cast<uint8_t*>(VirtualAlloc(nullptr,nt->OptionalHeader.SizeOfImage,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!base)return 2;memcpy(base,mapped,nt->OptionalHeader.SizeOfHeaders);
    auto section=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)
        memcpy(base+section[i].VirtualAddress,mapped+section[i].VirtualAddress,section[i].Misc.VirtualSize);
    g_base=reinterpret_cast<uintptr_t>(base);auto image=locate::Describe(base);
    InitializeCriticalSection(&g_logLock);g_log=stdout;
    InstallIncomeProduction(self,image);InstallIncomeArithmetic(self,image);InstallIncomeTarget(self,image);
    InstallResourceLedger(self,image);InstallResourceRounding(self,image);
    InstallDiplomacyArithmetic(self,image);InstallDiplomaticDisplay(self,image);InstallFixedPointConversion(self,image);
    InstallFleetStats(self,image);InstallFleetPowerArithmetic(self,image);InstallResourceMultiplication(self,image);
    InstallNumericDisplay(self,image);InstallCountryTotals(self,image);
    InstallDiplomaticVotes(self,image);InstallFederationSums(self,image);InstallDiplomaticScores(self,image);
    InstallShipFormula(self,image);
    InstallDesignCache(self,image);
    InstallShipDamage(self,image);
    InstallShipStats(self,image);
    InstallComponentDamage(self,image);
    InstallForecastArithmetic(self,image);
    InstallFileLookups(self);
    int failures=0;
    auto check=[&](bool valid,const char* name){if(!valid){++failures;printf("FAIL installed %s\n",name);}};
    check(g_ledgerOriginalAdd && g_ledgerOriginalRemove && g_ledgerOriginalConvert,"single stockpile hooks");
    check(g_fixedConversionInstalled,"fixed-point prerequisite");
    check(g_incomeArithmeticInstalled && g_originalIncomeCalculation,"exact income target");
    check(!g_incomeOn,"legacy shortcut superseded by exact income target");
    check(g_originalHull[0] && g_originalHull[1] && g_originalPoints[0] && g_originalPoints[1],"fleet points");
    check(g_originalFleetPower && g_originalHealthPower,"fleet power");
    check(g_originalResourceMultiply!=nullptr,"resource multiplication");
    check(g_originalMilitaryFormat!=nullptr,"military formatter");
    auto blocks=[&](numericpatch::Block* begin,size_t n){for(size_t i=0;i<n;++i)check(begin[i].at && begin[i].at[0]==0xe9,"arithmetic entry jump");};
    blocks(g_resourceLedger.batches,2);blocks(g_diplomacyOverflow.blocks,diplomacyoverflow::kBlockCount);
    blocks(g_numericDisplay.rounds,8);blocks(g_countryTotals.blocks,3);blocks(g_forecastOverflow.blocks,3);
    blocks(g_diploScores.blocks,diploscores::kBlockCount);
    blocks(g_diploVotes.blocks,7);
    blocks(g_federationSums.blocks,federationsums::kCount);
    blocks(g_diploDisplay.blocks,diplodisplay::kCount);
    blocks(g_incomeProduction.producers.blocks.data(),incomeproducers::kCount);blocks(g_incomeProduction.boundaries,6);
    for(auto original:g_incomeProduction.originals)check(original!=nullptr,"income production boundaries");
    blocks(&g_shipFormula.block,1);
    check(g_shipStats.originals[0] && g_shipStats.originals[1] && g_shipStats.originals[2],"ship maximum stats");
    blocks(g_componentMath.blocks,2);
    check(g_shipDamage.originals[0] && g_shipDamage.originals[1] && g_shipDamage.originals[2],"ship damage and helpers");
    for(int i=0;i<4;++i)check(g_designCache.originals[i]!=nullptr,"design cache entry hooks");
    check(g_designCache.originals[5]!=nullptr,"design modifier contributions");
    check(g_designCache.originals[6]!=nullptr,"grouped design descriptions");
    blocks(g_designCache.blocks,6);
    check(g_filesOn && g_origCreateFileW && g_origGetAttributes,"file lookup import hooks");
    g_log=nullptr;DeleteCriticalSection(&g_logLock);VirtualFree(base,0,MEM_RELEASE);FreeLibrary(mod);
    printf("combined installation: %d failures\n",failures);return failures?1:0;
}
