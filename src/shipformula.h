#pragma once
#include "combatmath.h"
#include "numericpatch.h"
#include "shipformula_patterns.h"
namespace shipformula {
struct Locations {
    numericpatch::Block block;uint8_t* power=nullptr;uint8_t* logarithm=nullptr;
    const int64_t* defines[6]{};const int64_t* constants[4]{};
};
inline bool Hook(numericpatch::Context& c,const void* data){
    try {
        const auto& l=*static_cast<const Locations*>(data);
        combatmath::Defines d{*l.defines[0],*l.defines[1],*l.defines[2],*l.defines[3],*l.defines[4],*l.defines[5]};
        int64_t value=combatmath::Formula(reinterpret_cast<const int64_t*>(c.rdx),(c.r8&255)!=0,d);
        *reinterpret_cast<int64_t*>(c.rcx)=value;c.rax=c.rcx;return true;
    }catch(...){return false;}
}
inline uint8_t* Relative(uint8_t* p,size_t o){return locate::RelTarget(p+o,p+o+4);}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    auto p=locate::FindOne(image,"shared ship military formula",numericpatterns::kShipFormula,error);
    l.power=locate::FindOne(image,"combat fixed-point power",numericpatterns::kCombatPower,error);
    l.logarithm=locate::FindOne(image,"combat fixed-point logarithm",numericpatterns::kCombatLog,error);
    if(!p || !l.power || !l.logarithm)return false;
    if(Relative(p,0xd3)!=l.power || Relative(l.power,0x80)!=l.logarithm || Relative(l.power,0xfb)!=l.logarithm){error="combat formula call cross-check failed";return false;}
    const size_t offsets[]={0xb,0x1c,0x65,0x7b,0x9e,0xaa};
    for(size_t i=0;i<6;++i)l.defines[i]=reinterpret_cast<int64_t*>(Relative(p,offsets[i]));
    // Filled from the reviewed logarithm's constant loads; validate their fixed Q7 values.
    const size_t constantOffsets[]={0xb,0x1e,0x3e,0x50};
    const int64_t expected[]={0,128,256,348};
    for(size_t i=0;i<4;++i){l.constants[i]=reinterpret_cast<int64_t*>(Relative(l.logarithm,constantOffsets[i]));
        if(*l.constants[i]!=expected[i]){error="unsupported combat Q7 constants";return false;}}
    l.block.at=p;l.block.stolen=l.block.span=8;l.block.handler=Hook;l.block.data=&l;l.block.returns=true;return true;
}
} // namespace shipformula
