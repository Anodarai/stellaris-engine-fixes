// Ship damage is one calculation: do not narrow between modifiers, component
// adjustments and the damaged-hull penalty, where large signed terms can cancel.
#pragma once
#include "shipstats.h"
#include "componentmath.h"
#include "damagehelpers_patterns.h"
namespace shipdamage {
using numeric::Integer;
using shipstats::Read;
using componentmath::Mul;
using PtrFn=uint8_t*(__fastcall*)(uint8_t*);
using PairFn=bool(__fastcall*)(uint8_t*,uint8_t*);
using BoolFn=bool(__fastcall*)(uint8_t*);
using MainFn=int64_t*(__fastcall*)(int64_t*,uint8_t*,uint8_t*,uint8_t*,bool,void*);
using AdjustFn=int64_t*(__fastcall*)(uint8_t*,int64_t*,uint8_t*,bool,void*);
using PenaltyFn=int64_t*(__fastcall*)(int64_t*,uint8_t*,int);
constexpr size_t kStolen[]={15,15,18};
struct Locations {
    shipstats::Locations stats;componentmath::Locations components;
    uint8_t* functions[3]{};uint8_t* originals[3]{};
    PtrFn owner=nullptr,federation=nullptr,system=nullptr,systemOwner=nullptr;
    PairFn member=nullptr,home=nullptr;BoolFn specialTerritory=nullptr;
    uint8_t** fleetDb=nullptr;uint8_t** nullFleet=nullptr;uint8_t** state=nullptr;const int* dynamic=nullptr;
    size_t fleetId=0;  // CFleet's own id, from the inlined lookup's `cmp [rdi+id],eax`
};
template<class F>F Virtual(uint8_t* object,size_t slot){return reinterpret_cast<F>(Read<uint8_t*>(Read<uint8_t*>(object,0),slot));}
inline bool Valid(uint8_t* object,size_t slot){return Virtual<BoolFn>(object,slot)(object);}
inline uint8_t* Record(uint8_t* design){
    auto definition=Virtual<PtrFn>(design,0x10)(design);int index=Read<int>(design,0x52c);
    if(index<0 || index>=Read<int>(definition,0x2c))index=0;
    return Read<uint8_t*>(definition,0x20)+size_t(index)*0x110;
}
template<class F>void EachComponent(uint8_t* record,F visit){
    int count=Read<int>(record,0x24);if(count<0)throw std::runtime_error("invalid design sections");
    auto sections=Read<uint8_t**>(record,0x18);
    for(int i=0;i<count;++i){auto section=sections[i];int n=Read<int>(section,0x5c);
        if(n<0)throw std::runtime_error("invalid section components");auto data=Read<uint8_t*>(section,0x50);
        for(int j=0;j<n;++j)visit(Read<uint8_t*>(data,size_t(j)*0x20+0x10));}
}
inline Integer ComponentModifier(const Locations& l,uint8_t* component,uint8_t* mods,int mode){
    int count=Read<int>(component,0x89c);if(count<0)throw std::runtime_error("invalid component categories");
    auto data=Read<uint8_t*>(component,0x890);Integer sum;
    for(int i=0;i<count;++i)sum+=shipstats::Modifier(l.stats,mods,Read<int>(data,size_t(i)*0x48+0x38+mode*4));
    return sum;
}
inline Integer Adjustments(const Locations& l,uint8_t* design,uint8_t* mods,bool weighted){
    size_t count=0;
    auto eligible=[](uint8_t* c){return Valid(c,0x40) && (c[0x1e1]&0xfd)==0;};
    // Keep the native two traversals and swap-last deduplication order. Only the
    // arithmetic and storage sizes change; virtual eligibility calls still occur twice.
    EachComponent(Record(design),[&](uint8_t* c){if(eligible(c))++count;});
    std::vector<uint8_t*> components;components.reserve(count);
    EachComponent(Record(design),[&](uint8_t* c){if(eligible(c))components.push_back(c);});
    std::vector<int64_t> counts;counts.reserve(components.size());
    for(size_t i=0;i<components.size();++i){int64_t n=1;
        for(size_t j=i+1;j<components.size();)if(components[j]==components[i]){components[j]=components.back();components.pop_back();++n;}else ++j;
        counts.push_back(n);}
    Integer result;
    for(size_t i=0;i<components.size();++i){auto c=components[i];auto base=componentmath::Damage(l.components,c,c[0x1e1]==2,weighted);
        auto changed=Mul(base,Integer(100000)+ComponentModifier(l,c,mods,0));
        changed=Mul(changed,Integer(100000)+ComponentModifier(l,c,mods,1));
        result+=(changed-base)*Integer(counts[i]);}
    return result;
}
inline Integer Penalty(const Locations& l,uint8_t* ship,int id){
    auto maximum=Integer(Read<int64_t>(ship,0xef8));auto current=Integer((std::max)(int64_t(0),Read<int64_t>(ship,0xa40)));
    auto value=shipstats::Modifier(l.stats,Read<uint8_t*>(*l.state,0x290),id);
    if(maximum.Positive())value=Mul(value,componentmath::Divide(maximum-current,Mul(maximum,Integer(99000))));
    return value;
}
inline uint8_t* Fleet(const Locations& l,uint32_t id){return game::ResolveRefOr(*l.fleetDb,id,l.fleetId,*l.nullFleet);}
inline uint8_t* System(const Locations& l,uint8_t* ship){
    auto object=ship+0x20;
    // The virtual call returns an object pointer. The lookup reads its reference ID at +0x20.
    return l.system(Virtual<PtrFn>(object,8)(object));
}
inline Integer Damage(const Locations& l,uint8_t* design,uint8_t* mods,uint8_t* ship,bool weighted){
    auto size=l.stats.size(ship+0x458);auto combined=[&](int id){return shipstats::Combined(l.stats,size,mods,id);};
    auto modifier=[&](int id){return shipstats::Modifier(l.stats,mods,id);};
    auto factor=Integer(100000)+combined(0x3a);auto flags=Read<uint32_t>(size,0xc8);
    if(flags&0x4000)factor+=modifier(0x206);if(flags&0x10000)factor+=modifier(0x20a);
    if(design[0x468]!=14)factor+=modifier(l.dynamic[design[0x468]]);
    auto result=Mul(Integer(Read<int64_t>(design,weighted?0x380:0xd0)),factor);
    factor=Integer(100000)+combined(0x40)+modifier(0x6a);
    auto owner=l.owner(ship);auto federation=l.federation(owner+0x1c38);
    if(Valid(federation,8)){auto systemOwner=l.systemOwner(System(l,ship));
        if(Valid(systemOwner+0x18,8) && (systemOwner==owner || l.member(l.federation(owner+0x1c38),systemOwner)))factor+=combined(0x168);}
    if(Read<uint32_t>(l.stats.size(design),0xc8)&0x4000)factor+=modifier(0x205);
    if(Read<uint32_t>(l.stats.size(design),0xc8)&0x10000)factor+=modifier(0x20b);
    if(Valid(ship,0x20)){
        owner=l.owner(ship);if(l.home(owner+0x2e98,System(l,ship)))factor+=combined(0x41);
        auto fleet=Fleet(l,Read<uint32_t>(ship,0x998));
        if(Valid(fleet+0x18,0x20)){factor+=combined(l.stats.territory(fleet)<2?0x1d2:0x1d3);
            if(l.specialTerritory(fleet))factor+=combined(0x230);}
    }
    result=Mul(result,factor)+Adjustments(l,design,mods,weighted);
    if(Valid(ship,0x20))result=Mul(result,Integer(100000)+Penalty(l,ship,0x40));
    return result;
}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    if(!shipstats::Locate(image,l.stats,error) || !componentmath::Locate(image,l.components,error))return false;
    l.functions[0]=locate::FindOne(image,"ship average damage",numericpatterns::kShipAverageDamage,error);
    l.functions[1]=locate::FindOne(image,"ship component adjustments",numericpatterns::kComponentAdjustments,error);
    l.functions[2]=locate::FindOne(image,"ship damaged hull penalty",numericpatterns::kDamagePenalty,error);
    if(!l.functions[0] || !l.functions[1] || !l.functions[2])return false;
    auto p=l.functions[0];auto rel=[&](size_t offset){return shipstats::Relative(p,offset);};
    l.owner=reinterpret_cast<PtrFn>(rel(0x912));l.federation=reinterpret_cast<PtrFn>(rel(0x92e));
    l.system=reinterpret_cast<PtrFn>(rel(0x951));l.systemOwner=reinterpret_cast<PtrFn>(rel(0x959));
    l.member=reinterpret_cast<PairFn>(rel(0x98e));l.home=reinterpret_cast<PairFn>(rel(0xaf2));
    l.specialTerritory=reinterpret_cast<BoolFn>(rel(0xe04));
    l.fleetDb=reinterpret_cast<uint8_t**>(rel(0xb78));l.nullFleet=reinterpret_cast<uint8_t**>(rel(0xbaa));l.fleetId=p[0xba4];
    l.dynamic=reinterpret_cast<int*>(rel(0x68a));l.state=reinterpret_cast<uint8_t**>(shipstats::Relative(l.functions[2],0x2c));
    if(rel(0xf48)!=l.functions[1] || rel(0xf75)!=l.functions[2] ||
       shipstats::Relative(l.functions[2],0xcd)!=reinterpret_cast<uint8_t*>(l.stats.modifierTypes)){
        error="ship damage helper cross-check failed";return false;}
    return true;
}
} // namespace shipdamage
