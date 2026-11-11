#include <array>
#include <cstdio>
#include <random>
#include "../src/forecastoverflow.h"
namespace {
int failures=0;
void Check(bool b,const char* name){if(!b && ++failures<=20)printf("FAIL %s\n",name);}
using C=numericpatch::Context;
constexpr uint64_t C::* registers[]={&C::rax,&C::rcx,&C::rdx,&C::rbx,nullptr,&C::rbp,&C::rsi,&C::rdi,
    &C::r8,&C::r9,&C::r10,&C::r11,&C::r12,&C::r13,&C::r14,&C::r15};
numericpatch::Code Prologue(){
    numericpatch::Code c;c.Put({0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57});
    C sample{};
    for(uint8_t reg=0;reg<16;++reg)if(reg!=1 && reg!=4){
        uint8_t off=static_cast<uint8_t>(reinterpret_cast<uint8_t*>(&(sample.*registers[reg]))-reinterpret_cast<uint8_t*>(&sample));
        c.Put({static_cast<uint8_t>(0x48|(reg>=8?4:0)),0x8b,static_cast<uint8_t>(0x41|((reg&7)<<3)),off});
    }
    c.Put({0x48,0x8b,0x49,static_cast<uint8_t>(offsetof(C,rcx))});return c;
}
void Tail(numericpatch::Code& c,uint64_t C::* result){
    uint8_t reg=0;while(reg<16 && registers[reg]!=result)++reg;
    c.Put({static_cast<uint8_t>(0x48|(reg>=8?1:0)),0x8b,static_cast<uint8_t>(0xc0|(reg&7))});
    c.Put({0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0x5b,0xc3});
}

void Test(const numericpatch::Block& live,int index){
 auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));if(!code){Check(false,"allocate fixture");return;}
 auto body=Prologue();size_t at=body.b.size();body.b.insert(body.b.end(),live.at,live.at+live.span);Tail(body,&C::rax);
 memcpy(code,body.b.data(),body.b.size());memcpy(code+2048,body.b.data(),body.b.size());
 numericpatch::Block b;b.at=code+at;b.stolen=live.stolen;b.span=live.span;b.handler=live.handler;
 std::vector<uint8_t> jump;Check(numericpatch::Prepare(b,code+4096,1024,jump),"prepare forecast stub");memcpy(b.at,jump.data(),jump.size());
 using Fn=void(__fastcall*)(C*);auto native=reinterpret_cast<Fn>(code+2048),fixed=reinterpret_cast<Fn>(code);
 std::array<uint64_t,48> stack{};std::array<int64_t,4> values{},produce{},upkeep{},initial{},expected{};
 forecastoverflow::Array array{values.data(),4,4},prod{produce.data(),4,4},up{upkeep.data(),4,4};
 stack[0x40/8]=reinterpret_cast<uint64_t>(&array);stack[0x110/8]=reinterpret_cast<uint64_t>(&prod);stack[0x130/8]=reinterpret_cast<uint64_t>(&up);
 C c{};c.rbp=reinterpret_cast<uint64_t>(stack.data());c.rcx=reinterpret_cast<uint64_t>(values.data());
 std::mt19937_64 rng(3781);
 for(int n=0;n<20000;++n){for(int i=0;i<4;++i){initial[i]=static_cast<int64_t>(rng()%1000000000000ULL)-500000000000LL;produce[i]=static_cast<int64_t>(rng()%1000000000000ULL)-500000000000LL;upkeep[i]=static_cast<int64_t>(rng()%1000000000000ULL)-500000000000LL;}
  c.rax=static_cast<uint64_t>(produce[0]);values=initial;native(&c);expected=values;values=initial;fixed(&c);Check(expected==values,"native forecast arithmetic parity");}
 values.fill(INT64_MAX);produce.fill(INT64_MAX);upkeep.fill(0);c.rax=1;
 if(index==0){fixed(&c);Check(values[0]==INT64_MAX,"combined positive overflow");values.fill(INT64_MAX);upkeep.fill(INT64_MAX);fixed(&c);Check(values[0]==INT64_MAX,"combined cancellation before narrowing");}
 else {values.fill(INT64_MIN);fixed(&c);Check(values[0]==INT64_MIN,"signed forecast difference saturates");}
 b.failed=true;values=initial;native(&c);expected=values;values=initial;fixed(&c);Check(values==expected,"forecast native fallback");
 VirtualFree(code,0,MEM_RELEASE);
}
}
int main(int argc,char** argv){
 if(argc!=2)return 2;auto mod=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!mod)return 2;
 auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod)&~uintptr_t(3)));
 forecastoverflow::Locations l;std::string error;if(!forecastoverflow::Locate(image,l,error)){puts(error.c_str());return 1;}
 for(int i=0;i<3;++i)Test(l.blocks[i],i);FreeLibrary(mod);
 printf("forecast: 60000 native block fixtures, cancellation and boundaries; %d failures\n",failures);return failures?1:0;
}
