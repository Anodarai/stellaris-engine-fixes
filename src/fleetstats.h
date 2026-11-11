// Coupled aggregate point totals. Per-ship combat state is untouched by this aggregate layer.
#pragma once
#include "fleetoverflow.h"
#include "fleetstats_patterns.h"
namespace fleetstats {
using HullFn=int64_t*(__fastcall*)(uint8_t*,int64_t*);
using PointsFn=void(__fastcall*)(uint8_t*,int64_t*,int64_t*);
// The fifth argument is CString*, not a bool. A byte-sized false leaves the
// upper seven bytes of the Windows x64 stack argument uninitialized.
using MaximumFn=int64_t*(__fastcall*)(int64_t*,uint8_t*,uint8_t*,uint8_t*,void*);
struct Locations {
    fleetoverflow::Locations fleet;
    uint8_t* functions[4]{};
    uint8_t** fleetDb=nullptr;uint8_t** nullFleet=nullptr;
    MaximumFn maximum=nullptr;
    size_t hull=0,shipFleet=0,fleetId=0,designObject=0,designSlot=0,properties=0;
    size_t current[2]{},maximumPoints[2]{};
};
constexpr size_t kStolen[]={15,16,15,15};
inline uint8_t* Relative(uint8_t* p,size_t off){return locate::RelTarget(p+off,p+off+4);}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    if(!fleetoverflow::Locate(image,l.fleet,error))return false;
    const char* patterns[]={numericpatterns::kHull,numericpatterns::kMaxHull,numericpatterns::kArmor,numericpatterns::kShield};
    for(int i=0;i<4;++i)if(!(l.functions[i]=locate::FindOne(image,"fleet points",patterns[i],error)))return false;
    auto a=l.functions[0],b=l.functions[1];
    if(locate::Disp32(a+8)!=static_cast<int>(l.fleet.count) || locate::Disp32(b+12)!=static_cast<int>(l.fleet.count) ||
       Relative(a,0x23)!=reinterpret_cast<uint8_t*>(l.fleet.shipDb) || Relative(b,0x53)!=reinterpret_cast<uint8_t*>(l.fleet.shipDb) ||
       Relative(a,0x2a)!=reinterpret_cast<uint8_t*>(l.fleet.nullShip) || Relative(b,0x8b)!=reinterpret_cast<uint8_t*>(l.fleet.nullShip)){
        error="fleet-stat layout/global disagreement";return false;
    }
    l.hull=locate::Disp32(a+0x7f);l.shipFleet=locate::Disp32(b+0x9e);l.fleetId=b[0xc5];
    l.fleetDb=reinterpret_cast<uint8_t**>(Relative(b,0x92));l.nullFleet=reinterpret_cast<uint8_t**>(Relative(b,0xcb));
    l.designObject=b[0xd6];l.designSlot=locate::Disp32(b+0xd9);l.properties=locate::Disp32(b+0xe0);
    l.maximum=reinterpret_cast<MaximumFn>(Relative(b,0xf5));
    for(int i=0;i<2;++i){auto p=l.functions[i+2];
        l.current[i]=locate::Disp32(p+0x16e);l.maximumPoints[i]=locate::Disp32(p+0x12d);
        if(locate::Disp32(p+0x18)!=static_cast<int>(l.fleet.count)){error="fleet point-count disagreement";return false;}
    }
    return true;
}
inline uint8_t* Fleet(const Locations& l,uint8_t* ship){
    uint32_t id=*reinterpret_cast<uint32_t*>(ship+l.shipFleet);
    return game::ResolveRefOr(*l.fleetDb,id,l.fleetId,*l.nullFleet);
}
inline bool Hull(const Locations& l,uint8_t* fleet,int64_t& current,int64_t& maximum){
    int count=*reinterpret_cast<int*>(fleet+l.fleet.count);if(count<0)return false;
    auto ids=*reinterpret_cast<uint32_t**>(fleet+l.fleet.ships);
    numeric::Sum a,b;
    for(int i=0;i<count;++i){
        auto ship=fleetoverflow::Ship(l.fleet,ids[i]);
        a.Add(numeric::DecimalToBinary(*reinterpret_cast<int64_t*>(ship+l.hull)));
        auto object=ship+l.designObject;
        using DesignFn=uint8_t*(__fastcall*)(uint8_t*);
        auto design=reinterpret_cast<DesignFn>((*reinterpret_cast<void***>(object))[l.designSlot/8])(object);
        int64_t raw;l.maximum(&raw,ship+l.properties,design,Fleet(l,ship),nullptr);
        b.Add(numeric::DecimalToBinary(raw));
    }
    numeric::Coupled(a,b,current,maximum);return true;
}
inline bool Points(const Locations& l,uint8_t* fleet,int type,int64_t& current,int64_t& maximum,bool aliased=false){
    int count=*reinterpret_cast<int*>(fleet+l.fleet.count);if(count<0)return false;
    auto ids=*reinterpret_cast<uint32_t**>(fleet+l.fleet.ships);
    aliased=aliased || &current==&maximum;
    numeric::Sum a,b;a.Add(current);if(!aliased)b.Add(maximum);
    using Predicate=bool(__fastcall*)(uint8_t*);
    for(int i=0;i<count;++i){
        auto ship=fleetoverflow::Ship(l.fleet,ids[i]);
        if(!reinterpret_cast<Predicate>((*reinterpret_cast<void***>(ship))[4])(ship))continue;
        ship=fleetoverflow::Ship(l.fleet,ids[i]);
        if(!reinterpret_cast<Predicate>((*reinterpret_cast<void***>(ship+0x20))[0x70/8])(ship+0x20))continue;
        ship=fleetoverflow::Ship(l.fleet,ids[i]);b.Add(*reinterpret_cast<int64_t*>(ship+l.maximumPoints[type]));
        ship=fleetoverflow::Ship(l.fleet,ids[i]);a.Add(*reinterpret_cast<int64_t*>(ship+l.current[type]));
    }
    if(aliased){a.Merge(b);current=maximum=a.Clamp();}
    else numeric::Coupled(a,b,current,maximum);
    return true;
}
} // namespace fleetstats
