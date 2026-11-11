#include <array>
#include <random>
#include <cstdio>
#include "../src/countrytotals.h"
namespace {
int failures=0;
void Check(bool b,const char* name){if(!b && ++failures<=20)printf("FAIL %s\n",name);}
template<class T>void Set(uint8_t* p,size_t o,T v){memcpy(p+o,&v,sizeof(v));}
bool __fastcall ExcludedA(uint8_t* p){return p[0x100]!=0;}
bool __fastcall ExcludedB(uint8_t* p){return p[0x101]!=0;}
int64_t* __fastcall Weight(uint8_t*,int64_t* out,uint8_t* p){memcpy(out,p+0x100,8);return out;}
void Jump(uint8_t* p,const void* target){uint8_t j[]={0xff,0x25,0,0,0,0};memcpy(p,j,6);memcpy(p+6,&target,8);}
struct Database {
    std::array<std::array<uint8_t,0x1400>,8> objects{};
    std::array<uint8_t,0x1400> nullObject{};std::array<uint8_t,136> entries{};std::array<uint8_t,40> db{};
    uint8_t* dbPointer=db.data();uint8_t* nullPointer=nullObject.data();std::array<uint32_t,8> ids{};
    explicit Database(countrytotals::Ref& r){
        r.db=&dbPointer;r.nullObject=&nullPointer;Set(db.data(),0x18,entries.data());Set(db.data(),0x20,uint32_t(8));
        for(size_t i=0;i<8;++i){ids[i]=0x07000000|static_cast<uint32_t>(i);Set(objects[i].data(),r.id,ids[i]);Set(entries.data(),8+i*16,objects[i].data());}
    }
};
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto mod=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!mod)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod)&~uintptr_t(3)));
    countrytotals::Locations l;std::string error;if(!countrytotals::Locate(image,l,error)){puts(error.c_str());return 1;}
    Check(l.fleet.id==0x30 && l.colony.id==0x10 && l.job.id==0x10 && l.pop.id==0x10,"decoded object IDs");
    Check(l.type==0x1280 && l.flags==0x1268 && l.power==0x308,"decoded fleet fields");
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,32768,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!code)return 2;
    struct Binding{uint8_t* from;uint8_t* to;};std::vector<Binding> bindings;
    auto bind=[&](uint8_t* from,uint8_t* to){bindings.push_back({from,to});};
    countrytotals::Ref* refs[]={&l.fleet,&l.colony,&l.job,&l.pop};
    for(size_t i=0;i<4;++i){bind(reinterpret_cast<uint8_t*>(refs[i]->db),code+8192+i*16);bind(reinterpret_cast<uint8_t*>(refs[i]->nullObject),code+8200+i*16);}
    bind(reinterpret_cast<uint8_t*>(l.excluded[0]),code+4096);bind(reinterpret_cast<uint8_t*>(l.excluded[1]),code+4128);bind(reinterpret_cast<uint8_t*>(l.weight),code+4160);
    Jump(code+4096,reinterpret_cast<void*>(&ExcludedA));Jump(code+4128,reinterpret_cast<void*>(&ExcludedB));Jump(code+4160,reinterpret_cast<void*>(&Weight));
    Database fleet(l.fleet),colony(l.colony),job(l.job),pop(l.pop);
    Database* dbs[]={&fleet,&colony,&job,&pop};for(size_t i=0;i<4;++i){Set(code,8192+i*16,dbs[i]->dbPointer);Set(code,8200+i*16,dbs[i]->nullPointer);}
    l.excluded[0]=ExcludedA;l.excluded[1]=ExcludedB;l.weight=Weight;
    const size_t sizes[]={numericpatterns::kCountryFleetSize,numericpatterns::kCountryPureFleetSize,numericpatterns::kPopulationWeightSize};
    for(int fn=0;fn<3;++fn){auto p=code+fn*1024;auto original=l.blocks[fn].at;memcpy(p,original,sizes[fn]);
        for(size_t off=0;off+4<=sizes[fn];++off)for(const auto& b:bindings)if(locate::RelTarget(original+off,original+off+4)==b.from){int32_t rel=static_cast<int32_t>(b.to-p-off-4);memcpy(p+off,&rel,4);}
        auto& block=l.blocks[fn];block.at=code+12288+fn*1024;memcpy(block.at,p,sizes[fn]);
        // Successful leaf hooks never execute these copied relative references.
        std::vector<uint8_t> jump;Check(numericpatch::Prepare(block,code+16384+fn*1024,1024,jump),"prepare country leaf stub");memcpy(block.at,jump.data(),jump.size());
    }
    using FleetFn=int64_t*(__fastcall*)(uint8_t*,int64_t*);using PopFn=int64_t*(__fastcall*)(int64_t*,uint8_t*);
    std::array<uint8_t,64> manager{},colonies{};Set(manager.data(),l.fleets,fleet.ids.data());Set(manager.data(),l.count,8);
    Set(colonies.data(),l.colonies,colony.ids.data());Set(colonies.data(),l.colonyCount,8);
    std::array<uint32_t,16> pairs{};for(size_t i=0;i<8;++i){pairs[i*2]=pop.ids[i];Set(colony.objects[i].data(),l.jobs,job.ids.data());Set(colony.objects[i].data(),l.jobCount,8);Set(job.objects[i].data(),l.pops,pairs.data());Set(job.objects[i].data(),l.popCount,8);}
    std::mt19937_64 rng(98411);
    for(int n=0;n<20000;++n){for(size_t i=0;i<8;++i){auto p=fleet.objects[i].data();p[l.type]=static_cast<uint8_t>(rng()%12);Set(p,l.flags,uint32_t(rng()%1024));Set(p,l.power,static_cast<int64_t>(rng()%1000000000000ULL)-100000000000LL);p[0x100]=rng()%2;p[0x101]=rng()%2;Set(pop.objects[i].data(),0x100,static_cast<int64_t>(rng()%1000000000)-100000000LL);}
        for(int pure=0;pure<2;++pure){int64_t a,b;reinterpret_cast<FleetFn>(code+pure*1024)(manager.data(),&a);reinterpret_cast<FleetFn>(l.blocks[pure].at)(manager.data(),&b);Check(a==b,"native country fleet parity");}
        int64_t a,b;reinterpret_cast<PopFn>(code+2048)(&a,colonies.data());reinterpret_cast<PopFn>(l.blocks[2].at)(&b,colonies.data());Check(a==b,"native nested population parity");
    }
    for(auto& f:fleet.objects){f[l.type]=0;Set(f.data(),l.flags,uint32_t(0));f[0x100]=f[0x101]=0;Set(f.data(),l.power,INT64_MAX);}
    for(int pure=0;pure<2;++pure){int64_t a,b;reinterpret_cast<FleetFn>(code+pure*1024)(manager.data(),&a);reinterpret_cast<FleetFn>(l.blocks[pure].at)(manager.data(),&b);Check(a<0 && b==INT64_MAX,"native country wrap corrected");}
    for(auto& p:pop.objects)Set(p.data(),0x100,INT64_MAX);
    int64_t a,b;reinterpret_cast<PopFn>(code+2048)(&a,colonies.data());reinterpret_cast<PopFn>(l.blocks[2].at)(&b,colonies.data());Check(a<0 && b==INT64_MAX,"native population wrap corrected");
    for(size_t i=0;i<8;++i)Set(fleet.objects[i].data(),l.power,i<4?INT64_MAX:-INT64_MAX);
    countrytotals::Fleet(l,manager.data(),false,a);Check(a==0,"country cancellation remains exact");
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(mod);printf("country totals: 20000 native fixtures, emitted leaf stubs and extreme totals; %d failures\n",failures);return failures?1:0;
}
