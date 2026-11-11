// Weapon and strike-craft damage producers, retaining native decimal division rounding.
#pragma once
#include "numericbig.h"
#include "numericpatch.h"
#include "components_patterns.h"
#include "mean_patterns.h"
namespace componentmath {
using numeric::Integer;
template<class T>T Read(const uint8_t* p,size_t offset){return *reinterpret_cast<const T*>(p+offset);}
inline Integer Mul(const Integer& a,const Integer& b){return a*b/Integer(100000);}
inline Integer Divide(const Integer& value,const Integer& denominator){
    if(denominator.Zero())return Integer(0xffffffffLL);
    if(value.Abs()<Integer(92233720368548LL))return value*Integer(100000)/denominator;
    if(denominator.Abs()<Integer(10000000000LL)){
        auto whole=value/Integer(100000)*Integer(100000);
        return whole/denominator*Integer(100000)+(value%Integer(100000))*Integer(100000)/denominator+
            (whole%denominator)*Integer(100000)/denominator;
    }
    return value/(denominator/Integer(100000));
}
inline Integer Mean(const uint8_t* p){return Integer(Read<int64_t>(p,0))+Integer(Read<int64_t>(p,8))/Integer(2);}
struct Locations {
    uint8_t* functions[2]{};uint8_t* modifier=nullptr;uint8_t* mean=nullptr;
    numericpatch::Block blocks[2];
    // Defines at 558,560,568,570,578,580,588,590,598,5a0 in the reviewed build.
    const int64_t* defines[10]{};
};
inline Integer Damage(const Locations& l,const uint8_t* component,bool strike,bool weighted){
    auto value=[&](size_t offset){return Integer(Read<int64_t>(component,offset));};
    auto factor=[&](size_t index){return Integer(*l.defines[index]);};
    Integer result;
    if(strike){
        result=Mean(component+0x1250)*Integer(Read<int>(component,0x11c8));
        auto denominator=value(0x11e8);if(!denominator.Positive())denominator=Integer(100000);
        result=Divide(result,denominator);
    }else {
        auto denominator=(value(0x11e0)+Mean(component+0x11d0))/Integer(10);
        if(denominator<Integer(100))denominator=Integer(100);
        result=Mul(Divide(Mean(component+0x1218),denominator),value(0x11f8));
    }
    if(!weighted)return result;
    Integer weights;
    if(strike){
        weights=Integer(100000)+Mul(value(0x1280),factor(9))+Divide(value(0x11f0),factor(3))+
            Mul(value(0x1260)-Integer(100000),factor(6))+Mul(value(0x1268)-Integer(100000),factor(5))+
            Mul(value(0x1278),factor(8))+Mul(value(0x1270)-Integer(100000),factor(7));
    }else {
        weights=Integer(200000)+Mul(value(0x1248),factor(9))+Mul(value(0x1250),factor(4))+
            Divide(value(0x11e8),factor(3))+Mul(value(0x1230)-Integer(100000),factor(5))+
            Mul(value(0x1240),factor(8))+Mul(value(0x1238)-Integer(100000),factor(7));
        weights=Mul(weights,value(0x1258));
    }
    result=Mul(result,weights);
    result=Mul(result,Integer(100000)+Mul(Integer(100000)-value(strike?0x1210:0x1200),factor(2)));
    uint8_t category=component[strike?0x11e0:0x1330];
    if(category==1 || category==2)result=Mul(result,factor(category-1));
    return result;
}
inline uint8_t* Relative(uint8_t* p,size_t offset){return locate::RelTarget(p+offset,p+offset+4);}
template<bool Strike>bool Hook(numericpatch::Context& c,const void* data){
    try {auto& l=*static_cast<const Locations*>(data);
        auto value=Damage(l,reinterpret_cast<uint8_t*>(c.rcx),Strike,(c.r8&255)!=0).Clamp();
        *reinterpret_cast<int64_t*>(c.rdx)=value;c.rax=c.rdx;return true;
    }catch(...){return false;}
}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    l.functions[0]=locate::FindOne(image,"weapon average damage",numericpatterns::kWeaponDamage,error);
    l.functions[1]=locate::FindOne(image,"strike craft average damage",numericpatterns::kStrikeCraftDamage,error);
    l.modifier=locate::FindOne(image,"component modifier sum",numericpatterns::kComponentModifier,error);
    l.mean=locate::FindOne(image,"component range mean",numericpatterns::kComponentRangeMean,error);
    if(!l.functions[0] || !l.functions[1] || !l.modifier || !l.mean)return false;
    const size_t weaponOffsets[]={0x8a4,0x8e2,0x794,0x4f0,0x5d0,0x465,0,0x348,0x3da,0x1a9};
    const size_t strikeOffsets[]={0x6e5,0x723,0x5d5,0x428,0,0x2ed,0x37f,0x1d0,0x262,0x12c};
    for(size_t i=0;i<10;++i){auto a=weaponOffsets[i]?Relative(l.functions[0],weaponOffsets[i]):nullptr;
        auto b=strikeOffsets[i]?Relative(l.functions[1],strikeOffsets[i]):nullptr;
        if(a && b && a!=b){error="component damage defines disagree";return false;}
        l.defines[i]=reinterpret_cast<int64_t*>(a?a:b);}
    if(Relative(l.functions[0],0x3a)!=l.mean || Relative(l.functions[1],0x22)!=l.mean){error="component mean cross-check failed";return false;}
    for(int i=0;i<2;++i){auto& b=l.blocks[i];b.at=l.functions[i];b.stolen=b.span=5;b.returns=true;b.handler=i?Hook<true>:Hook<false>;b.data=&l;}
    return true;
}
} // namespace componentmath
