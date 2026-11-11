// Diplomatic consumers keep vote sums and modifiers wide through their final ABI.
#pragma once
#include <array>
#include "diploscores.h"
#include "componentmath.h"
#include "diplovotes_patterns.h"
#include "federationdecision_patterns.h"
namespace diplovotes {
using numeric::Integer;using diploscores::Read;using diploscores::Relative;
struct Locations {
    diploscores::Locations scores;numericpatch::Block blocks[7];
    uint8_t** modifierTypes=nullptr;diploscores::Eligible eligible=nullptr;
};
inline Integer Vote(const Locations& l,uint8_t* country,uint8_t* resolution){
    auto value=diploscores::DecimalToBinary(Integer(Read<int64_t>(country,0x2f70)))+
        diploscores::DecimalToBinary(diploscores::TotalLoans(l.scores,country,resolution,false).total);
    int id=Read<int>(Read<uint8_t*>(Read<uint8_t*>(resolution,0x18),0x9d0),0x48);
    if(id<INT32_MAX){
        if(id<0)throw std::runtime_error("invalid vote modifier id");
        auto type=*l.modifierTypes+size_t(id)*0xb0;int count=Read<int>(country,0x31bc);
        if(count<0)throw std::runtime_error("invalid vote modifier count");
        auto ids=Read<int*>(country,0x31b0);auto values=Read<uint8_t*>(country,0x31d8);int64_t modifier=0;
        for(int i=0;i<count;++i)if(ids[i]==id){modifier=Read<int64_t>(values,size_t(i)*16);break;}
        if(Read<uint32_t>(type,0xa8)&32)modifier=(std::max)(int64_t(0),(std::min)(int64_t(100000),modifier));
        auto factor=Integer(modifier)+Integer(100000),minimum=Integer(Read<int64_t>(type,0xa0));
        if(factor<minimum)factor=minimum;
        value=(value*diploscores::DecimalToBinary(factor)).FloorDivide(Integer(32768));
    }
    return value;
}
inline Integer Side(const Locations& l,uint8_t* resolution,bool against){
    size_t offset=against?0x50:0x38;int count=Read<int>(resolution,offset+12);
    if(count<0)throw std::runtime_error("invalid resolution voter count");
    auto ids=Read<uint32_t*>(resolution,offset);Integer sum;
    for(int i=0;i<count;++i)sum+=Vote(l,diploscores::ResolveCountry(l.scores,ids[i]),resolution);
    return sum;
}
inline std::array<Integer,3> Split(const Locations& l,uint8_t* community,uint8_t* resolution){
    std::array<Integer,3> result{Side(l,resolution,false),Side(l,resolution,true),Integer()};
    int count=Read<int>(community,0x194);if(count<0)throw std::runtime_error("invalid community member count");
    auto ids=Read<uint32_t*>(community,0x188);
    for(int i=0;i<count;++i){auto country=diploscores::ResolveCountry(l.scores,ids[i]);
        if(l.eligible(resolution,country))result[2]+=Vote(l,country,resolution);}
    return result;
}
inline void PublishSplit(const std::array<Integer,3>& values,int64_t* out){
    // All three fields participate in the same comparison/percentage. If the ABI
    // cannot hold them, use one positive divisor instead of clipping each side.
    Integer largest(INT64_MAX);for(const auto& value:values)if(largest<value.Abs())largest=value.Abs();
    for(size_t i=0;i<3;++i)out[i]=(values[i]*Integer(INT64_MAX)/largest).Clamp();
}
inline Integer Federation(const Locations& l,uint8_t* country,uint8_t* proposal,uint8_t* federation){
    if(Read<int>(Read<uint8_t*>(proposal,0x40),0x50)==0x40ed &&
       diploscores::ResolveCountry(l.scores,Read<uint32_t>(proposal,0x24))==country)return Integer();
    if(Read<uint8_t>(Read<uint8_t*>(federation,0x80),0x2be))return Integer(100000);
    auto binary=diploscores::DecimalToBinary(Integer(Read<int64_t>(country,0x2f70)));
    auto decimal=binary.Abs()*Integer(100000);auto result=(decimal+Integer(16384))/Integer(32768);
    return binary.Negative()?-result:result;
}
inline Integer Needed(uint8_t* split,bool ignoreAbstain,bool unanimous){
    Integer total(Read<int64_t>(split,0x18));if(ignoreAbstain)total-=Integer(Read<int64_t>(split,0x10));
    return unanimous?total:componentmath::Divide(total,Integer(200000))+Integer(100);
}
inline std::array<Integer,4> FederationSplit(const Locations& l,uint8_t* context,uint8_t* federation,bool valid){
    std::array<Integer,4> sums{};int count=Read<int>(context,0x2c);
    if(count<0)throw std::runtime_error("invalid federation vote count");
    auto entries=Read<uint8_t*>(context,0x20);auto proposal=Read<uint8_t*>(context,8);
    for(int i=0;i<count;++i){auto entry=entries+size_t(i)*16;
        auto vote=valid?Federation(l,diploscores::ResolveCountry(l.scores,Read<uint32_t>(entry,8)),proposal,federation):Integer(100000);
        int kind=Read<int>(entry,12);sums[kind==0?0:kind==1?1:2]+=vote;sums[3]+=vote;}
    return sums;
}
inline bool Decision(numericpatch::Context& c,const void* data){
    try{const auto& l=*static_cast<const Locations*>(data);
        auto sums=FederationSplit(l,*reinterpret_cast<uint8_t**>(c.rbp+0x14b8),
            *reinterpret_cast<uint8_t**>(c.rbp+0x14c8),*reinterpret_cast<uint8_t*>(c.rbp+0x14b0)!=0);
        bool wide=false;for(const auto& sum:sums)if(!(sum==Integer(sum.Clamp())))wide=true;
        if(!wide)return false;
        auto total=sums[3];if(uint8_t(c.r8))total-=sums[2];
        auto needed=uint8_t(c.r9)?total:componentmath::Divide(total,Integer(200000))+Integer(100);
        // Only the two branch predicates consume these registers after this
        // call. Encode their exact answers while leaving all event effects native.
        c.r12=sums[0]<needed?uint64_t(-1):1;
        c.r15=sums[3]-needed<sums[1]?1:uint64_t(-1);c.rbx=0;
        *reinterpret_cast<int64_t*>(c.rcx)=0;c.rax=c.rcx;return true;
    }catch(...){return false;}
}
inline Integer Borrowed(const Locations& l,uint8_t* country,uint8_t* table){
    int mask=Read<int>(table,0x14);if(mask<0)throw std::runtime_error("invalid favor table mask");
    size_t end=size_t(mask)+Read<uint8_t>(table,0x18)+1;auto data=Read<uint8_t*>(table,8);Integer total;
    for(size_t i=0;i<end;++i){auto entry=data+i*12;if(!entry[0])continue;
        total+=diploscores::DecimalToBinary(diploscores::Loan(l.scores,country,
            diploscores::ResolveCountry(l.scores,Read<uint32_t>(entry,4)),Read<int32_t>(entry,8)));}
    return total;
}
template<int Kind>bool Hook(numericpatch::Context& c,const void* data){
    try{const auto& l=*static_cast<const Locations*>(data);Integer value;uint64_t output=c.rdx;
        if constexpr(Kind==0)value=Federation(l,reinterpret_cast<uint8_t*>(c.rcx),reinterpret_cast<uint8_t*>(c.r8),reinterpret_cast<uint8_t*>(c.r9));
        if constexpr(Kind==1){output=c.rcx;value=Needed(reinterpret_cast<uint8_t*>(c.rdx),uint8_t(c.r8)!=0,uint8_t(c.r9)!=0);}
        if constexpr(Kind==2){auto result=Split(l,reinterpret_cast<uint8_t*>(c.rdx),reinterpret_cast<uint8_t*>(c.r8));
            PublishSplit(result,reinterpret_cast<int64_t*>(c.rcx));c.rax=c.rcx;return true;}
        if constexpr(Kind==3){output=c.rcx;value=Borrowed(l,reinterpret_cast<uint8_t*>(c.rdx),reinterpret_cast<uint8_t*>(c.r8));}
        if constexpr(Kind==4 || Kind==5)value=Side(l,reinterpret_cast<uint8_t*>(c.rcx),Kind==5);
        *reinterpret_cast<int64_t*>(output)=value.Clamp();c.rax=output;return true;
    }catch(...){return false;}
}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    if(!diploscores::Locate(image,l.scores,error))return false;
    const char* patterns[]={numericpatterns::kFederationVote,numericpatterns::kVotesNeeded,numericpatterns::kVoteSplit,
        numericpatterns::kBorrowedVotes,numericpatterns::kVotesFor,numericpatterns::kVotesAgainst};
    numericpatch::Handler handlers[]={Hook<0>,Hook<1>,Hook<2>,Hook<3>,Hook<4>,Hook<5>};size_t stolen[]={7,5,5,5,6,6};
    for(size_t i=0;i<6;++i){auto& b=l.blocks[i];b.at=locate::FindOne(image,"diplomatic vote consumer",patterns[i],error);
        if(!b.at)return false;b.stolen=b.span=stolen[i];b.handler=handlers[i];b.data=&l;b.returns=true;}
    auto split=l.blocks[2].at;l.modifierTypes=reinterpret_cast<uint8_t**>(Relative(split,0x199));
    l.eligible=reinterpret_cast<diploscores::Eligible>(Relative(split,0xbd));
    if(Relative(split,0x36)!=l.blocks[4].at || Relative(split,0x49)!=l.blocks[5].at ||
       Relative(split,0xfd)!=l.scores.blocks[4].at || Relative(split,0x83)!=reinterpret_cast<uint8_t*>(l.scores.countryDb) ||
       Relative(l.blocks[4].at,0x157)!=reinterpret_cast<uint8_t*>(l.modifierTypes)){
        error="diplomatic vote caller cross-check failed";return false;}
    auto decision=locate::FindOne(image,"federation vote decision",numericpatterns::kFederationDecision,error);
    if(!decision || Relative(decision,0x214)!=l.blocks[1].at){error="federation vote threshold call changed";return false;}
    auto& d=l.blocks[6];d.at=decision+0x213;d.stolen=d.span=5;d.ripOperand=1;d.handler=Decision;d.data=&l;
    return true;
}
} // namespace diplovotes
