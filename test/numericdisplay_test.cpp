#include <array>
#include <cstdio>
#include <random>
#include "../src/numericdisplay.h"
namespace {
int failures=0,legacyCalls=0,wideCalls=0;
void Check(bool ok,const char* label) {if(!ok && ++failures<=20) printf("FAIL %s\n",label);}
void* __fastcall Legacy(void* out,int64_t value) {++legacyCalls;*static_cast<int64_t*>(out)=value;return out;}
void* __fastcall Wide(void* out,const numericdisplay::StringView* key,const char* name,const int64_t* raw) {
    ++wideCalls;Check(key->length==9 && !strcmp(key->text,"SI_NUMBER") && !strcmp(name,"VALUE"),"localized SI key retained");
    *static_cast<int64_t*>(out)=*raw;return out;
}
// Executable fixture invokes the real emitted stub with nonvolatile registers and XMM
// state live. Returning the transformed RDX exercises the register frame and continuation.
void StubTest(const numericdisplay::RoundSpec& spec,const numericpatch::Block& live) {
    auto memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!memory) {Check(false,"stub allocation");return;}
    numericpatch::Code pro;
    pro.Put({0x56,0x57,0x41,0x56}); // save RSI/RDI/R14; stack is aligned inside fixture
    pro.Put({0x48,0x8b,0xd1,0x33,0xf6,0x33,0xff,0x45,0x33,0xf6,0x33,0xc0});
    memcpy(memory,pro.b.data(),pro.b.size());size_t at=pro.b.size();
    memcpy(memory+at,live.at,live.span);
    const uint8_t tail[]={0x48,0x8b,0xc2,0x41,0x5e,0x5f,0x5e,0xc3};memcpy(memory+at+live.span,tail,sizeof(tail));
    memcpy(memory+2048,memory,at+live.span+sizeof(tail));
    numericpatch::Block block;block.at=memory+at;block.stolen=live.stolen;block.span=live.span;block.handler=&numericdisplay::Round;block.data=&spec;
    std::vector<uint8_t> jump;
    Check(numericpatch::Prepare(block,memory+1024,1024,jump),"prepare arithmetic stub");
    memcpy(block.at,jump.data(),jump.size());
    using Fn=int64_t(__fastcall*)(int64_t);
    auto run=reinterpret_cast<Fn>(memory);
    auto original=reinterpret_cast<Fn>(memory+2048);
    Check(original(INT64_MAX)<0 && run(INT64_MAX)>0,"original caller overflow reproduced");
    for(int64_t v:{INT64_MIN,INT64_MIN+1,INT64_MAX,INT64_MAX-1,-16384LL,-1LL,0LL,1LL,16384LL,INT64_C(1)<<48}) {
        Check(run(v)==numeric::MulDiv(v,1,32768,numeric::Rounding::NearestAway),"executed rounding stub boundary");
    }
    std::mt19937_64 rng(18);
    for(int n=0;n<20000;++n) {
        int64_t v=static_cast<int64_t>(rng()%20000000000000000ULL)-10000000000000000LL;
        int64_t expected=original(v);
        Check(run(v)==expected,"executed rounding stub ordinary parity");
    }
    block.failed.store(true);Check(run(32768)==1,"native displaced-instruction fallback");
    VirtualFree(memory,0,MEM_RELEASE);
}
void SITest(const numericpatch::Block& live,bool binary){
    char buffer[80];memset(buffer,'!',sizeof(buffer));
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,2048,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    if(!code){Check(false,"SI stub allocation");return;}
    memcpy(code,live.at,live.stolen);
    numericpatch::Block b;b.at=code;b.stolen=b.span=live.stolen;b.handler=live.handler;b.returns=true;
    std::vector<uint8_t> jump;Check(numericpatch::Prepare(b,code+512,1024,jump),"prepare SI leaf return");memcpy(code,jump.data(),jump.size());
    using Binary=void(__fastcall*)(int64_t*,char*,size_t,int);
    using Decimal=void(__fastcall*)(int64_t,char*,size_t,int);
    int64_t raw=INT64_MAX;
    if(binary)reinterpret_cast<Binary>(code)(&raw,buffer,64,1);
    else reinterpret_cast<Decimal>(code)(raw,buffer,64,1);
    Check(!strcmp(buffer,binary?"281.4T":"92.2T"),"executed SI full-width endpoint");
    Check(buffer[64]=='!',"SI output stays in buffer");
    raw=INT64_MIN;
    if(binary)reinterpret_cast<Binary>(code)(&raw,buffer,64,4);
    else reinterpret_cast<Decimal>(code)(raw,buffer,64,4);
    Check(!strcmp(buffer,binary?"-281.4749T":"-92.2337T"),"executed signed SI endpoint");
    numericdisplay::FormatSI(buffer,4,INT64_MIN,4);Check(!strcmp(buffer,"-9."),"SI capacity terminates safely");
    numericdisplay::FormatSI(buffer,64,INT64_MIN,4);Check(!strcmp(buffer,"-9.2233E"),"SI int64 magnitude");
    VirtualFree(code,0,MEM_RELEASE);
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    HMODULE mod=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!mod)return 2;
    auto img=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod)&~uintptr_t(3)));
    numericdisplay::Locations loc;std::string error;
    if(!numericdisplay::Locate(img,loc,error)) {puts(error.c_str());return 1;}
    loc.localize=&Wide;int64_t out=0;
    numericdisplay::Format(loc,&Legacy,&out,INT32_MAX);Check(legacyCalls==1 && out==INT32_MAX,"ordinary formatter unchanged");
    numericdisplay::Format(loc,&Legacy,&out,INT64_C(1)<<32);Check(wideCalls==1 && out==(INT64_C(1)<<47),"display above uint32");
    numericdisplay::Format(loc,&Legacy,&out,-(INT64_C(1)<<32));Check(out==-(INT64_C(1)<<47),"signed large display");
    numericdisplay::Format(loc,&Legacy,&out,INT64_C(1)<<48);Check(out==INT64_MAX,"rounded Q15 endpoint");
    for(size_t i=0;i<6;++i) StubTest(numericdisplay::kRounds[i],loc.rounds[i]);
    SITest(loc.rounds[6],true);SITest(loc.rounds[7],false);
    printf("display: six caller stubs, 120000 parity cases, signed boundaries; %d failures\n",failures);
    FreeLibrary(mod);return failures?1:0;
}
