#include <array>
#include <cstdio>
#include <random>
#include "../src/federationsums.h"
using C=numericpatch::Context;
uint8_t* Native(const numericpatch::Block& block,bool flags,int32_t* yes=nullptr,int32_t* no=nullptr){
    auto out=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    numericpatch::Code code;code.Put({0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57});
    constexpr uint64_t C::* regs[]={&C::rax,&C::rcx,&C::rdx,&C::rbx,nullptr,&C::rbp,&C::rsi,&C::rdi,
        &C::r8,&C::r9,&C::r10,&C::r11,&C::r12,&C::r13,&C::r14,&C::r15};C sample{};
    for(uint8_t reg=0;reg<16;++reg)if(reg!=1 && reg!=4){
        uint8_t offset=uint8_t(reinterpret_cast<uint8_t*>(&(sample.*regs[reg]))-reinterpret_cast<uint8_t*>(&sample));
        code.Put({uint8_t(0x48|(reg>=8?4:0)),0x8b,uint8_t(0x41|((reg&7)<<3)),offset});}
    size_t begin=code.b.size();code.b.insert(code.b.end(),block.at,block.at+block.span);
    if(yes)for(size_t i=0;i+4<=block.span;++i){auto target=diploscores::Relative(block.at,i);
        if(target==reinterpret_cast<uint8_t*>(yes) || target==reinterpret_cast<uint8_t*>(no)){
            int slot=target==reinterpret_cast<uint8_t*>(yes)?4000:4004;
            int32_t rel=int32_t(slot-begin-i-4);memcpy(code.b.data()+begin+i,&rel,4);}}
    if(flags)code.Put({0x0f,0x9d,0xc0});
    code.Put({0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0x5b,0xc3});
    memcpy(out,code.b.data(),code.b.size());if(yes){*reinterpret_cast<int32_t*>(out+4000)=*yes;*reinterpret_cast<int32_t*>(out+4004)=*no;}return out;
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    federationsums::Locations l;std::string error;if(!federationsums::Locate(image,l,error)){puts(error.c_str());return 1;}
    int failures=0;auto check=[&](bool ok,const char* label){if(!ok && ++failures<15)printf("FAIL %s\n",label);};
    struct Stack{numericpatch::Context c;uint8_t bytes[0x200];} stack{};
    std::array<uint8_t,0x500> frame{},member{};stack.c.rbp=uint64_t(frame.data()+0x80);
    int32_t accept=71,reject=-81;l.accept=&accept;l.reject=&reject;
    // Relocate the actual decision instructions and their native result globals.
    // The locator's pointers are captured again before replacing them for fixtures.
    federationsums::Locations nativeLocations;if(!federationsums::Locate(image,nativeLocations,error))return 2;
    auto nativeAccept=Native(nativeLocations.blocks[3],false);
    auto nativePredict=Native(nativeLocations.blocks[8],false,nativeLocations.accept,nativeLocations.reject);
    *reinterpret_cast<int32_t*>(nativePredict+4000)=accept;*reinterpret_cast<int32_t*>(nativePredict+4004)=reject;
    auto nativeType=Native(nativeLocations.blocks[15],true);
    using NativeFn=uint64_t(__fastcall*)(C*);
    auto run=[&](size_t index){return numericpatch::Dispatch(&stack.c,&l.blocks[index]);};
    using numeric::Integer;std::mt19937_64 rng(84692);
    for(int n=0;n<10000;++n){
        bool huge=n%2!=0;int64_t first=huge?INT64_MAX:int64_t(rng()%10000000);
        stack.c.rdi=uint64_t(first);run(4);Integer yes(first),total(first),unknown;
        for(int i=0;i<8;++i){int64_t value=huge?INT64_MAX:int64_t(rng()%20000000)-10000000;
            *reinterpret_cast<int64_t*>(stack.bytes+0x38)=value;stack.c.rbx=uint64_t(value);run(5);total+=Integer(value);
            if(i%3==0){run(6);unknown+=Integer(value);}if(i%3==1){run(7);yes+=Integer(value);}}
        bool unanimous=n%3==0;stack.c.rax=unanimous;
        int expected=unanimous?(yes+unknown<total?reject:yes<total?0:accept):
            (total<yes*Integer(2)?accept:total<(yes+unknown)*Integer(2)?0:reject);
        if(!huge){C native=stack.c;native.rsi=uint64_t(total.Clamp());native.rdi=uint64_t(yes.Clamp());native.rbx=uint64_t((yes+unknown).Clamp());
            check(int32_t(reinterpret_cast<NativeFn>(nativePredict)(&native))==expected,"native prediction parity");}
        check(run(8) && int32_t(stack.c.rax)==expected,"AI result keeps exact majority/unanimity");
        *reinterpret_cast<int64_t*>(stack.bytes+0x58)=first;run(0);yes=Integer(first);Integer no;
        for(int i=0;i<8;++i){int64_t v=huge?INT64_MAX:int64_t(rng()%20000000)-10000000;
            *reinterpret_cast<int64_t*>(stack.bytes+0x58)=v;run(i%2?1:2);if(i%2)yes+=Integer(v);else no+=Integer(v);}
        stack.c.rax=unanimous;
        if(!huge){C native=stack.c;native.rdi=uint64_t(yes.Clamp());native.rbx=uint64_t(no.Clamp());
            check((uint8_t(reinterpret_cast<NativeFn>(nativeAccept)(&native))!=0)==(unanimous?!(Integer()<no):no<yes),"native acceptance parity");}
        check(run(3) && (uint8_t(stack.c.rax)!=0)==(unanimous?!(Integer()<no):no<yes),"acceptance retains signed vote comparison");
        *reinterpret_cast<int64_t*>(stack.c.rbp+0x2d0)=first;run(12);yes=total=Integer(first);
        for(int i=0;i<8;++i){stack.c.rbx=uint64_t(first);run(14);total+=Integer(first);if(i%2){run(13);yes+=Integer(first);}}
        *reinterpret_cast<uint8_t*>(stack.c.rbp+0x2c8)=uint8_t(unanimous);
        auto needed=unanimous?total:componentmath::Divide(total,Integer(200000))+Integer(100);
        if(!huge){C native=stack.c;native.rdi=uint64_t(yes.Clamp());native.r15=uint64_t(total.Clamp());
            check((uint8_t(reinterpret_cast<NativeFn>(nativeType)(&native))!=0)==!(yes<needed),"native type threshold parity");}
        run(15);check(((stack.c.flags&0x80)!=0)==(yes<needed),"type view uses exact threshold");
    }
    run(9);stack.c.r14=uint64_t(member.data());member[0x1c0]=1;
    for(int i=0;i<3;++i){*reinterpret_cast<int64_t*>(stack.c.rbp+0x140)=INT64_MAX;run(10);}
    member[0x1c0]=0;run(10);stack.c.rbx=4;check(run(11),"law text replacement");
    check(std::string(reinterpret_cast<char*>(stack.c.rax))=="276701161105643 / 368934881474191" && stack.c.rdi==0x52,"law view retains full totals and color");
    check(federationsums::states.empty(),"all completed loops release their exact state");
    printf("federation sums: 30000 signed/overflow decision fixtures and full law text; %d failures\n",failures);
    VirtualFree(nativeAccept,0,MEM_RELEASE);VirtualFree(nativePredict,0,MEM_RELEASE);VirtualFree(nativeType,0,MEM_RELEASE);
    FreeLibrary(module);return failures?1:0;
}
