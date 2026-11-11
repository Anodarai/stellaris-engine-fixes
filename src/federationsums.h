// Preserve the engine's voter selection and AI evaluations; only their numeric
// accumulators and final predicates change. Each reviewed loop has one init and
// one finish; frame-keyed TLS also isolates nested calls and concurrent workers.
#pragma once
#include <map>
#include "diplovotes.h"
#include "federationsums_patterns.h"
namespace federationsums {
using numeric::Integer;using C=numericpatch::Context;
struct State {Integer yes,total,unknown;};
inline thread_local std::map<std::pair<uint64_t,int>,State> states;
struct Spec {int group,action;};
constexpr Spec specs[]={{0,0},{0,1},{0,2},{0,3},{1,0},{1,1},{1,2},{1,3},{1,4},
    {2,0},{2,1},{2,2},{3,0},{3,1},{3,2},{3,3}};
constexpr size_t kCount=sizeof(specs)/sizeof(specs[0]);
struct Locations;
struct Data {const Spec* spec;const Locations* locations;};
struct Locations {numericpatch::Block blocks[kCount];Data data[kCount]{};int32_t* accept=nullptr;int32_t* reject=nullptr;};
inline std::string Digits(Integer value){
    bool negative=value.Negative();value=value.Abs();std::string text;
    do{auto digit=(value%Integer(10)).Clamp();text.push_back(char('0'+digit));value=value/Integer(10);}while(!value.Zero());
    if(negative)text.push_back('-');std::reverse(text.begin(),text.end());return text;
}
inline void CompareFlags(C& c,bool less,bool equal){
    c.flags&=~uint64_t(0x8c1);if(less)c.flags|=0x80;if(equal)c.flags|=0x40;
}
inline bool Handler(C& c,const void* opaque){
    try{const auto& d=*static_cast<const Data*>(opaque);const auto& spec=*d.spec;
        auto key=std::make_pair(c.rbp,spec.group);
        if(spec.action==0){State s;
            if(spec.group==0)s.yes=Integer(*reinterpret_cast<int64_t*>(c.Rsp()+0x58));
            if(spec.group==1)s.yes=s.total=Integer(int64_t(c.rdi));
            if(spec.group==3)s.yes=s.total=Integer(*reinterpret_cast<int64_t*>(c.rbp+0x2d0));
            states[key]=std::move(s);return false;}
        auto it=states.find(key);if(it==states.end())return false;auto& s=it->second;
        if(spec.group==0){
            if(spec.action==1)s.yes+=Integer(*reinterpret_cast<int64_t*>(c.Rsp()+0x58));
            if(spec.action==2)s.total+=Integer(*reinterpret_cast<int64_t*>(c.Rsp()+0x58));
            if(spec.action==3){bool accepted=uint8_t(c.rax)?!(Integer()<s.total):s.total<s.yes;
                states.erase(it);c.rax=(c.rax&~uint64_t(255))|uint64_t(accepted);return true;}
        }else if(spec.group==1){
            if(spec.action==1)s.total+=Integer(*reinterpret_cast<int64_t*>(c.Rsp()+0x38));
            if(spec.action==2)s.unknown+=Integer(int64_t(c.rbx));
            if(spec.action==3)s.yes+=Integer(int64_t(c.rbx));
            if(spec.action==4){int32_t result;
                if(uint8_t(c.rax))result=s.yes+s.unknown<s.total?*d.locations->reject:s.yes<s.total?0:*d.locations->accept;
                else result=s.total<s.yes*Integer(2)?*d.locations->accept:s.total<(s.yes+s.unknown)*Integer(2)?0:*d.locations->reject;
                states.erase(it);c.rax=uint32_t(result);return true;}
        }else if(spec.group==2){
            if(spec.action==1){auto vote=Integer(*reinterpret_cast<int64_t*>(c.rbp+0x140));s.total+=vote;
                if(*reinterpret_cast<uint8_t*>(c.r14+0x1c0))s.yes+=vote;}
            if(spec.action==2){auto yes=s.yes/Integer(100000),total=s.total/Integer(100000);
                bool accepted=int32_t(c.rbx)==4?!(yes<total):total<yes*Integer(2);
                auto text=Digits(yes)+" / "+Digits(total);
                // The native formatter owns a 128-byte stack buffer. A signed
                // int32 member count times int64 weights needs at most 52 bytes.
                if(text.size()>=128)return false;
                memcpy(reinterpret_cast<void*>(c.rbp+0x80),text.c_str(),text.size()+1);
                c.rdi=accepted?0x47:0x52;c.rax=c.rbp+0x80;states.erase(it);return true;}
        }else{
            if(spec.action==1)s.yes+=Integer(int64_t(c.rbx));
            if(spec.action==2)s.total+=Integer(int64_t(c.rbx));
            if(spec.action==3){auto needed=*reinterpret_cast<uint8_t*>(c.rbp+0x2c8)?s.total:
                    componentmath::Divide(s.total,Integer(200000))+Integer(100);
                CompareFlags(c,s.yes<needed,s.yes==needed);states.erase(it);return true;}
        }
        return false; // The native instructions still advance the loop/metadata.
    }catch(...){states.erase(std::make_pair(c.rbp,static_cast<const Data*>(opaque)->spec->group));return false;}
}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    using namespace numericpatterns;
    const char* patterns[]={kFederationAccept,kFederationPredict,kFederationLawInit,kFederationLawAdd,kFederationLawText,
        kFederationTypeInit,kFederationTypeAdd,kFederationTypeDecision};uint8_t* p[8]{};
    for(int i=0;i<8;++i){p[i]=locate::FindOne(image,"federation sum consumer",patterns[i],error);if(!p[i])return false;}
    l.accept=reinterpret_cast<int32_t*>(diploscores::Relative(p[1],0x57b));
    l.reject=reinterpret_cast<int32_t*>(diploscores::Relative(p[1],0x58c));
    uint8_t* sites[]={p[0]+0xbd,p[0]+0x809,p[0]+0x810,p[0]+0x82e,
        p[1]+0xb7,p[1]+0x37b,p[1]+0x39d,p[1]+0x3c5,p[1]+0x558,
        p[2],p[3],p[4],p[5],p[6],p[6]+7,p[7]};
    size_t stolen[]={5,5,5,7,6,5,10,10,7,6,7,7,7,7,10,7};
    size_t spans[]={5,5,5,0x12,6,5,10,10,0x3c,6,7,0x3a,7,7,10,0xe7};
    for(size_t i=0;i<kCount;++i){auto& b=l.blocks[i];b.at=sites[i];b.stolen=stolen[i];b.span=spans[i];b.handler=Handler;
        l.data[i]={&specs[i],&l};b.data=&l.data[i];}
    l.blocks[3].shortBranch=2;l.blocks[8].shortBranch=2;
    l.blocks[6].ripOperand=l.blocks[7].ripOperand=l.blocks[14].ripOperand=6;
    return true;
}
} // namespace federationsums
