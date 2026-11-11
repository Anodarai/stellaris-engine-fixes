#pragma once
#include "incomeproducers.h"
#include "atomiccompare_patterns.h"
#include "atomiccategory_patterns.h"
namespace incomelifetime {
struct Locations;
struct BeginData {Locations* owner=nullptr;int index=0;};
struct Locations {
    incomeatomic::Locations tables;incomeaccumulator::Pass pass;incomeproducers::Locations producers;
    numericpatch::Block boundaries[6];BeginData begin[2];uint8_t* originals[5]{};
    unsigned depth=0;
};
inline bool Begin(numericpatch::Context& c,const void* opaque){
    auto& data=*static_cast<const BeginData*>(opaque);auto& l=*data.owner;
    if(l.depth!=1 || l.pass.Active())return false;
    try{auto job=*l.tables.jobs[data.index];l.pass.Begin(incomeatomic::Ranges(l.tables));c.rcx=uint64_t(job);return true;}catch(...){return false;}
}
inline bool Store(numericpatch::Context& c,const void* opaque){
    auto& l=*const_cast<Locations*>(static_cast<const Locations*>(opaque));
    return l.pass.Set(reinterpret_cast<int64_t*>(c.rcx),int64_t(c.rdx));
}
inline uint64_t SubFlags(uint64_t prior,uint64_t a,uint64_t b){
    uint64_t result=a-b;unsigned byte=unsigned(result&255);byte^=byte>>4;byte^=byte>>2;byte^=byte>>1;
    return (prior&~uint64_t(0x8d5)) | (a<b?1:0) | ((byte&1)?0:4) | ((a^b^result)&16) |
        (result==0?64:0) | ((result>>56)&128) | ((((a^b)&(a^result))>>52)&0x800);
}
inline bool CompareSubtract(numericpatch::Context& c,const void* opaque){
    auto& l=*const_cast<Locations*>(static_cast<const Locations*>(opaque));int64_t old=0;bool exchanged=false;
    if(!l.pass.CompareSubtract(reinterpret_cast<int64_t*>(c.r9),int64_t(c.rax),int64_t(c.rcx),old,exchanged))return false;
    c.flags=SubFlags(c.flags,c.rax,uint64_t(old));if(!exchanged)c.rax=uint64_t(old);return true;
}
inline bool Category(numericpatch::Context& c,const void* opaque){
    auto& l=*const_cast<Locations*>(static_cast<const Locations*>(opaque));
    try{auto value=incomeatomic::Category(l.pass,reinterpret_cast<uint8_t*>(c.rcx),
        incomeatomic::Read<int>(reinterpret_cast<uint8_t*>(c.r8),0x18),reinterpret_cast<uint8_t*>(c.r9));
        *reinterpret_cast<int64_t*>(c.rdx)=value.Clamp();c.rax=c.rdx;return true;}catch(...){return false;}
}
inline bool VisibleCategory(numericpatch::Context& c,const void* opaque){
    auto& l=*const_cast<Locations*>(static_cast<const Locations*>(opaque));
    try{c.rbx=uint64_t(incomeatomic::VisibleCategory(l.pass,reinterpret_cast<uint8_t*>(c.r13),
        incomeatomic::Read<int>(reinterpret_cast<uint8_t*>(c.r12),0x18),reinterpret_cast<uint8_t*>(c.rsi),int(c.r8/0x18),
        incomeatomic::Read<uint8_t>(reinterpret_cast<uint8_t*>(c.r14),0x21)).Clamp());return true;}catch(...){return false;}
}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    if(!incomeatomic::Locate(image,l.tables,error) || !incomeproducers::Locate(image,l.producers,l.pass,error))return false;
    for(int i=0;i<2;++i){l.begin[i]={&l,i};auto& b=l.boundaries[i];b.at=l.tables.roots[i]+(i?0xdc:0xde);
        b.stolen=b.span=7;b.ripOperand=3;b.handler=&Begin;b.data=&l.begin[i];}
    auto& s=l.boundaries[2];s.at=l.tables.store;s.stolen=s.span=6;s.shortBranch=4;s.returns=true;s.handler=&Store;s.data=&l;
    auto& c=l.boundaries[3];auto site=locate::FindOne(image,"economic compare/subtract loop",numericpatterns::kAtomicCompareSubtract,error);
    if(!site)return false;c.at=site+0x15;c.stolen=c.span=5;c.handler=&CompareSubtract;c.data=&l;
    auto& v=l.boundaries[4];v.at=l.tables.readResource+0x382;v.stolen=9;v.span=0xf1;v.handler=&VisibleCategory;v.data=&l;
    auto& r=l.boundaries[5];r.at=locate::FindOne(image,"recursive economic category read",numericpatterns::kAtomicCategoryRead,error);
    if(!r.at || incomeatomic::Relative(l.tables.readResource,0x41b)!=r.at){error="economic category reader cross-check failed";return false;}
    r.stolen=r.span=5;r.returns=true;r.handler=&Category;r.data=&l;
    return true;
}
} // namespace incomelifetime
