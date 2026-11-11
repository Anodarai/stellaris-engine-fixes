#include <array>
#include <cstdio>
#include <random>
#include "../src/diploscores.h"
namespace {
int failures=0,eligibilityCalls=0,happinessCalls=0,relationCalls=0;
void Check(bool ok,const char* label){if(!ok && ++failures<=20)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t offset,T value){memcpy(p+offset,&value,sizeof(value));}
void Jump(uint8_t* p,const void* target){uint8_t op[]={0xff,0x25,0,0,0,0};memcpy(p,op,6);memcpy(p+6,&target,8);}
bool __fastcall Valid(uint8_t* p){return p[0x40]!=0;}
bool __fastcall Eligible(uint8_t* p,uint8_t*){++eligibilityCalls;return p[0x10]!=0;}
bool __fastcall Happy(uint8_t* p){++happinessCalls;return p[0x50]!=0;}
std::array<uint8_t,0x120> relation{};
uint8_t* __fastcall Relation(uint8_t*,uint8_t*){++relationCalls;return relation.data();}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    diploscores::Locations l;std::string error;if(!diploscores::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,65536,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!code)return 2;
    struct Binding{uint8_t* from;uint8_t* to;};std::vector<Binding> bindings;
    auto cell=[&](const int64_t*& p,int64_t value){auto to=code+32768+bindings.size()*32;Set(to,0,value);bindings.push_back({reinterpret_cast<uint8_t*>(const_cast<int64_t*>(p)),to});p=reinterpret_cast<int64_t*>(to);};
    cell(l.techDivisor,3276800);cell(l.techBase,32768);cell(l.repeatWeight,65536);cell(l.economicDivisor,327680);
    cell(l.jobBase,100000);cell(l.happinessFactor,100000);cell(l.favorFactor,10000);cell(l.missingResource,0);
    std::array<uint8_t,0x4000> country{},owner{},moduleObject{},nullModuleObject{},lender{},pop{},job{};
    std::array<uint8_t,64> manager{},table{};std::array<int64_t,16> income{};
    std::array<std::array<uint8_t,0x600>,16> tech{};std::array<uint8_t,640> techEntries{};
    std::array<std::array<uint8_t,0x180>,16> resources{};std::array<uint8_t*,16> resourcePointers{};
    std::array<int32_t,8> pairs{};void* virtualTable[17]{};virtualTable[1]=reinterpret_cast<void*>(&Valid);virtualTable[16]=reinterpret_cast<void*>(&Valid);
    for(auto p:{owner.data(),moduleObject.data(),nullModuleObject.data(),pop.data()+8})Set(p,0,virtualTable);
    Set(owner.data(),0xaa8,moduleObject.data());Set(job.data(),0x18,owner.data());Set(job.data(),l.jobPops,pairs.data());Set(job.data(),l.jobPopCount,4);
    Set(pop.data(),0x10,int32_t(42));pairs={11,3,42,5,17,7,19,9};
    Set(country.data(),0x2a90,moduleObject.data());Set(moduleObject.data(),0x10,owner.data());Set(nullModuleObject.data(),0x10,owner.data());
    auto pointerCell=[&](uint8_t**& p,uint8_t* value){auto to=code+32768+bindings.size()*32;Set(to,0,value);bindings.push_back({reinterpret_cast<uint8_t*>(p),to});p=reinterpret_cast<uint8_t**>(to);};
    pointerCell(l.resources,manager.data());pointerCell(l.nullModule,nullModuleObject.data());
    auto bindCall=[&](auto& fn,auto replacement){auto to=code+40000+bindings.size()*32;Jump(to,reinterpret_cast<void*>(replacement));bindings.push_back({reinterpret_cast<uint8_t*>(fn),to});fn=replacement;};
    bindCall(l.eligible,&Eligible);bindCall(l.hasHappiness,&Happy);bindCall(l.relation,&Relation);
    Set(country.data(),l.technologies,techEntries.data());Set(country.data(),l.technologyCount,16);
    Set(country.data(),l.income,table.data());Set(table.data(),0,income.data());Set(table.data(),12,16);
    Set(manager.data(),8,resourcePointers.data());Set(manager.data(),0x14,16);
    for(size_t i=0;i<16;++i){Set(techEntries.data(),i*40+16,tech[i].data());resourcePointers[i]=resources[i].data();}
    const size_t sizes[]={numericpatterns::kTechScoreSize,numericpatterns::kEconomicScoreSize,numericpatterns::kJobWeightSize,numericpatterns::kLoanWeightSize};
    for(int fn=0;fn<4;++fn){auto original=l.blocks[fn].at;auto target=code+fn*4096;memcpy(target,original,sizes[fn]);
        for(size_t off=0;off+4<=sizes[fn];++off)for(const auto& b:bindings)if(diploscores::Relative(original,off)==b.from){int32_t rel=static_cast<int32_t>(b.to-target-off-4);memcpy(target+off,&rel,4);}
        auto& block=l.blocks[fn];block.at=code+16384+fn*2048;memcpy(block.at,target,sizes[fn]);
        // Rebase the relocated native copy again for the hooked copy/fallback path.
        for(size_t off=0;off+4<=sizes[fn];++off)for(const auto& b:bindings)if(diploscores::Relative(target,off)==b.to){int32_t rel=static_cast<int32_t>(b.to-block.at-off-4);memcpy(block.at+off,&rel,4);}
        std::vector<uint8_t> jump;Check(numericpatch::Prepare(block,code+24576+fn*1024,1024,jump),"prepare score entry stub");memcpy(block.at,jump.data(),jump.size());
    }
    using Fn=int64_t*(__fastcall*)(uint8_t*,int64_t*,uint8_t*,int32_t);
    std::mt19937_64 rng(0x35383);
    for(int n=0;n<12000;++n){
        for(size_t i=0;i<16;++i){Set(techEntries.data(),i*40+8,int32_t(rng()%80)-20);Set(tech[i].data(),0x68,int64_t(rng()%100000000));Set(tech[i].data(),0x588,int32_t(rng()%2)-1);
            resources[i][0x10]=rng()%2;Set(resources[i].data(),0x18,int32_t(rng()%20)-2);Set(resources[i].data(),0x158,int64_t(rng()%10000000)-100);Set(resources[i].data(),0x160,int64_t(rng()%10000000)-100);income[i]=int64_t(rng()%100000000)-10000000;}
        *const_cast<int64_t*>(l.repeatWeight)=int64_t(rng()%200000)-20000;
        owner[0x40]=rng()%2;moduleObject[0x40]=rng()%2;pop[0x48]=rng()%2;pop[0x50]=rng()%2;Set(owner.data(),0x928,uint32_t(rng()%16));
        pairs[3]=int32_t(rng()%10000)-100;Set(pop.data(),l.happiness,int64_t(rng()%300000)-100000);
        Set(relation.data(),0x108,int32_t(rng()%20));Set(lender.data(),0x2f70,int64_t(rng()%1000000000)-100000000);int32_t favors=int32_t(rng()%25)-3;
        for(int fn=0;fn<4;++fn){int64_t native=0,patched=0;auto first=fn==2?job.data():country.data();auto third=fn==2?pop.data():lender.data();
            eligibilityCalls=happinessCalls=relationCalls=0;reinterpret_cast<Fn>(code+fn*4096)(first,&native,third,favors);
            auto ec=eligibilityCalls,hc=happinessCalls,rc=relationCalls;eligibilityCalls=happinessCalls=relationCalls=0;
            reinterpret_cast<Fn>(l.blocks[fn].at)(first,&patched,third,favors);
            Check(native==patched,"full native diplomatic producer parity");Check(ec==eligibilityCalls && hc==happinessCalls && rc==relationCalls,"native predicate/callee counts");
        }
    }
    // Execute the displaced RIP-relative technology prologue through the fallback.
    l.blocks[0].failed=true;int64_t a,b;reinterpret_cast<Fn>(code)(country.data(),&a,nullptr,0);reinterpret_cast<Fn>(l.blocks[0].at)(country.data(),&b,nullptr,0);Check(a==b,"RIP-relative original fallback");l.blocks[0].failed=false;
    Set(country.data(),l.technologyCount,1);Set(techEntries.data(),8,INT32_MAX);Set(tech[0].data(),0x68,INT64_MAX);Set(tech[0].data(),0x588,0);
    Check(diploscores::Technology(l,country.data()).Clamp()==INT64_MAX,"wide cost times technology level");
    owner[0x40]=0;pop[0x48]=1;pop[0x50]=1;pairs[3]=INT32_MAX;*const_cast<int64_t*>(l.jobBase)=INT64_MAX;*const_cast<int64_t*>(l.happinessFactor)=100000;Set(pop.data(),l.happiness,int64_t(-100000));
    Check(diploscores::Job(l,job.data(),pop.data()).Zero(),"job overflow cancels with happiness before narrowing");
    Set(relation.data(),0x108,INT32_MAX);*const_cast<int64_t*>(l.favorFactor)=INT64_MAX;Set(lender.data(),0x2f70,int64_t(0));
    Check(diploscores::Loan(l,country.data(),lender.data(),INT32_MAX).Zero(),"zero weight cancels oversized loan factor");
    Set(lender.data(),0x2f70,INT64_MIN);Check(diploscores::Loan(l,country.data(),lender.data(),INT32_MAX).Clamp()==INT64_MIN,"negative loan endpoint");
    printf("diplomatic producers: 48000 full native comparisons, emitted stubs, predicates and extremes; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);return failures?1:0;
}
