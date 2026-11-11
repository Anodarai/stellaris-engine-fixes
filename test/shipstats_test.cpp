#include <array>
#include <cstdio>
#include <random>
#include "../src/shipstats.h"
namespace {
int failures=0,sizeCalls=0,specialCalls=0,territoryCalls=0,virtualCalls=0;
void Check(bool ok,const char* label){if(!ok && ++failures<=20)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t offset,T value){memcpy(p+offset,&value,sizeof(value));}
void Jump(uint8_t* p,const void* target){uint8_t op[]={0xff,0x25,0,0,0,0};memcpy(p,op,6);memcpy(p+6,&target,8);}
uint8_t* __fastcall Size(uint8_t* design){++sizeCalls;return shipstats::Read<uint8_t*>(design,0x580);}
int __fastcall Special(uint8_t*,int id){++specialCalls;return id+1024;}
uint32_t __fastcall Territory(uint8_t* fleet){++territoryCalls;return shipstats::Read<uint32_t>(fleet,0x100);}
bool __fastcall Valid(uint8_t* object){++virtualCalls;return object[0x60]!=0;}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    shipstats::Locations l;std::string error;if(!shipstats::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,32768,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!code)return 2;
    struct Binding{uint8_t* from;uint8_t* to;};std::vector<Binding> bindings;
    auto bindCall=[&](auto& fn,auto replacement){auto at=code+24576+bindings.size()*32;Jump(at,reinterpret_cast<void*>(replacement));bindings.push_back({reinterpret_cast<uint8_t*>(fn),at});fn=replacement;};
    bindCall(l.size,&Size);bindCall(l.special,&Special);bindCall(l.territory,&Territory);bindings.push_back({l.combined,code+16384});
    auto bindCell=[&](const void* from,size_t offset){bindings.push_back({reinterpret_cast<uint8_t*>(const_cast<void*>(from)),code+offset});};
    bindCell(l.modifierTypes,20480);l.modifierTypes=reinterpret_cast<uint8_t**>(code+20480);
    std::vector<uint8_t> types(0x1000*0xb0);Set(code,20480,types.data());
    for(size_t i=0;i<3;++i){bindCell(l.dynamic[i],20512+i*64);l.dynamic[i]=reinterpret_cast<int*>(code+20512+i*64);for(size_t j=0;j<15;++j)Set(code,20512+i*64+j*4,int(600+i*16+j));}
    for(size_t i=0;i<4;++i){bindCell(l.growth[i],20800+i*8);l.growth[i]=reinterpret_cast<int64_t*>(code+20800+i*8);Set(code,20800+i*8,int64_t(i+1)*1000000);}
    uint8_t* functions[]={l.functions[0],l.functions[1],l.functions[2],l.combined};size_t sizes[]={numericpatterns::kShipMaxHullSize,numericpatterns::kShipMaxArmorSize,numericpatterns::kShipMaxShieldSize,numericpatterns::kCombinedShipModifierSize};
    for(size_t i=0;i<4;++i){auto target=i==3?code+16384:code+i*4096;auto original=functions[i];memcpy(target,original,sizes[i]);
        for(size_t off=0;off+4<=sizes[i];++off)for(const auto& b:bindings)if(shipstats::Relative(original,off)==b.from){int32_t rel=static_cast<int32_t>(b.to-target-off-4);memcpy(target+off,&rel,4);}}
    std::array<uint8_t,0x700> design{},size{},fleet{},modifiers{};
    Set(design.data(),0x580,size.data());void* vt[5]={nullptr,nullptr,nullptr,nullptr,reinterpret_cast<void*>(&Valid)};Set(fleet.data(),0x18,vt);
    std::vector<int> ids={0x1e,0x1f,0x2e,0x2f,0x24,0x25,0x1ca,0x1cb,0x1c8,0x1c9,0x1c6,0x1c7,0x1ed,0x1ee,0x1ef,0x1f0,0x1f1,0x1f2,0x1f3,0x201,0x202,0x203,0x1ff,0x200};
    size_t initial=ids.size();for(size_t i=0;i<initial;++i)ids.push_back(ids[i]+1024);for(int i=600;i<648;++i)ids.push_back(i);
    std::vector<int64_t> values(ids.size()*2);Set(modifiers.data(),0x10,ids.data());Set(modifiers.data(),0x1c,int(ids.size()));Set(modifiers.data(),0x38,values.data());
    auto set=[&](int id,int64_t value){for(size_t i=0;i<ids.size();++i)if(ids[i]==id)values[i*2]=value;};
    std::mt19937_64 rng(731893);
    for(int n=0;n<10000;++n){
        for(size_t i=0;i<ids.size();++i){values[i*2]=int64_t(rng()%300000)-100000;Set(types.data(),size_t(ids[i])*0xb0+0xa8,uint32_t(rng()%2)*32);}
        for(size_t offset:{size_t(0x10),size_t(0x130),size_t(0x70)})Set(design.data(),offset,int64_t(rng()%1000000000)-10000000);
        design[0x468]=rng()%15;Set(size.data(),0xc8,uint32_t(rng()%2)*0x4000);Set(size.data(),0x670,int64_t(rng()%6000000));fleet[0x78]=rng()%2;Set(fleet.data(),0x100,uint32_t(rng()%4));
        for(int kind=0;kind<3;++kind){int64_t native;
            sizeCalls=specialCalls=territoryCalls=virtualCalls=0;
            reinterpret_cast<shipstats::MaximumFn>(code+kind*4096)(&native,design.data(),modifiers.data(),fleet.data(),nullptr);
            auto sc=sizeCalls,pc=specialCalls,tc=territoryCalls,vc=virtualCalls;sizeCalls=specialCalls=territoryCalls=virtualCalls=0;
            auto corrected=shipstats::Maximum(l,kind,design.data(),modifiers.data(),fleet.data()).Clamp();
            Check(native==corrected,"full native maximum stat parity");Check(sc==sizeCalls && pc==specialCalls && tc==territoryCalls && vc==virtualCalls,"native maximum stat callee order/count");
        }
    }
    std::fill(values.begin(),values.end(),0);std::fill(types.begin(),types.end(),uint8_t(0));Set(size.data(),0xc8,uint32_t(0));fleet[0x78]=1;Set(fleet.data(),0x100,uint32_t(0));design[0x468]=0;Set(design.data(),0x10,int64_t(0));
    set(0x1e,INT64_MAX);set(0x1e+1024,INT64_MAX);set(0x1ca,-INT64_MAX);
    Check(shipstats::Maximum(l,0,design.data(),modifiers.data(),fleet.data()).Clamp()==INT64_MAX,"combined modifier overflow cancels before destination");
    std::fill(values.begin(),values.end(),0);Set(design.data(),0x10,int64_t(100000));set(0x1f,INT64_MAX);set(0x1f+1024,INT64_MAX);set(600,-INT64_MAX);set(0x1cb,-INT64_MAX);
    Check(shipstats::Maximum(l,0,design.data(),modifiers.data(),fleet.data()).Clamp()==100000,"wide multiplier cancellation precedes nonnegative clamp");
    std::fill(values.begin(),values.end(),0);Set(size.data(),0xc8,uint32_t(0x4000));Set(size.data(),0x670,INT64_MAX);set(0x203,INT64_MAX);set(0x2f,-100000);
    Check(shipstats::Maximum(l,1,design.data(),modifiers.data(),fleet.data()).Zero(),"zero multiplier cancels enormous bioship armor");
    printf("ship maximum stats: 30000 complete native fixtures, helper counts and exact cancellation; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);return failures?1:0;
}
