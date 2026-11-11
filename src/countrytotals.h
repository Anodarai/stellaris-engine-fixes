// Country score producers: preserve signed cancellation across the complete traversal.
#pragma once
#include "numericpatch.h"
#include "numericmath.h"
#include "countrytotals_patterns.h"
namespace countrytotals {
using Predicate=bool(__fastcall*)(uint8_t*);
using WeightFn=int64_t*(__fastcall*)(uint8_t*,int64_t*,uint8_t*);
struct Ref {uint8_t** db=nullptr;uint8_t** nullObject=nullptr;size_t id=0;};
inline uint8_t* Resolve(const Ref& r,uint32_t id){
    auto db=*r.db;uint32_t index=id&0xffffff;
    if(db && index<*reinterpret_cast<uint32_t*>(db+0x20)){
        auto entries=*reinterpret_cast<uint8_t**>(db+0x18);
        auto object=*reinterpret_cast<uint8_t**>(entries+8+size_t(index)*16);
        if(object && *reinterpret_cast<uint32_t*>(object+r.id)==id)return object;
    }
    return *r.nullObject;
}
struct Locations {
    numericpatch::Block blocks[3];Ref fleet,colony,job,pop;
    Predicate excluded[2]{};WeightFn weight=nullptr;
    size_t fleets=0,count=0,flags=0,type=0,power=0;
    size_t colonies=0,colonyCount=0,jobs=0,jobCount=0,pops=0,popCount=0;
};
inline bool Fleet(const Locations& l,uint8_t* manager,bool pure,int64_t& out){
    int count=*reinterpret_cast<int*>(manager+l.count);if(count<0)return false;
    auto ids=*reinterpret_cast<uint32_t**>(manager+l.fleets);numeric::Sum sum;
    for(int i=0;i<count;++i){
        auto f=Resolve(l.fleet,ids[i]);uint8_t type=f[l.type];
        uint32_t flags=*reinterpret_cast<uint32_t*>(f+l.flags);
        bool military=type==0 || (type==10 && (flags&512));
        // The original pure-power path calls both predicates even for excluded fleet types.
        bool a=false,b=false;if(pure){a=l.excluded[0](f);b=l.excluded[1](f);}
        if(!military || a || b)continue;
        if((flags&2) && !(type==10 && (flags&512)))continue;
        sum.Add(*reinterpret_cast<int64_t*>(f+l.power));
    }
    out=sum.Clamp();return true;
}
inline bool Population(const Locations& l,uint8_t* colonies,int64_t& out){
    int count=*reinterpret_cast<int*>(colonies+l.colonyCount);if(count<0)return false;
    auto ids=*reinterpret_cast<uint32_t**>(colonies+l.colonies);numeric::Sum sum;
    for(int i=0;i<count;++i){auto colony=Resolve(l.colony,ids[i]);
        int jobs=*reinterpret_cast<int*>(colony+l.jobCount);if(jobs<0)return false;
        auto jobIds=*reinterpret_cast<uint32_t**>(colony+l.jobs);
        for(int j=0;j<jobs;++j){auto job=Resolve(l.job,jobIds[j]);
            int pops=*reinterpret_cast<int*>(job+l.popCount);if(pops<0)return false;
            auto pairs=*reinterpret_cast<uint32_t**>(job+l.pops);
            for(int k=0;k<pops;++k){int64_t value;l.weight(job,&value,Resolve(l.pop,pairs[size_t(k)*2]));sum.Add(value);}
        }
    }
    out=sum.Clamp();return true;
}
inline bool Military(numericpatch::Context& c,const void* data){int64_t out;
    if(!Fleet(*static_cast<const Locations*>(data),reinterpret_cast<uint8_t*>(c.rcx),false,out))return false;
    *reinterpret_cast<int64_t*>(c.rdx)=out;c.rax=c.rdx;return true;}
inline bool Pure(numericpatch::Context& c,const void* data){int64_t out;
    if(!Fleet(*static_cast<const Locations*>(data),reinterpret_cast<uint8_t*>(c.rcx),true,out))return false;
    *reinterpret_cast<int64_t*>(c.rdx)=out;c.rax=c.rdx;return true;}
inline bool PopulationHook(numericpatch::Context& c,const void* data){int64_t out;
    if(!Population(*static_cast<const Locations*>(data),reinterpret_cast<uint8_t*>(c.rdx),out))return false;
    *reinterpret_cast<int64_t*>(c.rcx)=out;c.rax=c.rcx;return true;}
inline uint8_t* Relative(uint8_t* p,size_t o){return locate::RelTarget(p+o,p+o+4);}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    const char* patterns[]={numericpatterns::kCountryFleet,numericpatterns::kCountryPureFleet,numericpatterns::kPopulationWeight};
    numericpatch::Handler handlers[]={Military,Pure,PopulationHook};size_t stolen[]={6,6,7};
    for(int i=0;i<3;++i){auto& b=l.blocks[i];b.at=locate::FindOne(image,"country score aggregate",patterns[i],error);
        if(!b.at)return false;b.stolen=b.span=stolen[i];b.handler=handlers[i];b.data=&l;b.returns=true;}
    auto a=l.blocks[0].at,b=l.blocks[1].at,p=l.blocks[2].at;
    l.fleet={reinterpret_cast<uint8_t**>(Relative(a,0x30)),reinterpret_cast<uint8_t**>(Relative(a,0x24)),a[0x5d]};
    if(Relative(b,0x43)!=reinterpret_cast<uint8_t*>(l.fleet.db) || Relative(b,0x73)!=reinterpret_cast<uint8_t*>(l.fleet.nullObject)){
        error="country fleet database disagreement";return false;}
    l.fleets=a[5];l.count=a[0xc];l.type=locate::Disp32(a+0x66);l.flags=locate::Disp32(a+0x71);l.power=locate::Disp32(a+0xa4);
    l.excluded[0]=reinterpret_cast<Predicate>(Relative(b,0x95));l.excluded[1]=reinterpret_cast<Predicate>(Relative(b,0xa0));
    l.colony={reinterpret_cast<uint8_t**>(Relative(p,0x38)),reinterpret_cast<uint8_t**>(Relative(p,0x3f)),p[0x9d]};
    l.job={reinterpret_cast<uint8_t**>(Relative(p,0x46)),reinterpret_cast<uint8_t**>(Relative(p,0x4d)),p[0xf4]};
    l.pop={reinterpret_cast<uint8_t**>(Relative(p,0x123)),reinterpret_cast<uint8_t**>(Relative(p,0x155)),p[0x14f]};
    l.colonies=p[0x17];l.colonyCount=p[0x1b];l.jobs=p[0xb2];l.jobCount=p[0xb8];
    l.pops=locate::Disp32(p+0xfd);l.popCount=locate::Disp32(p+0x106);l.weight=reinterpret_cast<WeightFn>(Relative(p,0x162));
    return true;
}
} // namespace countrytotals
