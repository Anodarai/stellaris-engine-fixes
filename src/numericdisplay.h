// Preserve the engine's localized SI-number/boss/color formatting while removing int32 narrowing.
#pragma once
#include "numericmath.h"
#include "numericpatch.h"
#include "numericdisplay_patterns.h"
#include "numericsi_patterns.h"
#include <cstdio>
#include <algorithm>

namespace numericdisplay {
struct StringView { const char* text; int32_t length; uint32_t flags; };
using FormatFn = void*(__fastcall*)(void*,int64_t);
using LocalizeFn = void*(__fastcall*)(void*,const StringView*,const char*,const int64_t*);
struct RoundSpec { const char* name; const char* pattern; size_t begin,stolen,end,call; int input,flag; };
// Registers: input 0=RDX,1=RCX; flag 0=RSI,1=RAX,2=R14,3=RDI.
inline constexpr RoundSpec kRounds[] = {
    {"federation power",numericpatterns::kFederationRound,3,7,24,28,0,0},
    {"template power 1",numericpatterns::kTemplateRound1,3,6,23,27,1,1},
    {"template power 2",numericpatterns::kTemplateRound2,0,7,21,25,1,2},
    {"manager power",numericpatterns::kManagerRound,3,7,24,28,0,2},
    {"transport power",numericpatterns::kTransportRound,5,6,25,37,1,1},
    {"outliner power",numericpatterns::kOutlinerRound,3,7,24,36,1,3},
};
inline bool Round(numericpatch::Context& c,const void* data) {
    const auto& spec=*static_cast<const RoundSpec*>(data);
    int64_t raw=static_cast<int64_t>(spec.input ? c.rcx : c.rdx);
    uint64_t flag=raw<=0 ? 1 : 0;
    if(spec.flag==0) c.rsi=(c.rsi&~uint64_t(255))|flag;
    if(spec.flag==1) c.rax=(c.rax&~uint64_t(255))|flag;
    if(spec.flag==2) c.r14=(c.r14&~uint64_t(255))|flag;
    if(spec.flag==3) c.rdi=(c.rdi&~uint64_t(255))|flag;
    c.rdx=static_cast<uint64_t>(numeric::MulDiv(raw,1,32768,numeric::Rounding::NearestAway));
    return true;
}
struct Locations {
    uint8_t* format=nullptr;
    uint8_t* yellow=nullptr;
    LocalizeFn localize=nullptr;
    numericpatch::Block rounds[8];
};
// Native SI formatting truncates up to four decimal digits. Use unsigned magnitude,
// including INT64_MIN, and extend the same K/M/G sequence with T/P/E.
inline void FormatSI(char* out,size_t capacity,int64_t value,int precision) {
    static constexpr const char* suffix[]={"","K","M","G","T","P","E"};
    uint64_t magnitude=numeric::Magnitude(value),factor=1;
    size_t rank=0;
    while(magnitude/factor>=1000 && rank<6) {factor*=1000;++rank;}
    if(precision<0) precision=0;if(precision>4) precision=4;
    char text[64];
    if(!rank || !precision) snprintf(text,sizeof(text),"%s%llu%s",value<0?"-":"",
        static_cast<unsigned long long>(magnitude/factor),suffix[rank]);
    else {
        char fraction[24];snprintf(fraction,sizeof(fraction),"%0*llu",static_cast<int>(rank*3),
            static_cast<unsigned long long>(magnitude%factor));
        snprintf(text,sizeof(text),"%s%llu.%.*s%s",value<0?"-":"",
            static_cast<unsigned long long>(magnitude/factor),precision,fraction,suffix[rank]);
    }
    if(capacity) {size_t n=(std::min)(capacity-1,strlen(text));memcpy(out,text,n);out[n]=0;}
}
inline bool BinarySI(numericpatch::Context& c,const void*) {
    int64_t value=*reinterpret_cast<int64_t*>(c.rcx)/32768;
    if(value>INT32_MIN && value<=INT32_MAX) return false;
    FormatSI(reinterpret_cast<char*>(c.rdx),64,value,static_cast<int>(c.r9));return true;
}
inline bool DecimalSI(numericpatch::Context& c,const void*) {
    int64_t value=static_cast<int64_t>(c.rcx)/100000;
    if(value>INT32_MIN && value<=INT32_MAX) return false;
    FormatSI(reinterpret_cast<char*>(c.rdx),static_cast<size_t>(c.r8),value,static_cast<int>(c.r9));return true;
}
inline uint8_t* Call(uint8_t* instruction) { return locate::RelTarget(instruction+1,instruction+5); }
inline bool Locate(const locate::Image& image,Locations& l,std::string& error) {
    using namespace numericpatterns;
    l.format=locate::FindOne(image,"military SI formatter",kMilitaryFormat,error);
    auto helper=locate::FindOne(image,"binary localization argument",kBinaryLocalize,error);
    if(!l.format || !helper) return false;
    l.localize=reinterpret_cast<LocalizeFn>(helper);
    auto wrappers=locate::FindInCode(image,kYellowFormat,3);
    if(wrappers.size()!=2) { error="yellow wrapper signatures changed";return false; }
    for(auto p:wrappers) if(Call(p+0x2c)==l.format) {
        if(l.yellow) {error="ambiguous numeric yellow wrapper";return false;} l.yellow=p;
    }
    if(!l.yellow) {error="missing numeric yellow wrapper";return false;}
    // Audited direct-call inventory: five numeric callers plus the yellow wrapper;
    // eight yellow callers. Refuse to widen an ABI if the inventory changes.
    size_t direct=0,yellow=0;
    for(const auto& section:image.code) for(auto p=section.begin;p+5<=section.end;++p) if(*p==0xe8) {
        auto target=Call(p);direct+=target==l.format;yellow+=target==l.yellow;
    }
    if(direct!=6 || yellow!=8) {error="military formatter caller inventory changed";return false;}
    for(size_t i=0;i<6;++i) {
        const auto& s=kRounds[i];
        auto p=locate::FindOne(image,s.name,s.pattern,error);
        if(!p) return false;
        auto expected=(i==1 || i==2) ? l.format : l.yellow;
        if(p[s.call]!=0xe8 || Call(p+s.call)!=expected) {error="rounding/formatter call disagreement";return false;}
        auto& b=l.rounds[i]; b.at=p+s.begin;b.stolen=s.stolen;b.span=s.end-s.begin;
        b.handler=&Round;b.data=&s;
    }
    auto binary=locate::FindOne(image,"binary SI formatting",kBinarySI,error);
    auto decimal=locate::FindOne(image,"decimal SI formatting",kDecimalSI,error);
    if(!binary || !decimal) return false;
    auto& b=l.rounds[6];b.at=binary;b.stolen=7;b.span=7;b.handler=&BinarySI;b.returns=true;
    auto& d=l.rounds[7];d.at=decimal;d.stolen=5;d.span=5;d.handler=&DecimalSI;d.returns=true;
    return true;
}
inline void* Format(const Locations& l,FormatFn original,void* out,int64_t value) {
    if(value>=INT32_MIN && value<=INT32_MAX) return original(out,value);
    // Every audited caller supplies a whole Q15 value, hence at most 2^48 units.
    // The endpoint 2^48 rounds to the same localized SI text from INT64_MAX raw.
    int64_t raw=numeric::MulDiv(value,32768,1);
    const StringView key{"SI_NUMBER",9,0};
    return l.localize(out,&key,"VALUE",&raw);
}
} // namespace numericdisplay
