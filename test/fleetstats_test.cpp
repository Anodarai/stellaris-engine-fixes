#include <array>
#include <cstdio>
#include <random>
#include "../src/fleetstats.h"
namespace {
int failures=0;
void Check(bool ok,const char* label){if(!ok && ++failures<=20)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t off,T value){memcpy(p+off,&value,sizeof(value));}
void Jump(uint8_t* p,const void* target){const uint8_t op[]={0xff,0x25,0,0,0,0};memcpy(p,op,6);memcpy(p+6,&target,8);}
bool __fastcall Valid(uint8_t* ship){return ship[0x80]!=0;}
bool __fastcall Military(uint8_t* object){return object[0x61]!=0;}
uint8_t* __fastcall Design(uint8_t* object){return object-0x40;}
int64_t* __fastcall Maximum(int64_t* out,uint8_t*,uint8_t* ship,uint8_t*,void* tooltip){
    Check(tooltip==nullptr,"maximum hull receives a full-width null CString pointer");
    memcpy(out,ship+0x1100,8);return out;
}
bool __fastcall HullCall(const fleetstats::Locations* l,uint8_t* fleet,int64_t* a,int64_t* b){return fleetstats::Hull(*l,fleet,*a,*b);}
struct Fixture {
    std::array<uint8_t,0x10000> fleet{};
    std::array<std::array<uint8_t,0x1200>,8> ships{};
    std::array<uint8_t,0x1200> nullShip{};
    std::array<uint8_t,0x28> db{};
    std::array<uint8_t,136> entries{};
    std::array<uint8_t,64> nullFleet{};
    std::array<uint32_t,8> ids{};
    std::array<void*,20> validVtable{},militaryVtable{},designVtable{};
    uint8_t* shipDb=db.data();uint8_t* nullShipPointer=nullShip.data();
    uint8_t* fleetDb=nullptr;uint8_t* nullFleetPointer=nullFleet.data();
    explicit Fixture(fleetstats::Locations& l){
        l.fleet.shipDb=&shipDb;l.fleet.nullShip=&nullShipPointer;l.fleetDb=&fleetDb;l.nullFleet=&nullFleetPointer;l.maximum=&Maximum;
        Set(fleet.data(),l.fleet.ships,ids.data());Set(fleet.data(),l.fleet.count,8);
        Set(db.data(),0x18,entries.data());Set(db.data(),0x20,uint32_t(8));
        validVtable[4]=reinterpret_cast<void*>(&Valid);militaryVtable[14]=reinterpret_cast<void*>(&Military);designVtable[l.designSlot/8]=reinterpret_cast<void*>(&Design);
        for(int i=0;i<9;++i){auto ship=i==8?nullShip.data():ships[i].data();
            Set(ship,0,validVtable.data());Set(ship,0x20,militaryVtable.data());Set(ship,l.designObject,designVtable.data());
            if(i<8){ids[i]=0x05000000|i;Set(ship,l.fleet.shipId,ids[i]);Set(entries.data(),8+size_t(i)*16,ship);ship[0x80]=ship[0x81]=1;}
        }
    }
};
}
int main(int argc,char** argv){
    if(argc!=2)return 2;HMODULE mod=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!mod)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod)&~uintptr_t(3)));
    fleetstats::Locations found;std::string error;if(!fleetstats::Locate(image,found,error)){puts(error.c_str());return 1;}
    Check(found.hull==0xa40 && found.current[0]==0xa50 && found.current[1]==0xa48 && found.maximumPoints[0]==0xf08,"derived point fields");
    auto l=found;Fixture f(l);
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,32768,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));if(!code)return 2;
    Jump(code+4096,reinterpret_cast<void*>(&Maximum));
    Set(code,8192,f.shipDb);Set(code,8200,f.nullShipPointer);Set(code,8208,f.fleetDb);Set(code,8216,f.nullFleetPointer);
    struct Binding{uint8_t* from;uint8_t* to;};
    Binding bindings[]={
        {reinterpret_cast<uint8_t*>(found.fleet.shipDb),code+8192},{reinterpret_cast<uint8_t*>(found.fleet.nullShip),code+8200},
        {reinterpret_cast<uint8_t*>(found.fleetDb),code+8208},{reinterpret_cast<uint8_t*>(found.nullFleet),code+8216},
        {reinterpret_cast<uint8_t*>(found.maximum),code+4096}};
    size_t sizes[]={numericpatterns::kHullSize,numericpatterns::kMaxHullSize,numericpatterns::kArmorSize,numericpatterns::kShieldSize};
    for(int fn=0;fn<4;++fn){auto p=code+fn*1024;memcpy(p,found.functions[fn],sizes[fn]);
        for(size_t off=0;off+4<=sizes[fn];++off)for(const auto& b:bindings)
            if(locate::RelTarget(found.functions[fn]+off,found.functions[fn]+off+4)==b.from){
                int32_t delta=static_cast<int32_t>(b.to-p-off-4);memcpy(p+off,&delta,4);
            }
        memcpy(p+512,p,fleetstats::kStolen[fn]);Jump(p+512+fleetstats::kStolen[fn],p+fleetstats::kStolen[fn]);
    }
    // Seed the stack area the C++ caller will reuse. The previous bool prototype
    // only wrote [rsp+20] as a byte, so a test callback accepting bool hid the bug.
    const uint8_t poison[]={0x48,0x81,0xec,0x28,0x10,0,0, // sub rsp,1028h
        0x48,0xb8,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,
        0x4c,0x8d,0x14,0x24,0x41,0xbb,0,2,0,0, // r10=rsp, r11d=512
        0x49,0x89,0x02,0x49,0x83,0xc2,8,0x49,0xff,0xcb,0x75,0xf4,
        0x48,0x81,0xc4,0,0x10,0,0,0x48,0xb8};
    auto invoke=code+16384;memcpy(invoke,poison,sizeof(poison));
    auto call=&HullCall;memcpy(invoke+sizeof(poison),&call,8);
    const uint8_t tail[]={0xff,0xd0,0x48,0x83,0xc4,0x28,0xc3};memcpy(invoke+sizeof(poison)+8,tail,sizeof(tail));
    int64_t poisonedCurrent=0,poisonedMax=0;
    using PoisonFn=bool(__fastcall*)(const fleetstats::Locations*,uint8_t*,int64_t*,int64_t*);
    Check(reinterpret_cast<PoisonFn>(invoke)(&l,f.fleet.data(),&poisonedCurrent,&poisonedMax),"poisoned-stack hull caller");
    auto nativeHull=reinterpret_cast<fleetstats::HullFn>(code+512);
    auto nativeMax=reinterpret_cast<fleetstats::HullFn>(code+1536);
    fleetstats::PointsFn nativePoints[]={reinterpret_cast<fleetstats::PointsFn>(code+2560),reinterpret_cast<fleetstats::PointsFn>(code+3584)};
    std::mt19937_64 rng(0x1982);
    for(int n=0;n<20000;++n){
        for(auto& ship:f.ships){
            Set(ship.data(),l.hull,static_cast<int64_t>(rng()%1000000000000ULL));Set(ship.data(),0x1100,static_cast<int64_t>(rng()%1000000000000ULL));
            for(int type=0;type<2;++type){Set(ship.data(),l.current[type],static_cast<int64_t>(rng()%1000000000000ULL)-1000000000);Set(ship.data(),l.maximumPoints[type],static_cast<int64_t>(rng()%1000000000000ULL));}
            ship[0x80]=static_cast<uint8_t>(rng()%2);ship[0x81]=static_cast<uint8_t>(rng()%2);
        }
        int64_t a,b,c,d;nativeHull(f.fleet.data(),&a);nativeMax(f.fleet.data(),&b);
        Check(fleetstats::Hull(l,f.fleet.data(),c,d) && a==c && b==d,"native hull/max parity");
        for(int type=0;type<2;++type){a=c=99;b=d=-21;nativePoints[type](f.fleet.data(),&a,&b);
            Check(fleetstats::Points(l,f.fleet.data(),type,c,d) && a==c && b==d,"native filtered armor/shield parity");
            a=c=99;nativePoints[type](f.fleet.data(),&a,&a);fleetstats::Points(l,f.fleet.data(),type,c,c);
            Check(a==c,"aliased armor/shield accumulators");}
    }
    for(auto& ship:f.ships){Set(ship.data(),l.hull,INT64_MAX/2);Set(ship.data(),0x1100,INT64_MAX);ship[0x80]=ship[0x81]=1;
        for(int type=0;type<2;++type){Set(ship.data(),l.current[type],INT64_MAX/2);Set(ship.data(),l.maximumPoints[type],INT64_MAX);}}
    int64_t a,b;fleetstats::Hull(l,f.fleet.data(),a,b);
    Check(b==INT64_MAX && a>=INT64_MAX/2-4 && a<=INT64_MAX/2,"coupled hull retains half health");
    nativeMax(f.fleet.data(),&b);Check(b!=INT64_MAX,"native max hull overflow reproduced");
    for(int type=0;type<2;++type){a=b=0;fleetstats::Points(l,f.fleet.data(),type,a,b);Check(b==INT64_MAX && a==INT64_MAX/2,"coupled armor/shield retain half health");}
    numeric::Sum numerator,denominator;
    for(int i=0;i<8;++i){numerator.Add(INT64_MAX/2);denominator.Add(INT64_MAX);}
    Check(numeric::Ratio(numerator,denominator,32768)==16383,"wide ratio retains truncation below half");
    numerator={};numerator.Add(INT64_MIN);denominator={};denominator.Add(-1);
    Check(numeric::Ratio(numerator,denominator,1)==INT64_MAX,"wide ratio saturation");
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(mod);
    printf("fleet stats: 20000 native hull/armor/shield fixtures and coupled extremes; %d failures\n",failures);return failures?1:0;
}
