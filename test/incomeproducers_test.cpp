#include <cstdio>
#include <random>
#include "../src/incomeproducers.h"
namespace {
int failures=0;void Check(bool b,const char* label){if(!b && ++failures<20)printf("FAIL %s\n",label);}
using C=numericpatch::Context;
constexpr uint64_t C::* registers[]={&C::rax,&C::rcx,&C::rdx,&C::rbx,nullptr,&C::rbp,&C::rsi,&C::rdi,&C::r8,&C::r9,&C::r10,&C::r11,&C::r12,&C::r13,&C::r14,&C::r15};
numericpatch::Code Prologue(){numericpatch::Code code;code.Put({0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57});C sample{};
    for(uint8_t reg=0;reg<16;++reg)if(reg!=1 && reg!=4){uint8_t off=uint8_t(reinterpret_cast<uint8_t*>(&(sample.*registers[reg]))-reinterpret_cast<uint8_t*>(&sample));
        code.Put({uint8_t(0x48|(reg>=8?4:0)),0x8b,uint8_t(0x41|((reg&7)<<3)),off});}
    code.Put({0x48,0x8b,0x49,uint8_t(offsetof(C,rcx))});return code;}
C Inputs(int op,int64_t* at,int64_t delta){C c{};
    if(op==0){c.r8=uint64_t(at);c.rcx=uint64_t(delta);}
    else if(op==1){c.rdx=uint64_t(at);c.rax=uint64_t(delta);}
    else{c.rax=uint64_t(at);c.rcx=0;if(op==2)c.r8=uint64_t(delta);if(op==3)c.rbx=uint64_t(delta);if(op==4)c.rdi=uint64_t(delta);}
    return c;
}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    incomeaccumulator::Pass pass;incomeproducers::Locations l;std::string error;
    if(!incomeproducers::Locate(image,l,pass,error)){puts(error.c_str());return 1;}
    bool done[5]{};std::mt19937_64 rng(1911);
    for(size_t i=0;i<incomeproducers::kCount;++i){int op=l.operands[i].operation;if(done[op])continue;done[op]=true;
        auto memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,16384,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
        auto body=Prologue();size_t start=body.b.size();auto& live=l.blocks[i];body.b.insert(body.b.end(),live.at,live.at+live.span);
        body.Put({0x9c,0x58,0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0x5b,0xc3});
        memcpy(memory,body.b.data(),body.b.size());memcpy(memory+4096,body.b.data(),body.b.size());
        numericpatch::Block block;block.at=memory+start;block.stolen=block.span=live.stolen;block.handler=live.handler;block.data=live.data;
        std::vector<uint8_t> jump;Check(numericpatch::Prepare(block,memory+8192,1024,jump),"prepare atomic producer stub");memcpy(block.at,jump.data(),jump.size());
        using Fn=uint64_t(__fastcall*)(C*);auto fixed=reinterpret_cast<Fn>(memory),native=reinterpret_cast<Fn>(memory+4096);
        for(int n=0;n<20000;++n){int64_t old=int64_t(rng()%1000000000000ULL)-500000000000LL,delta=int64_t(rng()%1000000000000ULL)-500000000000LL;
            int64_t a=old,b=old;auto ca=Inputs(op,&a,delta),cb=Inputs(op,&b,delta);uint64_t flags=native(&ca);
            pass.Begin({{&b,1}});uint64_t actual=fixed(&cb);Check(pass.End(),"ordinary atomic residual representable");
            Check(a==b && (flags&0x8d5)==(actual&0x8d5),"native atomic result and all arithmetic flags parity");
            b=old;Check((fixed(&cb)&0x8d5)==(flags&0x8d5) && b==a,"unregistered atomic native fallback");}
        int64_t value=0;pass.Begin({{&value,1}});
        for(int64_t delta:{INT64_MAX,INT64_MAX,-INT64_MAX,-INT64_MAX,int64_t(39)}){auto c=Inputs(op,&value,delta);fixed(&c);}
        Check(value==39 && pass.End(),"atomic producers preserve cancellation across overflow");
        VirtualFree(memory,0,MEM_RELEASE);
    }
    printf("income producers: %zu unique sites, 100000 native atomic/flag/fallback fixtures; %d failures\n",incomeproducers::kCount,failures);
    FreeLibrary(module);return failures?1:0;
}
