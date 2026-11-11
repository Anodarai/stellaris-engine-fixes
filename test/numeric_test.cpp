// Cross-build native arithmetic reproductions and fake-object parity for fleet power.
#include <array>
#include <cstdio>
#include <fstream>
#include <random>
#include "../src/fleetoverflow.h"
#include "../src/resourcearithmetic.h"

namespace {
int failures = 0, calls = 0, uncachedCalls = 0;
void Check(bool ok, const char* what) {
    if (!ok && ++failures <= 20) printf("FAIL %s\n", what);
}
template<class T> void Set(uint8_t* p, size_t offset, T value) { memcpy(p+offset, &value, sizeof(value)); }
int64_t* __fastcall ShipPower(uint8_t* ship, int64_t* out, bool cached) {
    ++calls; if (!cached) ++uncachedCalls;
    memcpy(out, ship + (cached ? 64 : 72), 8);
    return out;
}
void Jump(uint8_t* at, const void* target) {
    const uint8_t prefix[] = {0xff,0x25,0,0,0,0};
    memcpy(at,prefix,6); memcpy(at+6,&target,8);
}
void Relative(uint8_t* operand, uint8_t* target) {
    int32_t delta = static_cast<int32_t>(target-operand-4);
    memcpy(operand,&delta,4);
}

struct NativeFleet {
    uint8_t* code = nullptr;
    fleetoverflow::PowerFn power = nullptr;
    fleetoverflow::HealthFn health = nullptr;
    fleetoverflow::Locations layout;
    explicit NativeFleet(const fleetoverflow::Locations& loc) : layout(loc) {
        code = static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        if (!code) return;
        memcpy(code,loc.power,fleetoverflow::kPowerSize);
        memcpy(code+512,loc.healthPower,fleetoverflow::kHealthSize);
        Jump(code+1024,reinterpret_cast<void*>(&ShipPower));
        Relative(code+101,code+4096); Relative(code+149,code+4104); Relative(code+163,code+1024);
        Relative(code+512+325,code+4096); Relative(code+512+373,code+4104); Relative(code+512+386,code+1024);
        // Execute the same stolen prologue + absolute continuation used by the runtime fallback.
        memcpy(code+1056,code,fleetoverflow::kPowerStolen);
        Jump(code+1056+fleetoverflow::kPowerStolen,code+fleetoverflow::kPowerStolen);
        memcpy(code+1120,code+512,fleetoverflow::kHealthStolen);
        Jump(code+1120+fleetoverflow::kHealthStolen,code+512+fleetoverflow::kHealthStolen);
        DWORD old;
        if (!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)) return;
        FlushInstructionCache(GetCurrentProcess(),code,4096);
        power = reinterpret_cast<fleetoverflow::PowerFn>(code+1056);
        health = reinterpret_cast<fleetoverflow::HealthFn>(code+1120);
        layout.shipDb = reinterpret_cast<uint8_t**>(code+4096);
        layout.nullShip = reinterpret_cast<uint8_t**>(code+4104);
        layout.shipPower = &ShipPower;
    }
    ~NativeFleet() { if (code) VirtualFree(code,0,MEM_RELEASE); }
};

struct Fixture {
    std::array<uint8_t,0x10008> fleet{};
    std::array<uint8_t,0x28> db{};
    std::array<uint8_t,128> entries{};
    std::array<std::array<uint8_t,256>,8> ships{};
    std::array<uint8_t,256> nullShip{};
    std::array<uint32_t,8> ids{};
    NativeFleet& native;
    explicit Fixture(NativeFleet& n) : native(n) { Reset(); }
    void Reset() {
        auto& l = native.layout;
        *l.shipDb = db.data(); *l.nullShip = nullShip.data();
        Set(db.data(),0x18,entries.data()); Set(db.data(),0x20,uint32_t(8));
        Set(fleet.data(),l.ships,ids.data()); Count(8); Flags(0,0); Health(100*32768,100*32768);
        Set(nullShip.data(),64,int64_t(7)); Set(nullShip.data(),72,int64_t(11));
        for (int i=0;i<8;++i) {
            ids[i] = 0x01000000U + i;
            Set(ships[i].data(),l.shipId,ids[i]);
            Set(entries.data(),8+i*16,ships[i].data());
            Power(i,(i+1)*32768,(i+2)*32768);
        }
    }
    void Count(int32_t n) { Set(fleet.data(),native.layout.count,n); }
    void Flags(uint32_t flags,uint8_t type) { Set(fleet.data(),native.layout.flags,flags); fleet[native.layout.type]=type; }
    void Health(int64_t current,int64_t max) {
        Set(fleet.data(),native.layout.currentHealth,current); Set(fleet.data(),native.layout.maxHealth,max);
    }
    void Power(int i,int64_t cached,int64_t fresh) { Set(ships[i].data(),64,cached); Set(ships[i].data(),72,fresh); }
    int64_t Original(bool health,int mode=0,bool cached=true) {
        int64_t result=0;
        auto out = health ? native.health(fleet.data(),&result) : native.power(fleet.data(),&result,mode,cached);
        Check(out==&result,"native fleet return buffer ABI"); return result;
    }
    int64_t Fixed(bool health,int mode=0,bool cached=true) {
        int64_t result=0;
        Check(fleetoverflow::Compute(native.layout,fleet.data(),mode,cached,health,result),"replacement accepts valid fleet");
        return result;
    }
    void Compare(bool health,int mode=0,bool cached=true) {
        calls=uncachedCalls=0; int64_t original=Original(health,mode,cached);
        int oldCalls=calls, oldUncached=uncachedCalls;
        calls=uncachedCalls=0;
        Check(Fixed(health,mode,cached)==original,"fleet ordinary-scale native parity");
        Check(calls==oldCalls && uncachedCalls==oldUncached,"same ship calls and cache selection");
    }
};

void FleetTests(const locate::Image& img) {
    fleetoverflow::Locations loc;
    std::string error;
    if (!fleetoverflow::Locate(img,loc,error)) { Check(false,error.c_str()); return; }
    printf("  fleet power +%llX, health power +%llX\n",
           static_cast<unsigned long long>(loc.power-img.base),static_cast<unsigned long long>(loc.healthPower-img.base));
    NativeFleet native(loc);
    if (!native.power || !native.health) { Check(false,"native fleet copy allocation"); return; }
    {
        std::vector<uint8_t> bytes(native.code,native.code+1024);
        locate::Image fake;
        fake.code.push_back({bytes.data(),bytes.data()+bytes.size()});
        fleetoverflow::Locations found;
        Check(fleetoverflow::Locate(fake,found,error),"synthetic relocated fleet patterns");
        bytes[512+306] ^= 4; // masked count offset must still agree with the other function
        Check(!fleetoverflow::Locate(fake,found,error),"fleet layout cross-check rejects mismatch");
        bytes[512+306] ^= 4;
        bytes.insert(bytes.end(),loc.power,loc.power+fleetoverflow::kPowerSize);
        fake.code[0] = {bytes.data(),bytes.data()+bytes.size()};
        Check(!fleetoverflow::Locate(fake,found,error),"duplicate fleet pattern rejected");
    }
    Fixture f(native);
    for (uint32_t flags : {0U,2U,0x202U}) for (uint8_t type : {uint8_t(0),uint8_t(4),uint8_t(10)})
        for (int count : {0,1,8}) for (int mode : {0,1}) for (bool cached : {false,true}) {
            f.Count(count); f.Flags(flags,type); f.Compare(false,mode,cached); f.Compare(true,mode,cached);
        }
    for (int invalid=0;invalid<5;++invalid) {
        f.Reset(); f.Count(1);
        if (invalid==1) *native.layout.shipDb=nullptr;
        if (invalid==2) f.ids[0]=0x02000000;
        if (invalid==3) Set(f.entries.data(),8,static_cast<uint8_t*>(nullptr));
        if (invalid==4) f.ids[0]=99;
        f.Compare(false); f.Compare(true);
    }
    f.Reset(); f.Count(1); f.Health(0,0); f.Compare(true);
    std::mt19937_64 rng(303);
    f.Reset();
    for (int i=0;i<20000;++i) {
        f.Count(static_cast<int32_t>(rng()%9));
        int64_t max=static_cast<int64_t>(rng()%1000000000000ULL)+1;
        int64_t current=static_cast<int64_t>(rng()%static_cast<uint64_t>(max+1));
        if (i%5==0) current=-current; // signed round/shift behavior too
        f.Health(current,max);
        for (int j=0;j<8;++j) {
            int64_t power=static_cast<int64_t>(rng()%1000000000000ULL);
            if (i%7==0) power=-power;
            f.Power(j,power,power/2);
        }
        f.Compare(false,0,i%2==0); f.Compare(true);
    }
    f.Reset(); f.Count(2); f.Power(0,INT64_MAX,0); f.Power(1,INT64_MAX,0);
    Check(f.Original(false)==-2 && f.Fixed(false)==INT64_MAX,"positive fleet sum overflow");
    f.Power(0,INT64_MIN,0); f.Power(1,INT64_MIN,0);
    Check(f.Original(false)==0 && f.Fixed(false)==INT64_MIN,"negative fleet sum overflow");
    f.Count(3); f.Power(0,INT64_MAX,0); f.Power(1,1,0); f.Power(2,-INT64_MAX,0);
    Check(f.Fixed(false)==1,"wide sum preserves cancellation");
    f.Reset(); f.Count(1); f.Power(0,int64_t(1)<<48,0);
    Check(f.Original(true)<0 && f.Fixed(true)==(int64_t(1)<<48),"health product overflows before shift");
    f.Power(0,100*32768,0); f.Health(int64_t(1)<<48,int64_t(1)<<48);
    Check(f.Original(true)<0 && f.Fixed(true)==100*32768,"health numerator overflows before division");
    f.Count(-1); int64_t untouched=123;
    Check(!fleetoverflow::Compute(native.layout,f.fleet.data(),0,true,false,untouched) && untouched==123,
          "corrupt count fails before changing output");
    printf("  checked fleet parity, lookup/filter/cache cases, signed sums and two health overflow reproductions\n");
}

void ResourceTests(const locate::Image& img) {
    resourcearithmetic::Locations loc;
    std::string error;
    if (!resourcearithmetic::Locate(img,loc,error)) { Check(false,error.c_str()); return; }
    {
        std::vector<uint8_t> bytes(loc.multiply,loc.multiply+resourcearithmetic::kSize);
        locate::Image fake;
        fake.code.push_back({bytes.data(),bytes.data()+bytes.size()});
        resourcearithmetic::Locations found;
        Check(resourcearithmetic::Locate(fake,found,error),"synthetic resource pattern");
        bytes[6]=0xff;
        Check(!resourcearithmetic::Locate(fake,found,error),"negative resource displacement rejected");
        bytes[6]=static_cast<uint8_t>(loc.resourceIndex);
        bytes.insert(bytes.end(),loc.multiply,loc.multiply+resourcearithmetic::kSize);
        fake.code[0] = {bytes.data(),bytes.data()+bytes.size()};
        Check(!resourcearithmetic::Locate(fake,found,error),"duplicate resource pattern rejected");
    }
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!code) { Check(false,"resource native code allocation"); return; }
    memcpy(code,loc.multiply,resourcearithmetic::kSize);
    memcpy(code+256,code,resourcearithmetic::kStolen);
    Jump(code+256+resourcearithmetic::kStolen,code+resourcearithmetic::kStolen);
    DWORD old;
    if (!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)) { Check(false,"resource code protection"); VirtualFree(code,0,MEM_RELEASE); return; }
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    auto original=reinterpret_cast<resourcearithmetic::MultiplyFn>(code+256);
    std::array<uint8_t,256> resource{};
    Set(resource.data(),loc.resourceIndex,1);
    int64_t amounts[3]={111,0,333}; int64_t* values=amounts; int64_t** inner=&values;
    auto table=reinterpret_cast<uint8_t*>(&inner);
    auto test=[&](int64_t amount,int64_t multiplier,bool ordinary) {
        amounts[1]=amount; original(table,resource.data(),&multiplier); int64_t before=amounts[1];
        amounts[1]=amount; resourcearithmetic::Multiply(loc,table,resource.data(),&multiplier);
        Check(amounts[0]==111 && amounts[2]==333,"only selected resource changed");
        if (ordinary) Check(amounts[1]==before,"resource ordinary-scale native parity");
        return before;
    };
    std::mt19937_64 rng(33);
    for (int i=0;i<100000;++i) {
        int64_t a=static_cast<int64_t>(rng()%2000000001ULL)-1000000000;
        int64_t b=static_cast<int64_t>(rng()%2000000001ULL)-1000000000;
        test(a,b,true);
    }
    int64_t before=test(100,-1000000000000000000LL,false);
    Check(before!=-1000000000000000LL && amounts[1]==-1000000000000000LL,"signed slow-path product overflow");
    printf("  resource multiplication +%llX: original %lld, fixed %lld (raw units)\n",
           static_cast<unsigned long long>(loc.multiply-img.base),before,amounts[1]);
    test(INT64_MAX,200000,false); Check(amounts[1]==INT64_MAX,"positive multiplication saturation");
    test(INT64_MIN,200000,false); Check(amounts[1]==INT64_MIN,"negative multiplication saturation");
    test(INT64_MIN,-100000,false); Check(amounts[1]==INT64_MAX,"minimum times negative unity saturation");
    amounts[1]=-150000;
    resourcearithmetic::Multiply(loc,table,resource.data(),&amounts[1]);
    Check(amounts[1]==225000,"multiplier aliases selected amount");
    VirtualFree(code,0,MEM_RELEASE);
}

void Oracle(const char* path) {
    std::ifstream input(path);
    Check(input.good(),"open independent arithmetic oracle");
    int64_t a,b,d,want; int mode; size_t count=0;
    while (input>>a>>b>>d>>mode>>want) {
        Check(d!=0,"oracle has a nonzero divisor");
        if (d) Check(numeric::MulDiv(a,b,d,static_cast<numeric::Rounding>(mode))==want,"Python arbitrary-precision oracle");
        ++count;
    }
    Check(input.eof() && count>0,"complete oracle file");
    printf("Arithmetic oracle: %zu cases\n",count);
}
void RatioOracle(const char* path) {
    std::ifstream input(path);numeric::Sum a,b;uint64_t scale;int mode;int64_t want;size_t count=0;
    while(input>>a.low>>a.high>>b.low>>b.high>>scale>>mode>>want){
        Check(numeric::Ratio(a,b,scale,static_cast<numeric::Rounding>(mode))==want,"Python wide ratio oracle");++count;
    }
    Check(input.eof() && count>0,"complete wide ratio oracle");printf("Wide ratio oracle: %zu cases\n",count);
}
} // namespace

int main(int argc,char** argv) {
    if (argc<2) { printf("usage: numeric_test <exe> [more...] | --oracle <file>\n"); return 2; }
    if (argc==3 && strcmp(argv[1],"--oracle")==0) Oracle(argv[2]);
    else if (argc==3 && strcmp(argv[1],"--ratio-oracle")==0) RatioOracle(argv[2]);
    else for (int i=1;i<argc;++i) {
        HMODULE mod=LoadLibraryExA(argv[i],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!mod) { Check(false,"map executable"); continue; }
        auto base=reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod)&~uintptr_t(3));
        auto img=locate::Describe(base); printf("%s\n",argv[i]);
        FleetTests(img); ResourceTests(img); FreeLibrary(mod);
    }
    printf("%s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
