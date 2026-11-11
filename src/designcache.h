// A design rebuild owns these wide sums. They never outlive the synchronous
// rebuild, and native arrays, source labels and sorting retain their ownership.
#pragma once
#include <array>
#include <map>
#include <tuple>
#include "shipdamage.h"
#include "designcache_patterns.h"
#include "designmodifier_patterns.h"
#include "designmetadata_patterns.h"
#include "designsources_patterns.h"
namespace designcache {
using numeric::Integer;using shipstats::Read;using componentmath::Mul;
struct Terms {
    Integer values[3];
    void Add(int mode,const Integer& amount){if(mode>=0 && mode<3)values[mode]+=amount;}
    Integer BeforePost()const{return Mul(values[0],Integer(100000)+values[1]);}
    Integer Value()const{return BeforePost()+values[2];}
};
inline Terms ReadTerms(uint8_t* stat){
    Terms result;auto data=Read<uint8_t*>(stat,0x10);int count=Read<int>(stat,0x1c);
    if(count<0)throw std::runtime_error("invalid design stat contributions");
    for(int i=0;i<count;++i){auto p=data+size_t(i)*0x48;result.Add(Read<int>(p,8),Integer(Read<int64_t>(p,0)));}
    return result;
}
struct Pass {
    uint8_t* base;std::array<Terms,26> stats;Pass* previous=nullptr;int64_t poweredComponents=0;
    std::map<std::tuple<size_t,int,uint32_t>,Integer> descriptions;
    uint8_t* pendingStat=nullptr;int pendingMode=0;Integer pendingValue;
    void Remember(uint8_t* stat,int mode,const Integer& value){pendingStat=stat;pendingMode=mode;pendingValue=value;}
    explicit Pass(uint8_t* b,uint8_t* record):base(b){
        for(size_t i=0;i<stats.size();++i)stats[i]=ReadTerms(base+i*0x20);
        // This section-speed contribution is inserted directly by CalcStats,
        // bypassing SShipDesignStat::Add; include it once in the same lifetime.
        int count=Read<int>(record,0x24);if(count<0)throw std::runtime_error("invalid design sections");
        auto sections=Read<uint8_t**>(record,0x18);
        for(int i=0;i<count;++i)stats[12].Add(0,Integer(Read<int64_t>(Read<uint8_t*>(sections[i],0x40),0x130)));
        shipdamage::EachComponent(record,[&](uint8_t* c){if(Read<int64_t>(c,0x2a8)>0)++poweredComponents;});
        int extra=Read<int>(record,0x3c);if(extra<0)throw std::runtime_error("invalid auxiliary component count");auto data=Read<uint8_t**>(record,0x30);
        for(int i=0;i<extra;++i)if(Read<int64_t>(data[i],0x2a8)>0)++poweredComponents;
    }
    Terms* Find(uint8_t* stat){auto at=reinterpret_cast<uintptr_t>(stat),start=reinterpret_cast<uintptr_t>(base);
        if(at<start || at-start>=26*0x20 || (at-start)%0x20)return nullptr;return &stats[(at-start)/0x20];}
};
inline thread_local Pass* current=nullptr;
inline Terms GetTerms(uint8_t* stat){for(auto p=current;p;p=p->previous)if(auto terms=p->Find(stat))return *terms;return ReadTerms(stat);}
struct Scope {Pass& pass;explicit Scope(Pass& p):pass(p){pass.previous=current;current=&pass;}~Scope(){current=pass.previous;}};
struct Component {uint8_t* stat;uint8_t* component;bool used=false;Component* previous=nullptr;};
inline thread_local Component* component=nullptr;
struct ComponentScope {Component& value;explicit ComponentScope(Component& v):value(v){value.previous=component;component=&value;}~ComponentScope(){component=value.previous;}};
struct SpecialPass {uint8_t* design;std::array<Integer,9> sums;bool begun=false;SpecialPass* previous=nullptr;};
inline thread_local SpecialPass* special=nullptr;
struct GroupPass {uint8_t* output;std::map<std::pair<uint32_t,int>,Integer> sums;GroupPass* previous=nullptr;};
inline thread_local GroupPass* group=nullptr;
struct GroupScope {GroupPass& pass;explicit GroupScope(GroupPass& p):pass(p){pass.previous=group;group=&pass;}~GroupScope(){group=pass.previous;}};
struct SpecialScope {SpecialPass& pass;explicit SpecialScope(SpecialPass& p):pass(p){pass.previous=special;special=&pass;}~SpecialScope(){special=pass.previous;}};
struct Locations {
    // CalcStats, AddValue, AddComponent, RecalcSpecialValues, AddComponentSpecialValues.
    uint8_t* functions[7]{};uint8_t* originals[7]{};uint8_t* finalize=nullptr;uint8_t* excess=nullptr;
    componentmath::Locations components;shipstats::Locations stats;const int64_t* excessScale=nullptr;
    numericpatch::Block blocks[6];
};
constexpr size_t kStolen[]={15,15,15,21,14,15,15};
struct ModifierSpec {int id,offset,mode;};
constexpr ModifierSpec kModifiers[]={
    {0x223,0x260,0},{0x224,0x260,1},{0x1e,0,0},{0x1f,0,1},{0x20,0x20,0},{0x22,0x40,0},
    {0x24,0x60,0},{0x25,0x60,1},{0x27,0x80,0},{0x29,0xa0,0},{0x2e,0x120,0},{0x2f,0x120,1},
    {0x31,0x140,0},{0x33,0x160,0},{0x49,0x100,0},{0x4a,0x100,1},{0x4f,0xe0,1},{0x50,0xe0,1},
    {0x3a,0xc0,1},{0x6c,0x1e0,0},{0x6d,0x200,0},{0x1dd,0x220,0},{0x66,0x1a0,0},{0x67,0x1a0,1}};
template<class F>void AddModifiers(const Locations& l,uint8_t* base,uint8_t* mods,uint32_t kind,int count,F add){
    int64_t powered=count;Pass* owner=nullptr;
    for(auto p=current;p;p=p->previous)if(p->base==base){owner=p;break;}
    if(owner && kind!=5)powered=owner->poweredComponents;
    for(const auto& spec:kModifiers){auto value=shipstats::Modifier(l.stats,mods,spec.id);
        if(spec.id==0x66)value=value*Integer(powered);
        if(owner){owner->stats[size_t(spec.offset)/0x20].Add(spec.mode,value);owner->Remember(base+spec.offset,spec.mode,value);}
        add(base+spec.offset,value.Clamp(),spec.mode);}
}
inline void Add(const Locations& l,uint8_t* stat,int64_t amount,int mode){
    for(auto p=current;p;p=p->previous)if(auto terms=p->Find(stat)){
        Integer value(amount);
        if(component && !component->used && component->stat==stat && mode==0){
            auto c=component->component;value=componentmath::Damage(l.components,c,c[0x1e1]==2,false);component->used=true;
        }
        terms->Add(mode,value);p->Remember(stat,mode,value);return;
    }
}
inline void AddSpecial(const Locations& l,uint8_t* design,uint8_t* c){
    auto p=special;if(!p || p->design!=design)return;
    if(!p->begun){for(size_t i=0;i<9;++i)p->sums[i]=Integer(Read<int64_t>(design,0x350+i*8));p->begun=true;}
    // The native caller has already tested component validity when this is used.
    auto value=[&](size_t o){return Integer(Read<int64_t>(c,o));};auto& sum=p->sums;
    if(c[0x1e1]==0){sum[0]+=componentmath::Mean(c+0x1218);sum[1]+=value(0x1228);sum[2]+=value(0x1238);
        sum[3]+=value(0x1248);sum[4]+=value(0x1230);sum[5]+=value(0x1240);
        sum[6]+=componentmath::Damage(l.components,c,false,true);sum[7]+=value(0x11f8);sum[8]+=value(0x1200);
    }else if(c[0x1e1]==2){sum[0]+=componentmath::Mean(c+0x1250)*Integer(Read<int>(c,0x11c8));
        sum[6]+=componentmath::Damage(l.components,c,true,true);sum[5]+=value(0x1278);sum[3]+=value(0x1280);sum[7]+=value(0x1208);sum[8]+=value(0x1210);}
}
inline void PublishSpecial(SpecialPass& pass){
    if(!pass.begun)return;
    int64_t count=int64_t(Read<int>(pass.design,0x494))+Read<int>(pass.design,0x4ac);
    for(size_t i=0;i<9;++i){auto value=pass.sums[i];if(count>0 && i!=6)value=componentmath::Divide(value,Integer(count)*Integer(100000));
        *reinterpret_cast<int64_t*>(pass.design+0x350+i*8)=value.Clamp();}
}
inline Integer ExcessRatio(const Terms& terms,int64_t scale,Integer& remainder){
    auto power=terms.BeforePost();auto total=power+terms.values[2];auto denominator=Mul(power,Integer(scale));
    if(!total.Positive() || !denominator.Positive())return Integer(INT64_MIN);
    auto ratio=componentmath::Divide(total,denominator);
    if(total<Integer(92233720368548LL))remainder=total*Integer(100000)%denominator;
    else if(denominator<Integer(10000000000LL))remainder=(total%Integer(100000))*Integer(100000)%denominator;
    else remainder=total%(denominator/Integer(100000));
    return ratio<Integer(100000)?ratio:Integer(100000);
}
inline bool Finalize(numericpatch::Context& c,const void*){
    try {auto stat=reinterpret_cast<uint8_t*>(c.r15);auto value=GetTerms(stat).Value().Clamp();*reinterpret_cast<int64_t*>(stat)=value;return true;}catch(...){return false;}
}
inline bool Excess(numericpatch::Context& c,const void* data){
    try {auto& l=*static_cast<const Locations*>(data);Integer remainder;
        auto value=ExcessRatio(GetTerms(reinterpret_cast<uint8_t*>(c.r13)+0x1a0),*l.excessScale,remainder);
        c.rbx=uint64_t(value.Clamp());c.rdx=uint64_t(remainder.Clamp());return true;}catch(...){return false;}
}
inline bool Special(numericpatch::Context& c,const void* data){
    try {AddSpecial(*static_cast<const Locations*>(data),reinterpret_cast<uint8_t*>(c.rdi),reinterpret_cast<uint8_t*>(c.rsi));
        c.rcx=c.rdi+0x3b0;return true;}catch(...){return false;}
}
inline void Description(uint8_t* stat,uint32_t hash,int mode,int64_t* amount,bool pending){
    auto data=Read<uint8_t*>(stat,0x10);int count=Read<int>(stat,0x1c);if(count<0)throw std::runtime_error("invalid design description count");
    uint8_t* existing=nullptr;
    for(int i=0;i<count;++i){auto entry=data+size_t(i)*0x48;if(Read<uint32_t>(entry,0x40)==hash && Read<int>(entry,8)==mode){existing=entry;break;}}
    Integer value(*amount);Pass* owner=nullptr;
    for(auto p=current;p;p=p->previous)if(p->Find(stat)){owner=p;break;}
    if(owner){
        if(pending && owner->pendingStat==stat && owner->pendingMode==mode){value=owner->pendingValue;owner->pendingStat=nullptr;}
        auto key=std::make_tuple(size_t(stat-owner->base),mode,hash);auto found=owner->descriptions.find(key);
        if(found==owner->descriptions.end())found=owner->descriptions.emplace(key,Integer(existing?Read<int64_t>(existing,0):0)).first;
        value+=found->second;found->second=value;
    }else if(existing)value+=Integer(Read<int64_t>(existing,0));
    // The original lookup/insert owns strings and capacity. Make its subsequent
    // addition 0 + corrected_total so its source entry cannot wrap a second time.
    auto result=value.Clamp();if(existing)*reinterpret_cast<int64_t*>(existing)=0;*amount=result;
}
inline bool DescriptionHook(numericpatch::Context& c,const void*){
    try {auto stack=reinterpret_cast<uint8_t*>(c.Rsp());auto stat=reinterpret_cast<uint8_t*>(c.rbp);
        Description(stat,uint32_t(c.rax),Read<int>(stack,0x28),reinterpret_cast<int64_t*>(stack+0x20),true);
        *reinterpret_cast<uint32_t*>(stack+0x60)=uint32_t(c.rax);c.r10=uint64_t(int64_t(Read<int>(stat,0x1c)));return true;
    }catch(...){return false;}
}
inline bool SectionDescriptionHook(numericpatch::Context& c,const void*){
    try {auto frame=reinterpret_cast<uint8_t*>(c.rbp);auto stat=reinterpret_cast<uint8_t*>(c.rdi)+0x180;
        Description(stat,uint32_t(c.rax),*reinterpret_cast<int*>(frame-0x11),reinterpret_cast<int64_t*>(frame-0x19),false);
        *reinterpret_cast<uint32_t*>(frame+0x27)=uint32_t(c.rax);c.r8=uint32_t(c.rsi);c.rdx=c.rsi;return true;
    }catch(...){return false;}
}
inline void GroupDescription(uint8_t* output,uint32_t hash,int mode,int64_t* amount){
    auto data=Read<uint8_t*>(output,8);int count=Read<int>(output,0x14);
    if(count<0)throw std::runtime_error("invalid grouped description count");
    uint8_t* existing=nullptr;
    for(int i=0;i<count;++i){auto entry=data+size_t(i)*0x48;
        if(Read<uint32_t>(entry,0x40)==hash && Read<int>(entry,8)==mode){existing=entry;break;}}
    Integer value(*amount);GroupPass* owner=group;
    while(owner && owner->output!=output)owner=owner->previous;
    if(owner){auto key=std::make_pair(hash,mode);auto found=owner->sums.find(key);
        if(found==owner->sums.end())found=owner->sums.emplace(key,Integer(existing?Read<int64_t>(existing,0):0)).first;
        value+=found->second;found->second=value;
    }else if(existing)value+=Integer(Read<int64_t>(existing,0));
    auto result=value.Clamp();if(existing)*reinterpret_cast<int64_t*>(existing)=0;*amount=result;
}
inline bool GroupDescriptionHook(numericpatch::Context& c,const void*){
    try{auto stack=reinterpret_cast<uint8_t*>(c.Rsp());
        GroupDescription(reinterpret_cast<uint8_t*>(c.rsi),uint32_t(c.rax),Read<int>(stack,0x28),reinterpret_cast<int64_t*>(stack+0x20));
        *reinterpret_cast<uint32_t*>(c.rbp-0x59)=uint32_t(c.rax);c.r8=uint32_t(c.r12);return true;
    }catch(...){return false;}
}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    if(!componentmath::Locate(image,l.components,error) || !shipstats::Locate(image,l.stats,error))return false;
    const char* patterns[]={numericpatterns::kDesignCalcStats,numericpatterns::kDesignAddValue,numericpatterns::kDesignAddComponent,numericpatterns::kDesignRecalc,numericpatterns::kDesignAddSpecial};
    for(int i=0;i<5;++i){l.functions[i]=locate::FindOne(image,"design cache producer",patterns[i],error);if(!l.functions[i])return false;}
    l.functions[5]=locate::FindOne(image,"design modifier contributions",numericpatterns::kDesignAddModifier,error);if(!l.functions[5])return false;
    l.functions[6]=locate::FindOne(image,"design grouped descriptions",numericpatterns::kDesignGroupSources,error);if(!l.functions[6])return false;
    l.finalize=locate::FindOne(image,"design stat finalizer",numericpatterns::kDesignFinalize,error);
    l.excess=locate::FindOne(image,"design excess power",numericpatterns::kDesignExcessPower,error);if(!l.finalize || !l.excess)return false;
    if(shipstats::Relative(l.functions[0],0xff)!=l.functions[2] || shipstats::Relative(l.functions[2],0xba)!=l.functions[1] ||
       shipstats::Relative(l.functions[4],0x1ee)!=l.functions[1] || shipstats::Relative(l.functions[3],0x42b)!=l.functions[4]){
        error="design cache caller cross-check failed";return false;}
    l.excessScale=reinterpret_cast<int64_t*>(shipstats::Relative(l.excess,0x1e7));
    auto& f=l.blocks[0];f.at=l.finalize+0x11f;f.stolen=8;f.span=0x208-0x11f;f.handler=&Finalize;f.data=&l;
    auto& e=l.blocks[1];e.at=l.excess+0xab;e.stolen=10;e.span=0x33f-0xab;e.handler=&Excess;e.data=&l;
    auto& s=l.blocks[2];s.at=l.functions[4]+0x29;s.stolen=s.span=7;s.handler=&Special;s.data=&l;
    auto& d=l.blocks[3];d.at=locate::FindOne(image,"design source description sum",numericpatterns::kDesignDescriptionHash,error);d.stolen=d.span=8;d.handler=&DescriptionHook;
    auto& t=l.blocks[4];t.at=locate::FindOne(image,"design section description sum",numericpatterns::kDesignSectionDescriptionHash,error);t.stolen=t.span=9;t.handler=&SectionDescriptionHook;
    if(!d.at || !t.at || d.at<l.functions[1] || d.at-l.functions[1]>0x110 || t.at<l.functions[0] || t.at-l.functions[0]>0x300){error="design description caller mismatch";return false;}
    auto& g=l.blocks[5];g.at=locate::FindOne(image,"grouped description hash",numericpatterns::kDesignGroupHash,error);g.stolen=g.span=6;g.handler=&GroupDescriptionHook;
    if(!g.at || g.at<l.functions[6] || g.at-l.functions[6]>0x230){error="grouped description caller mismatch";return false;}
    return true;
}
} // namespace designcache
