#pragma once
#include "numericbig.h"
#include "locate.h"
#include "shipstats_patterns.h"
#include "stathelpers_patterns.h"
namespace shipstats {
using numeric::Integer;
using SizeFn=uint8_t*(__fastcall*)(uint8_t*);
using SpecialFn=int(__fastcall*)(uint8_t*,int);
using TerritoryFn=uint32_t(__fastcall*)(uint8_t*);
using MaximumFn=int64_t*(__fastcall*)(int64_t*,uint8_t*,uint8_t*,uint8_t*,void*);
template<class T>T Read(const uint8_t* p,size_t offset){return *reinterpret_cast<const T*>(p+offset);}
struct Locations {
    uint8_t* functions[4]{};uint8_t* combined=nullptr;
    SizeFn size=nullptr;SpecialFn special=nullptr;TerritoryFn territory=nullptr;
    uint8_t** modifierTypes=nullptr;const int* dynamic[3]{};const int64_t* growth[4]{};
    MaximumFn originals[3]{};
};
constexpr size_t kStolen[]={14,15,15};
inline Integer Modifier(const Locations& l,uint8_t* modifiers,int id){
    int count=Read<int>(modifiers,0x1c);if(count<0)throw std::runtime_error("invalid modifier count");
    auto ids=Read<int*>(modifiers,0x10);auto values=Read<uint8_t*>(modifiers,0x38);
    for(int i=0;i<count;++i)if(ids[i]==id){
        int64_t value=Read<int64_t>(values,size_t(i)*16);
        if(Read<uint32_t>(*l.modifierTypes,size_t(id)*0xb0+0xa8)&32)value=(std::max)(int64_t(0),(std::min)(int64_t(100000),value));
        return Integer(value);
    }
    return Integer();
}
inline Integer Combined(const Locations& l,uint8_t* size,uint8_t* modifiers,int id){
    auto global=Modifier(l,modifiers,id);int specific=l.special(size,id);return global+Modifier(l,modifiers,specific);
}
inline bool IsFleet(uint8_t* fleet){
    using Fn=bool(__fastcall*)(uint8_t*);auto object=fleet+0x18;
    return reinterpret_cast<Fn>(Read<uint8_t*>(Read<uint8_t*>(object,0),0x20))(object);
}
inline Integer Maximum(const Locations& l,int kind,uint8_t* design,uint8_t* modifiers,uint8_t* fleet){
    const size_t baseFields[]={0x10,0x130,0x70};const int flatIds[]={0x1e,0x2e,0x24};const int multIds[]={0x1f,0x2f,0x25};
    auto amount=Integer(Read<int64_t>(design,baseFields[kind]));auto size=l.size(design);
    amount+=Combined(l,size,modifiers,flatIds[kind]);
    auto multiplier=Combined(l,size,modifiers,multIds[kind])+Modifier(l,modifiers,l.dynamic[kind][design[0x468]]);
    if(IsFleet(fleet) && l.territory(fleet)<2){
        if(kind==0){amount+=Modifier(l,modifiers,0x1ca);multiplier+=Modifier(l,modifiers,0x1cb);}
        else {int flat=kind==1?0x1c8:0x1c6;amount+=Combined(l,size,modifiers,flat);multiplier+=Combined(l,size,modifiers,flat+1);}
    }
    auto bioSize=kind==2?l.size(design):size;
    if(Read<uint32_t>(bioSize,0xc8)&0x4000){
        if(kind==0){
            amount+=Modifier(l,modifiers,0x1ed);auto growth=Read<int64_t>(l.size(design),0x670);
            int tier=growth<=*l.growth[0]?0x1ef:growth<=*l.growth[1]?0x1f0:growth<=*l.growth[2]?0x1f1:growth<=*l.growth[3]?0x1f2:0x1f3;
            multiplier+=Modifier(l,modifiers,0x1ee)+Modifier(l,modifiers,tier);
        }else if(kind==1){
            auto growth=Read<int64_t>(l.size(design),0x670);amount+=Modifier(l,modifiers,0x201);
            multiplier+=Modifier(l,modifiers,0x202);
            amount+=Integer(growth)*Modifier(l,modifiers,0x203)/Integer(100000);
        }else {amount+=Modifier(l,modifiers,0x1ff);multiplier+=Modifier(l,modifiers,0x200);}
    }
    multiplier+=Integer(100000);if(multiplier.Negative())multiplier=Integer();
    return amount*multiplier/Integer(100000);
}
inline uint8_t* Relative(uint8_t* p,size_t offset){return locate::RelTarget(p+offset,p+offset+4);}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    const char* patterns[]={numericpatterns::kShipMaxHull,numericpatterns::kShipMaxArmor,numericpatterns::kShipMaxShield,numericpatterns::kShipAverageDamage};
    for(int i=0;i<3;++i){l.functions[i]=locate::FindOne(image,"ship stat producer",patterns[i],error);if(!l.functions[i])return false;}
    l.combined=locate::FindOne(image,"combined ship modifier",numericpatterns::kCombinedShipModifier,error);if(!l.combined)return false;
    const size_t sizeOffsets[]={0x344,0x34d,0x343},combinedOffsets[]={0x363,0x368,0x35e},territoryOffsets[]={0x5a2,0x4b9,0x599},dynamicOffsets[]={0x544,0x458,0x536},typeOffsets[]={0x421,0x5a1,0x472};
    auto hull=l.functions[0];l.size=reinterpret_cast<SizeFn>(Relative(hull,sizeOffsets[0]));l.territory=reinterpret_cast<TerritoryFn>(Relative(hull,territoryOffsets[0]));
    l.modifierTypes=reinterpret_cast<uint8_t**>(Relative(l.combined,0x27));l.special=reinterpret_cast<SpecialFn>(Relative(l.combined,0x5a));
    for(int i=0;i<3;++i){auto p=l.functions[i];l.dynamic[i]=reinterpret_cast<int*>(Relative(p,dynamicOffsets[i]));
        if(Relative(p,sizeOffsets[i])!=reinterpret_cast<uint8_t*>(l.size) || Relative(p,combinedOffsets[i])!=l.combined ||
            Relative(p,territoryOffsets[i])!=reinterpret_cast<uint8_t*>(l.territory) || Relative(p,typeOffsets[i])!=reinterpret_cast<uint8_t*>(l.modifierTypes)){
            error="ship stat producer callee/global disagreement";return false;}}
    const size_t growthOffsets[]={0x74e,0x7b3,0x7c4,0x7d8};for(int i=0;i<4;++i)l.growth[i]=reinterpret_cast<int64_t*>(Relative(hull,growthOffsets[i]));
    return true;
}
} // namespace shipstats
