#include <array>
#include <cstdio>
#include <random>
#include "../src/diplomacyoverflow.h"
namespace {
int failures=0;
void Check(bool ok,const char* label){if(!ok && ++failures<=25)printf("FAIL %s\n",label);}
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
void MultiplyTest(const diplomacyoverflow::MultiplySpec& spec,const numericpatch::Block& live){
    int before=failures;
    auto memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,16384,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    Check(memory!=nullptr,"allocate native fixture");if(!memory)return;
    auto body=Prologue();size_t at=body.b.size();body.b.insert(body.b.end(),live.at,live.at+live.span);Tail(body,spec.result);
    memcpy(memory,body.b.data(),body.b.size());memcpy(memory+4096,body.b.data(),body.b.size());
    numericpatch::Block block;block.at=memory+at;block.stolen=live.stolen;block.span=live.span;block.handler=live.handler;block.replay=live.replay;block.data=live.data;
    std::vector<uint8_t> jump;Check(numericpatch::Prepare(block,memory+8192,1024,jump),"prepare native multiply stub");
    memcpy(block.at,jump.data(),jump.size());
    using Fn=int64_t(__fastcall*)(C*);auto fixed=reinterpret_cast<Fn>(memory),original=reinterpret_cast<Fn>(memory+4096);
    auto setup=[&](int64_t a,int64_t b){C c{};for(auto reg:registers)if(reg)c.*reg=UINT64_C(0x29f16b11c6d1e109);c.*spec.a=a;if(spec.b)c.*spec.b=b;return c;};
    std::mt19937_64 random(0x981);
    for(int n=0;n<20000;++n){
        int64_t a=static_cast<int64_t>(random()%4000000001ULL)-2000000000LL;
        int64_t b=static_cast<int64_t>(random()%4000000001ULL)-2000000000LL;
        C c=setup(a,b);int64_t after=fixed(&c),prior=original(&c);
        if(after!=prior && failures==before)printf("block %zu +%llX: %lld * %lld, original=%lld fixed=%lld\n",spec.function,static_cast<unsigned long long>(spec.offset),a,b,prior,after);
        Check(after==prior,"diplomatic multiplication native parity");
    }
    for(auto pair: {std::pair<int64_t,int64_t>{100,-1000000000000000000LL},
        {INT64_MAX,100000},{INT64_MIN,100000},{INT64_MAX,200000},{INT64_MIN,-100000}}){
        C c=setup(pair.first,pair.second);Check(fixed(&c)==numeric::MulDiv(pair.first,spec.b?pair.second:1000000,100000),"diplomatic multiply boundary");
    }
    C c=spec.b?setup(100,-1000000000000000000LL):setup(INT64_MAX,1000000);Check(original(&c)!=fixed(&c),"original signed slow-path failure reproduced");
    block.failed=true;c=setup(120000,130000);Check(fixed(&c)==original(&c),"displaced native fallback");
    VirtualFree(memory,0,MEM_RELEASE);
}
void SumTests(){
    using namespace diplomacyoverflow;
    std::array<int64_t,128> stack{};int64_t result=0,base=INT64_MAX;
    C c{};c.rbp=reinterpret_cast<uint64_t>(stack.data()+32);c.r12=reinterpret_cast<uint64_t>(&result);
    Memory(c.rbp-0x18)=INT64_MAX;Memory(c.rbp-8)=-INT64_MAX;c.r13=1;
    SimpleSpec spec{};spec.kind=Kind::BaseTotal;SimpleData data{&spec,&base};
    Simple(c,&data);Check(result==INT64_MAX,"base sum saturates only after exact cancellation");
    c.r13=static_cast<uint64_t>(-1);Simple(c,&data);Check(result==INT64_MAX-1,"base cancellation retains low units");
    spec.kind=Kind::ModifierTotal;int64_t modifier=INT64_MAX;c.rax=reinterpret_cast<uint64_t>(&modifier);
    c.r14=INT64_MAX;c.r15=static_cast<uint64_t>(-INT64_MAX);c.r13=0;c.rbx=0;
    Memory(c.rbp+0x58)=-100000;Simple(c,&data);Check(static_cast<int64_t>(c.r8)==INT64_MAX,"modifier exact cancellation");
    spec.kind=Kind::BorrowTotal;result=INT64_MAX;c.r13=reinterpret_cast<uint64_t>(&result);c.rbx=INT64_MAX;c.rdx=static_cast<uint64_t>(-INT64_MAX);
    Simple(c,&data);Check(result==INT64_MAX,"borrow sum cancellation");
    spec.kind=Kind::Envoys;c.rax=INT32_MAX;c.rcx=INT32_MAX;Simple(c,&data);
    Check(c.rdi==UINT64_C(4294967294)*100000,"envoy count addition widened before conversion");
    spec.kind=Kind::EconomicConversion;c.r8=UINT64_C(1)<<46;Simple(c,&data);
    Check(c.rcx==UINT64_C(2147483648)*100000,"economic power conversion above int32");
}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;HMODULE mod=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!mod)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod)&~uintptr_t(3)));
    diplomacyoverflow::Locations loc;std::string error;
    if(!diplomacyoverflow::Locate(image,loc,error)){puts(error.c_str());return 1;}
    for(size_t i=0;i<diplomacyoverflow::kMultiplyCount;++i)MultiplyTest(diplomacyoverflow::kMultiply[i],loc.blocks[i]);
    SumTests();FreeLibrary(mod);printf("diplomacy: 19 native multiply blocks, 380000 parity cases, aggregate boundaries; %d failures\n",failures);return failures?1:0;
}
