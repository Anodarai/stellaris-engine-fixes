#include <array>
#include <cstdio>
#include <random>
#include "../src/diplodisplay.h"
namespace {
int failures=0,calls=0;int64_t captured=0;void* observedOut=nullptr;const numericdisplay::StringView* observedKey=nullptr;const char* observedName=nullptr;
void Check(bool ok,const char* label){if(!ok && ++failures<20)printf("FAIL %s\n",label);}
void* __fastcall Localize(void* out,const numericdisplay::StringView* key,const char* name,const int64_t* value){
    ++calls;captured=*value;observedOut=out;observedKey=key;observedName=name;return out;}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    diplodisplay::Locations l;std::string error;if(!diplodisplay::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto baseRound=l.blocks[diplodisplay::kArguments].at;
    // Copy the native fixed-point rounding leaf selected independently through
    // its actual CALL in the base-weight tooltip's contribution path.
    auto base=baseRound-0x8e9;auto rounding=numericdisplay::Call(base+0xf03);
    const char* expected="48 8B 01 48 8D 88 B0 3C FF FF";
    Check(locate::MatchAt(rounding,locate::Parse(expected)),"rounded decimal helper call cross-check");
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    // Reviewed instruction boundaries: an immediate also contains byte C3.
    memcpy(code,rounding,0x32);
    using RoundFn=int64_t(__fastcall*)(const int64_t*);std::mt19937_64 rng(119);
    for(int n=0;n<100000;++n){int64_t raw=int64_t(rng()%9000000000000000000ULL);if(rng()&1)raw=-raw;
        Check(reinterpret_cast<RoundFn>(code)(&raw)==diplodisplay::Rounded(raw),"native ordinary rounding parity");}
    std::array<uint8_t,0x500> frame{};int64_t pointed=0;uint8_t output[0x30]{};
    numericdisplay::StringView key{"VALUE_TEST",10,0};const char* name="VALUE";
    for(size_t i=0;i<diplodisplay::kArguments;++i){auto& data=l.data[i];data.localize=Localize;
        for(int64_t raw:{int64_t(1234567),int64_t(-9876543),INT64_MAX,INT64_MIN,int64_t(214748364850000),int64_t(-214748364950000)}){
            numericpatch::Context c{};c.rbp=uint64_t(frame.data()+0x80);c.rcx=uint64_t(output);c.rdx=uint64_t(&key);c.r8=uint64_t(name);
            const auto& spec=*data.spec;
            if(spec.source==0)*reinterpret_cast<int64_t*>(c.rbp+spec.offset)=raw;
            if(spec.source==1)c.r12=uint64_t(raw);if(spec.source==2)c.r13=uint64_t(raw);
            if(spec.source==3){pointed=raw;c.r13=uint64_t(&pointed);}if(spec.source==4)c.rbx=uint64_t(raw)+50000;
            calls=0;bool changed=diplodisplay::Argument(c,&data);auto whole=diplodisplay::Rounded(raw);
            if(whole>=INT32_MIN && whole<=INT32_MAX)Check(!changed && calls==0,"ordinary localized arguments use native integer path");
            else Check(changed && calls==1 && captured==whole*32768 && observedOut==output && observedKey==&key && observedName==name && c.rax==uint64_t(output),"large localized arguments retain full value, key, destination and name");
        }
    }
    for(int64_t raw:{INT64_MAX,INT64_MIN,int64_t(150000)}){numericpatch::Context c{};c.r12=uint64_t(&raw);c.rbp=uint64_t(frame.data()+0x80);
        diplodisplay::BaseRound(c,nullptr);*reinterpret_cast<int32_t*>(c.rbp+0x40)=5;diplodisplay::BaseArgument(c,nullptr);
        auto whole=diplodisplay::Rounded(raw);bool wide=whole<INT32_MIN || whole>INT32_MAX;
        Check(*reinterpret_cast<int32_t*>(c.rbp+0x40)==(wide?8:5),"base tooltip promotes only oversized values");
        if(wide)Check(*reinterpret_cast<int64_t*>(c.rbp+0xc0)==whole*32768,"base tooltip stores full Q15 localization payload");}
    struct Stack {numericpatch::Context c;uint8_t bytes[0x200];} stack{};
    for(int64_t raw:{INT64_MAX,INT64_MIN,int64_t(150000)}){
        stack.c.rax=uint64_t(&raw);
        Check(numericpatch::Dispatch(&stack.c,&l.blocks[diplodisplay::kArguments+2]),"trigger rounding dispatch");
        auto whole=*reinterpret_cast<int64_t*>(stack.bytes+0x98);
        Check(whole==raw/100000 && numericpatch::dispatchStack==0,"dispatcher retains original stack and restores TLS");
        stack.c.rbp=uint64_t(frame.data()+0x80);
        *reinterpret_cast<uint8_t**>(stack.c.rbp+0xc8)=l.triggerReturn;
        *reinterpret_cast<int64_t**>(stack.c.rbp+0xf8)=&whole;
        bool wide=whole<INT32_MIN || whole>INT32_MAX;
        Check(numericpatch::Dispatch(&stack.c,&l.blocks[diplodisplay::kArguments+3])==wide,"trigger promotes only wide values");
        if(wide)Check(*reinterpret_cast<int64_t*>(stack.c.rbp+0xa0)==whole*32768,"trigger full localization payload");
        *reinterpret_cast<uint8_t**>(stack.c.rbp+0xc8)=nullptr;
        Check(!numericpatch::Dispatch(&stack.c,&l.blocks[diplodisplay::kArguments+3]),"other trigger callers preserve int32 ABI");
    }
    printf("diplomatic display: 100000 native rounding fixtures, 15 localized values and signed endpoints; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);return failures?1:0;
}
