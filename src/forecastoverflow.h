#pragma once
#include "numericmath.h"
#include "numericpatch.h"
#include "forecast_patterns.h"
namespace forecastoverflow {
struct Array {int64_t* data;int32_t capacity,count;};
inline Array* At(uint64_t frame,size_t offset){return *reinterpret_cast<Array**>(frame+offset);}
inline bool Combined(numericpatch::Context& c,const void*){
    auto out=At(c.rbp,0x40),produce=At(c.rbp,0x110),upkeep=At(c.rbp,0x130);
    if(out->count<0 || produce->count<out->count || upkeep->count<out->count)return false;
    for(int i=0;i<out->count;++i){numeric::Sum sum;sum.Add(out->data[i]);sum.Add(produce->data[i]);sum.Subtract(upkeep->data[i]);out->data[i]=sum.Clamp();}
    // The continuation reloads all volatile registers before use. Nonvolatile values stay live.
    return true;
}
inline bool Difference(numericpatch::Context& c,const void*){
    auto p=reinterpret_cast<int64_t*>(c.rcx+c.r9);*p=numeric::Subtract(*p,static_cast<int64_t>(c.rax));c.r9+=8;return true;
}
inline bool Negation(numericpatch::Context& c,const void*){
    auto p=reinterpret_cast<int64_t*>(c.r8+c.rcx);*p=numeric::Subtract(*p,static_cast<int64_t>(c.rax));c.r8+=8;return true;
}
struct Locations {numericpatch::Block blocks[3];};
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    auto p=locate::FindOne(image,"income forecast arithmetic",numericpatterns::kForecast,error);if(!p)return false;
    const size_t offsets[]={0x115,0x34f,0x442},stolen[]={8,8,8},spans[]={0x71,8,8};
    numericpatch::Handler handlers[]={Combined,Difference,Negation};
    for(int i=0;i<3;++i){auto& b=l.blocks[i];b.at=p+offsets[i];b.stolen=stolen[i];b.span=spans[i];b.handler=handlers[i];}
    return true;
}
} // namespace forecastoverflow
