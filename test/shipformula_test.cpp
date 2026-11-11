#include <array>
#include <cstdio>
#include <fstream>
#include <random>
#include "../src/shipformula.h"
namespace {
int failures=0;
void Check(bool ok,const char* label){if(!ok && ++failures<=20)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t offset,T value){memcpy(p+offset,&value,sizeof(value));}
numeric::Integer Parse(const std::string& s){numeric::Integer v;bool negative=!s.empty() && s[0]=='-';for(size_t i=negative?1:0;i<s.size();++i)v=v*numeric::Integer(10)+numeric::Integer(s[i]-'0');return negative?-v:v;}
void Oracle(const char* file){std::ifstream in(file);std::string a,b,c;size_t count=0;
    while(in>>a>>b>>c){Check(combatmath::Exp(Parse(a),Parse(b))==Parse(c),"independent exponential interval oracle");++count;}
    Check(in.eof() && count>0,"complete exponential oracle");printf("combat exponential: %zu independent cases\n",count);
}
void Native(const char* path){
    auto module=LoadLibraryExA(path,nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module){Check(false,"map game");return;}
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    shipformula::Locations l;std::string error;if(!shipformula::Locate(image,l,error)){puts(error.c_str());Check(false,"locate formula");return;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,16384,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!code){Check(false,"allocate code");return;}
    struct Binding{const uint8_t* from;uint8_t* to;};std::vector<Binding> bindings;
    uint8_t* originals[]={l.block.at,l.power,l.logarithm};
    size_t sizes[]={numericpatterns::kShipFormulaSize,numericpatterns::kCombatPowerSize,numericpatterns::kCombatLogSize};
    for(size_t i=0;i<3;++i)bindings.push_back({originals[i],code+i*2048});
    for(size_t i=0;i<6;++i){bindings.push_back({reinterpret_cast<const uint8_t*>(l.defines[i]),code+8192+i*8});l.defines[i]=reinterpret_cast<int64_t*>(code+8192+i*8);}
    for(size_t i=0;i<4;++i){bindings.push_back({reinterpret_cast<const uint8_t*>(l.constants[i]),code+8256+i*8});Set(code,8256+i*8,*l.constants[i]);}
    for(size_t fn=0;fn<3;++fn){auto target=code+fn*2048;memcpy(target,originals[fn],sizes[fn]);
        for(size_t off=0;off+4<=sizes[fn];++off)for(const auto& b:bindings)if(shipformula::Relative(originals[fn],off)==b.from){int32_t rel=static_cast<int32_t>(b.to-target-off-4);memcpy(target+off,&rel,4);}}
    l.block.at=code+6144;memcpy(l.block.at,code,sizes[0]);
    for(size_t off=0;off+4<=sizes[0];++off)for(const auto& b:bindings)if(shipformula::Relative(code,off)==b.to){int32_t rel=static_cast<int32_t>(b.to-l.block.at-off-4);memcpy(l.block.at+off,&rel,4);}
    std::vector<uint8_t> jump;Check(numericpatch::Prepare(l.block,code+10240,1024,jump),"prepare formula leaf hook");memcpy(l.block.at,jump.data(),jump.size());
    using Fn=int64_t*(__fastcall*)(int64_t*,const int64_t*,bool);
    auto native=reinterpret_cast<Fn>(code),patched=reinterpret_cast<Fn>(l.block.at);
    std::mt19937_64 rng(836239);std::array<int64_t,6> values{};
    for(int n=0;n<16000;++n){
        values={int64_t(rng()%50000)-25000,int64_t(rng()%10000)+128,int64_t(rng()%10000),int64_t(rng()%10000),int64_t(rng()%120),int64_t(rng()%2000)};
        for(size_t i=0;i<6;++i)*const_cast<int64_t*>(l.defines[i])=32768;
        *const_cast<int64_t*>(l.defines[4])=int64_t(rng()%180)-40;*const_cast<int64_t*>(l.defines[4])*=256;
        if(n%7==0)values[1]=-values[1];bool allow=n%2!=0;int64_t a,b;native(&a,values.data(),allow);patched(&b,values.data(),allow);
        Check(a==b,"full native military formula parity");
        // The native output may alias its first input, after all source reads complete.
        auto alias=values;patched(alias.data(),alias.data(),allow);Check(alias[0]==b,"formula input/output alias");
    }
    l.block.failed=true;int64_t a,b;native(&a,values.data(),false);patched(&b,values.data(),false);Check(a==b,"native formula fallback");l.block.failed=false;
    combatmath::Defines d{32768,32768,32768,32768,32768,32768};
    values={0,INT64_MAX,INT64_MAX,INT64_MAX,127,INT64_MAX};Check(combatmath::Formula(values.data(),false,d)==INT64_MAX,"extreme positive ship power");
    d.exponent=INT64_MIN;Check(combatmath::Formula(values.data(),false,d)==0,"extreme negative exponent terminates at zero");
    d.exponent=INT64_MAX;d.scale=-32768;Check(combatmath::Formula(values.data(),false,d)==INT64_MIN,"extreme signed power scale");
    d.scale=0;values[0]=INT64_MIN;Check(combatmath::Formula(values.data(),false,d)==INT64_MIN,"zero power scale preserves signed bonus");
    printf("ship formula: 16000 full native fixtures, alias/fallback and extreme exponents\n");
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);
}
}
int main(int argc,char** argv){if(argc==3 && !strcmp(argv[1],"--oracle"))Oracle(argv[2]);else if(argc==2)Native(argv[1]);else return 2;printf("%s (%d failures)\n",failures?"FAIL":"PASS",failures);return failures?1:0;}
