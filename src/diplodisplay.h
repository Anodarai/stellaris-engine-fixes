#pragma once
#include "numericdisplay.h"
#include "diplomacy_patterns.h"
#include "diplotrigger_patterns.h"
namespace diplodisplay {
using C=numericpatch::Context;
struct Spec {int function;size_t call;int source,offset;};
// Source 0: decimal frame field; 1: R12; 2: R13; 3: *R13;
// 4: RBX with the native +50000 removed using unsigned arithmetic.
constexpr Spec specs[]={
    {0,0xc08,0,-0x18},{0,0xcdc,1,0},{0,0xe89,0,-8},{0,0xf4a,0,0x28},
    {0,0x1205,0,0},{0,0x12c2,0,8},{0,0x13c3,0,0x18},{0,0x1480,0,0x30},
    {0,0x162e,2,0},{0,0x16eb,0,0x38},{1,0x7f0,3,0},{1,0xe7c,0,0x1b0},{1,0x1561,4,0}};
constexpr size_t kArguments=sizeof(specs)/sizeof(specs[0]);
constexpr size_t kCount=kArguments+4;
struct Data {const Spec* spec=nullptr;numericdisplay::LocalizeFn localize=nullptr;numericpatch::Block* block=nullptr;};
struct Locations {numericpatch::Block blocks[kCount];Data data[kArguments];uint8_t* triggerReturn=nullptr;};
inline int64_t Rounded(int64_t raw){return numeric::MulDiv(raw,1,100000,numeric::Rounding::NearestAway);}
inline int64_t Raw(const C& c,const Spec& s){
    switch(s.source){case 0:return *reinterpret_cast<int64_t*>(c.rbp+s.offset);case 1:return int64_t(c.r12);
        case 2:return int64_t(c.r13);case 3:return *reinterpret_cast<int64_t*>(c.r13);default:return int64_t(c.rbx-50000);}
}
inline bool LocalizeChecked(numericdisplay::LocalizeFn fn,void* out,const numericdisplay::StringView* key,const char* name,const int64_t* value,void*& result){
    __try{result=fn(out,key,name,value);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
inline bool Argument(C& c,const void* opaque){
    auto& data=*static_cast<const Data*>(opaque);int64_t whole=Rounded(Raw(c,*data.spec));
    if(whole>=INT32_MIN && whole<=INT32_MAX)return false;
    // Decimal int64 raw values have at most 92 trillion whole units, so this
    // exact integer Q15 localization argument fits without further narrowing.
    int64_t binary=whole*32768;void* result=reinterpret_cast<void*>(c.rcx);bool writing=false;
    try{writing=true;if(!LocalizeChecked(data.localize,result,reinterpret_cast<const numericdisplay::StringView*>(c.rdx),
        reinterpret_cast<const char*>(c.r8),&binary,result))data.block->failed=true;
        c.rax=uint64_t(result);return true;
    }catch(...){if(writing){data.block->failed=true;c.rax=c.rcx;return true;}return false;}
}
inline bool BaseRound(C& c,const void*){c.r12=uint64_t(Rounded(*reinterpret_cast<int64_t*>(c.r12)));return true;}
inline bool BaseArgument(C& c,const void*){
    auto value=int64_t(c.r12);*reinterpret_cast<int32_t*>(c.rbp+0xc8)=int32_t(value);
    if(value<INT32_MIN || value>INT32_MAX){*reinterpret_cast<uint32_t*>(c.rbp+0x40)=8;*reinterpret_cast<int64_t*>(c.rbp+0xc0)=value*32768;}
    return true;
}
inline bool TriggerRound(C& c,const void*){
    // The caller's fourth home slot is eight bytes and is dead after this call.
    *reinterpret_cast<int64_t*>(c.Rsp()+0x98)=*reinterpret_cast<int64_t*>(c.rax)/100000;
    return true;
}
inline bool TriggerArgument(C& c,const void* opaque){
    const auto& l=*static_cast<const Locations*>(opaque);
    // This helper is shared by other triggers. Only our reviewed caller passes
    // an eight-byte value; every other caller keeps its original int32 ABI.
    if(*reinterpret_cast<uint8_t**>(c.rbp+0xc8)!=l.triggerReturn)return false;
    auto value=**reinterpret_cast<int64_t**>(c.rbp+0xf8);
    if(value>=INT32_MIN && value<=INT32_MAX)return false;
    *reinterpret_cast<uint32_t*>(c.rbp+0x20)=8;
    *reinterpret_cast<int64_t*>(c.rbp+0xa0)=value*32768;
    return true;
}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error,uint8_t* const* verified=nullptr){
    uint8_t* roots[2];
    for(int i=0;i<2;++i){roots[i]=verified?verified[i]:locate::FindOne(image,"diplomatic display owner",i?numericpatterns::kFinalWeight:numericpatterns::kBaseWeight,error);
        if(!roots[i])return false;}
    auto localize=locate::FindOne(image,"wide diplomatic localization",numericpatterns::kBinaryLocalize,error);if(!localize)return false;
    uint8_t* expected=nullptr;
    for(size_t i=0;i<kArguments;++i){const auto& s=specs[i];auto& b=l.blocks[i];b.at=roots[s.function]+s.call;
        if(b.at[0]!=0xe8){error="diplomatic display call changed";return false;}
        auto target=numericdisplay::Call(b.at);if(expected && expected!=target){error="diplomatic display helpers disagree";return false;}expected=target;
        b.stolen=b.span=5;b.ripOperand=1;b.handler=&Argument;l.data[i]={&s,reinterpret_cast<numericdisplay::LocalizeFn>(localize),&b};b.data=&l.data[i];}
    auto& round=l.blocks[kArguments];round.at=roots[0]+0x8e9;round.stolen=7;round.span=0x2e;round.handler=&BaseRound;
    auto& arg=l.blocks[kArguments+1];arg.at=roots[0]+0x97d;arg.stolen=arg.span=7;arg.handler=&BaseArgument;
    if(!locate::MatchAt(round.at,locate::Parse("49 8B 04 24 48 85 C0")) ||
       !locate::MatchAt(arg.at,locate::Parse("44 89 A5 C8 00 00 00"))){error="base diplomatic display argument changed";return false;}
    auto trigger=locate::FindOne(image,"diplomatic trigger tooltip",numericpatterns::kDiploTriggerTooltip,error);
    if(!trigger)return false;
    auto helper=numericdisplay::Call(trigger+0x117);
    if(!locate::MatchAt(helper,locate::Parse(numericpatterns::kCompareValueTooltip))){error="diplomatic trigger helper changed";return false;}
    l.triggerReturn=trigger+0x11c;
    auto& tr=l.blocks[kArguments+2];tr.at=trigger+0x2b;tr.stolen=13;tr.span=0x84;tr.handler=TriggerRound;
    auto& ta=l.blocks[kArguments+3];ta.at=helper+0x120;ta.stolen=7;ta.span=15;ta.handler=TriggerArgument;ta.data=&l;
    return true;
}
} // namespace diplodisplay
