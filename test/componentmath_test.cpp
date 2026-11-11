#include <array>
#include <cstdio>
#include <random>
#include "../src/componentmath.h"
namespace {
int failures=0;
void Check(bool ok,const char* label){if(!ok && ++failures<=20)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t offset,T value){memcpy(p+offset,&value,sizeof(value));}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    componentmath::Locations l;std::string error;if(!componentmath::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,32768,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!code)return 2;
    struct Binding{const uint8_t* from;uint8_t* to;};std::vector<Binding> bindings{{l.mean,code+8192}};
    for(size_t i=0;i<10;++i){bindings.push_back({reinterpret_cast<const uint8_t*>(l.defines[i]),code+12288+i*8});l.defines[i]=reinterpret_cast<int64_t*>(code+12288+i*8);}
    const size_t sizes[]={numericpatterns::kWeaponDamageSize,numericpatterns::kStrikeCraftDamageSize};
    memcpy(code+8192,l.mean,numericpatterns::kComponentRangeMeanSize);
    for(int fn=0;fn<2;++fn){auto target=code+fn*4096;auto original=l.functions[fn];memcpy(target,original,sizes[fn]);
        for(size_t off=0;off+4<=sizes[fn];++off)for(const auto& b:bindings)if(componentmath::Relative(original,off)==b.from){int32_t rel=static_cast<int32_t>(b.to-target-off-4);memcpy(target+off,&rel,4);}
        auto& block=l.blocks[fn];block.at=code+16384+fn*4096;memcpy(block.at,target,sizes[fn]);
        for(size_t off=0;off+4<=sizes[fn];++off)for(const auto& b:bindings)if(componentmath::Relative(target,off)==b.to){int32_t rel=static_cast<int32_t>(b.to-block.at-off-4);memcpy(block.at+off,&rel,4);}
        std::vector<uint8_t> jump;Check(numericpatch::Prepare(block,code+28672+fn*1024,1024,jump),"prepare component entry hook");memcpy(block.at,jump.data(),jump.size());}
    using Fn=int64_t*(__fastcall*)(uint8_t*,int64_t*,bool);
    std::array<uint8_t,0x1400> weapon{},strike{};std::mt19937_64 rng(73119);
    for(int n=0;n<16000;++n){
        for(size_t i=0;i<10;++i)*const_cast<int64_t*>(l.defines[i])=int64_t(rng()%200000)-30000;
        if(n%9==0)*const_cast<int64_t*>(l.defines[3])=0;
        for(auto object:{weapon.data(),strike.data()})for(size_t o=0x11c8;o<0x1330;o+=8)Set(object,o,int64_t(rng()%250000)-10000);
        Set(weapon.data(),0x11f8,int64_t(100000));Set(strike.data(),0x11c8,int(rng()%20));Set(strike.data(),0x11e8,int64_t(rng()%200000)+100000);
        weapon[0x1330]=rng()%4;strike[0x11e0]=rng()%4;
        for(int fn=0;fn<2;++fn){auto object=fn?strike.data():weapon.data();bool weighted=n%2!=0;int64_t native;
            reinterpret_cast<Fn>(code+fn*4096)(object,&native,weighted);
            int64_t corrected;auto returned=reinterpret_cast<Fn>(l.blocks[fn].at)(object,&corrected,weighted);
            Check(native==corrected && returned==&corrected,"full native component damage parity and hook ABI");}
    }
    for(int fn=0;fn<2;++fn){l.blocks[fn].failed=true;int64_t a,b;auto object=fn?strike.data():weapon.data();reinterpret_cast<Fn>(code+fn*4096)(object,&a,true);reinterpret_cast<Fn>(l.blocks[fn].at)(object,&b,true);Check(a==b,"native component trampoline fallback");l.blocks[fn].failed=false;}
    // Exercise the preserved coarse and split divide branches with small final quotients.
    for(int64_t denominator:{int64_t(200000),int64_t(10000000001LL),INT64_MAX}){
        Set(strike.data(),0x1250,INT64_MAX/8);Set(strike.data(),0x1258,int64_t(0));Set(strike.data(),0x11c8,1);Set(strike.data(),0x11e8,denominator);
        int64_t native;reinterpret_cast<Fn>(code+4096)(strike.data(),&native,false);Check(native==componentmath::Damage(l,strike.data(),true,false).Clamp(),"large native component divide rounding");
    }
    weapon.fill(0);Set(weapon.data(),0x1218,INT64_MAX);Set(weapon.data(),0x1220,INT64_MAX);Set(weapon.data(),0x11e0,int64_t(1000000));Set(weapon.data(),0x11f8,int64_t(100000));
    Check(componentmath::Damage(l,weapon.data(),false,false).Clamp()==INT64_MAX,"range mean keeps oversized damage positive");
    Set(weapon.data(),0x1258,int64_t(0));Check(componentmath::Damage(l,weapon.data(),false,true).Zero(),"zero weapon weight cancels oversized base damage");
    strike.fill(0);Set(strike.data(),0x1250,INT64_MAX);Set(strike.data(),0x1258,INT64_MAX);Set(strike.data(),0x11c8,INT32_MAX);Set(strike.data(),0x11e8,int64_t(100000));
    Check(componentmath::Damage(l,strike.data(),true,false).Clamp()==INT64_MAX,"wide strike-craft count and damage");
    printf("component damage: 32000 native fixtures, split/coarse division and extreme means/counts; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);return failures?1:0;
}
