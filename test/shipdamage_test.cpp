#include <array>
#include <cstdio>
#include <random>
#include "../src/shipdamage.h"
namespace {
int failures=0;std::vector<int> calls;
void Check(bool ok,const char* label){if(!ok && ++failures<=20)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t offset,T value){memcpy(p+offset,&value,sizeof(value));}
void Jump(uint8_t* p,const void* target){uint8_t op[]={0xff,0x25,0,0,0,0};memcpy(p,op,6);memcpy(p+6,&target,8);}
uint8_t* __fastcall Size(uint8_t* design){calls.push_back(1);return shipstats::Read<uint8_t*>(design,0x580);}
int __fastcall Special(uint8_t*,int id){calls.push_back(2);return id+1024;}
uint32_t __fastcall Territory(uint8_t* fleet){calls.push_back(3);return shipstats::Read<uint32_t>(fleet,0x100);}
bool __fastcall Valid(uint8_t* object){calls.push_back(4);return object[0x60]!=0;}
uint8_t* __fastcall Owner(uint8_t* ship){calls.push_back(5);return shipstats::Read<uint8_t*>(ship,0x1100);}
uint8_t* __fastcall Federation(uint8_t* ref){calls.push_back(6);return *reinterpret_cast<uint8_t**>(ref);}
uint8_t* systemObject=nullptr;
uint8_t* expectedSystemRef=nullptr;
uint8_t* __fastcall System(uint8_t* ref){
    calls.push_back(7);
    Check(ref==expectedSystemRef,"system lookup receives the full reference pointer");
    return systemObject;
}
uint8_t* __fastcall SystemOwner(uint8_t* system){calls.push_back(8);return shipstats::Read<uint8_t*>(system,0x100);}
bool __fastcall Member(uint8_t* fed,uint8_t*){calls.push_back(9);return fed[0x61]!=0;}
bool __fastcall Home(uint8_t* ref,uint8_t*){calls.push_back(10);return *ref!=0;}
bool __fastcall SpecialTerritory(uint8_t* fleet){calls.push_back(11);return fleet[0x101]!=0;}
uint8_t* __fastcall SystemRef(uint8_t* ref){calls.push_back(12);return ref;}
uint8_t* __fastcall Definition(uint8_t* design){calls.push_back(13);return shipstats::Read<uint8_t*>(design,0x590);}
}
int main(int argc,char** argv){
    AddVectoredExceptionHandler(1,[](EXCEPTION_POINTERS* e)->LONG{printf("exception %lx RIP=%llx RCX=%llx RDX=%llx\n",e->ExceptionRecord->ExceptionCode,e->ContextRecord->Rip,e->ContextRecord->Rcx,e->ContextRecord->Rdx);fflush(stdout);return EXCEPTION_CONTINUE_SEARCH;});
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    shipdamage::Locations l;std::string error;if(!shipdamage::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,131072,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!code)return 2;
    printf("native fixture code=%p image=%p\n",code,image.base);fflush(stdout);
    struct Binding{const uint8_t* from;uint8_t* to;};std::vector<Binding> bindings;
    auto bindCall=[&](auto& fn,auto replacement){auto at=code+110000+bindings.size()*32;Jump(at,reinterpret_cast<void*>(replacement));bindings.push_back({reinterpret_cast<uint8_t*>(fn),at});fn=replacement;};
    bindCall(l.stats.size,&Size);bindCall(l.stats.special,&Special);bindCall(l.stats.territory,&Territory);
    bindCall(l.owner,&Owner);bindCall(l.federation,&Federation);bindCall(l.system,&System);bindCall(l.systemOwner,&SystemOwner);
    bindCall(l.member,&Member);bindCall(l.home,&Home);bindCall(l.specialTerritory,&SpecialTerritory);
    size_t cell=100000;
    auto cells=[&](const void* from,size_t bytes){auto result=code+cell;bindings.push_back({static_cast<const uint8_t*>(from),result});cell+=bytes;return result;};
    l.stats.modifierTypes=reinterpret_cast<uint8_t**>(cells(l.stats.modifierTypes,8));
    std::vector<uint8_t> types(0x1000*0xb0);*l.stats.modifierTypes=types.data();
    l.dynamic=reinterpret_cast<int*>(cells(l.dynamic,64));for(int i=0;i<16;++i)const_cast<int*>(l.dynamic)[i]=600+i;
    l.fleetDb=reinterpret_cast<uint8_t**>(cells(l.fleetDb,8));l.nullFleet=reinterpret_cast<uint8_t**>(cells(l.nullFleet,8));l.state=reinterpret_cast<uint8_t**>(cells(l.state,8));
    for(auto& define:l.components.defines)define=reinterpret_cast<int64_t*>(cells(define,8));
    uint8_t* functions[]={l.functions[0],l.functions[1],l.functions[2],l.stats.combined,l.components.functions[0],l.components.functions[1],l.components.modifier,l.components.mean};
    const size_t sizes[]={numericpatterns::kShipAverageDamageSize,numericpatterns::kComponentAdjustmentsSize,numericpatterns::kDamagePenaltySize,numericpatterns::kCombinedShipModifierSize,numericpatterns::kWeaponDamageSize,numericpatterns::kStrikeCraftDamageSize,numericpatterns::kComponentModifierSize,numericpatterns::kComponentRangeMeanSize};
    for(int i=0;i<8;++i)bindings.push_back({functions[i],code+i*8192});
    // Native __chkstk's ABI preserves RAX; fixture allocas are below one page.
    code[120000]=0xc3;bindings.push_back({shipstats::Relative(l.functions[1],0xe7),code+120000});
    for(int i=0;i<8;++i){auto target=code+i*8192;memcpy(target,functions[i],sizes[i]);
        for(size_t off=0;off+4<=sizes[i];++off)for(const auto& b:bindings)if(shipstats::Relative(functions[i],off)==b.from){int32_t rel=static_cast<int32_t>(b.to-target-off-4);memcpy(target+off,&rel,4);}}
    std::array<uint8_t,0x1200> ship{};std::array<uint8_t,0x800> design{},size{},fleet{},mods{},penaltyMods{},state{},definition{},section{},system{},federation{};
    std::array<uint8_t,0x3000> owner{},foreign{};systemObject=system.data();*l.state=state.data();Set(state.data(),0x290,penaltyMods.data());
    expectedSystemRef=ship.data()+0x20;
    Check(reinterpret_cast<uintptr_t>(expectedSystemRef)>UINT32_MAX,"system reference fixture exercises the high pointer bits");
    void* vt[9]{};vt[1]=reinterpret_cast<void*>(&Valid);vt[2]=reinterpret_cast<void*>(&Definition);vt[4]=reinterpret_cast<void*>(&Valid);vt[8]=reinterpret_cast<void*>(&Valid);
    void* refVt[2]={nullptr,reinterpret_cast<void*>(&SystemRef)};
    Set(ship.data(),0,vt);Set(ship.data(),0x20,refVt);Set(ship.data(),0x1100,owner.data());Set(ship.data(),0x458+0x580,size.data());
    Set(design.data(),0,vt);Set(design.data(),0x580,size.data());Set(design.data(),0x590,definition.data());
    Set(fleet.data(),0x18,vt);Set(owner.data(),0x18,vt);Set(foreign.data(),0x18,vt);Set(federation.data(),0,vt);Set(owner.data(),0x1c38,federation.data());
    *l.fleetDb=nullptr;*l.nullFleet=fleet.data();
    std::array<uint8_t,0x110> record{};Set(definition.data(),0x20,record.data());Set(definition.data(),0x2c,1);Set(design.data(),0x52c,0);
    uint8_t* sections[]={section.data()};Set(record.data(),0x18,sections);Set(record.data(),0x24,1);
    std::array<uint8_t,0x1400> weapon{},strike{};Set(weapon.data(),0,vt);Set(strike.data(),0,vt);weapon[0x60]=strike[0x60]=1;strike[0x1e1]=2;
    std::array<uint8_t,0xa0> slots{};uint8_t* componentList[]={weapon.data(),strike.data(),weapon.data(),strike.data(),weapon.data()};
    for(size_t i=0;i<5;++i)Set(slots.data(),i*0x20+0x10,componentList[i]);Set(section.data(),0x50,slots.data());Set(section.data(),0x5c,5);
    std::array<uint8_t,0x90> categories{};Set(categories.data(),0x38,700);Set(categories.data(),0x3c,701);Set(categories.data(),0x80,702);Set(categories.data(),0x84,703);
    for(auto c:{weapon.data(),strike.data()}){Set(c,0x890,categories.data());Set(c,0x89c,2);}
    std::vector<int> ids={0x3a,0x40,0x206,0x20a,0x6a,0x168,0x205,0x20b,0x41,0x1d2,0x1d3,0x230,700,701,702,703};
    size_t initial=ids.size();for(size_t i=0;i<initial;++i)ids.push_back(ids[i]+1024);for(int i=600;i<616;++i)ids.push_back(i);
    std::vector<int64_t> values(ids.size()*2),penalties(ids.size()*2);
    for(auto p:{mods.data(),penaltyMods.data()}){Set(p,0x10,ids.data());Set(p,0x1c,int(ids.size()));}Set(mods.data(),0x38,values.data());Set(penaltyMods.data(),0x38,penalties.data());
    std::mt19937_64 rng(991332);
    for(int n=0;n<12000;++n){
        for(size_t i=0;i<ids.size();++i){values[i*2]=int64_t(rng()%160000)-80000;penalties[i*2]=int64_t(rng()%100000)-100000;Set(types.data(),size_t(ids[i])*0xb0+0xa8,uint32_t(rng()%2)*32);}
        for(auto d:l.components.defines)*const_cast<int64_t*>(d)=int64_t(rng()%100000)+100000;
        for(auto c:{weapon.data(),strike.data()})for(size_t o=0x11c8;o<0x1330;o+=8)Set(c,o,int64_t(rng()%100000)+100000);
        Set(strike.data(),0x11c8,int(rng()%8));strike[0x11e0]=rng()%4;weapon[0x1330]=rng()%4;
        design[0x468]=rng()%15;Set(design.data(),0xd0,int64_t(rng()%100000000));Set(design.data(),0x380,int64_t(rng()%100000000));
        Set(size.data(),0xc8,uint32_t(rng()%2)*0x4000+uint32_t(rng()%2)*0x10000);
        ship[0x60]=rng()%2;fleet[0x78]=rng()%2;federation[0x60]=rng()%2;federation[0x61]=rng()%2;owner[0x78]=foreign[0x78]=rng()%2;
        owner[0x2e98]=rng()%2;Set(system.data(),0x100,n%2?owner.data():foreign.data());Set(fleet.data(),0x100,uint32_t(rng()%4));fleet[0x101]=rng()%2;
        Set(ship.data(),0xef8,int64_t(rng()%100000000));Set(ship.data(),0xa40,int64_t(rng()%100000000)-1000000);
        calls.clear();int64_t native;reinterpret_cast<shipdamage::MainFn>(code)(&native,design.data(),mods.data(),ship.data(),n%2!=0,nullptr);auto expectedCalls=calls;calls.clear();
        auto corrected=shipdamage::Damage(l,design.data(),mods.data(),ship.data(),n%2!=0).Clamp();
        if(native!=corrected && failures<3)printf("fixture %d native=%lld wide=%lld\n",n,native,corrected);
        Check(native==corrected,"whole native ship damage parity");Check(calls==expectedCalls,"ship damage preserves helper call order");
    }
    std::fill(values.begin(),values.end(),0);std::fill(types.begin(),types.end(),uint8_t(0));
    auto set=[&](int id,int64_t v){for(size_t i=0;i<ids.size();++i)if(ids[i]==id)values[i*2]=v;};
    set(700,INT64_MAX);set(702,INT64_MAX);set(701,-100000);set(703,0);
    auto adjustments=shipdamage::Adjustments(l,design.data(),mods.data(),false);
    auto expected=-(componentmath::Damage(l.components,weapon.data(),false,false)*numeric::Integer(3)+componentmath::Damage(l.components,strike.data(),true,false)*numeric::Integer(2));
    Check(adjustments==expected,"component multiplier zero cancels enormous first multiplier before narrowing");
    Set(ship.data(),0xef8,INT64_MAX);Set(ship.data(),0xa40,INT64_MAX);Check(shipdamage::Penalty(l,ship.data(),0x40).Zero(),"full huge hull has no damage penalty");
    printf("ship damage: 12000 full native fixtures, callback order, duplicate components and wide cancellation; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);return failures?1:0;
}
