// Complete diplomatic score producers. Keep intermediates exact until the native output boundary.
#pragma once
#include "numericbig.h"
#include "numericpatch.h"
#include "diploproducers_patterns.h"
namespace diploscores {
using numeric::Integer;
template<class T>T Read(const uint8_t* p,size_t offset){return *reinterpret_cast<const T*>(p+offset);}
using Eligible=bool(__fastcall*)(uint8_t*,uint8_t*);
using Predicate=bool(__fastcall*)(uint8_t*);
using Relation=uint8_t*(__fastcall*)(uint8_t*,uint8_t*);
using Allocate=void*(__fastcall*)(size_t);
constexpr size_t kBlockCount=5;
struct Locations {
    numericpatch::Block blocks[kBlockCount];
    const int64_t *techDivisor=nullptr,*techBase=nullptr,*repeatWeight=nullptr,*economicDivisor=nullptr;
    const int64_t *jobBase=nullptr,*happinessFactor=nullptr,*favorFactor=nullptr,*missingResource=nullptr;
    uint8_t **resources=nullptr,**nullModule=nullptr;
    Eligible eligible=nullptr;Predicate hasHappiness=nullptr;Relation relation=nullptr;
    Eligible blocked=nullptr;Allocate allocate=nullptr;const float* growth=nullptr;
    uint8_t **countryDb=nullptr,**nullCountry=nullptr;
    size_t technologies=0,technologyCount=0,income=0,jobPops=0,jobPopCount=0,happiness=0;
};
inline bool VirtualPredicate(uint8_t* object,size_t slot){
    return reinterpret_cast<Predicate>(Read<uint8_t*>(Read<uint8_t*>(object,0),slot))(object);
}
inline Integer DecimalToBinary(const Integer& value){return value*Integer(32768)/Integer(100000);}
inline Integer Technology(const Locations& l,uint8_t* country){
    Integer total(*l.techBase),divisor((std::max)(*l.techDivisor,int64_t(32768)));
    auto entries=Read<uint8_t*>(country,l.technologies);int count=Read<int>(country,l.technologyCount);
    if(count<0)throw std::runtime_error("invalid technology count");
    for(int i=0;i<count;++i){auto entry=entries+size_t(i)*40;auto tech=Read<uint8_t*>(entry,16);
        auto value=DecimalToBinary(Integer(Read<int>(entry,8))*Integer(Read<int64_t>(tech,0x68)));
        if(!value.Positive())continue;
        if(Read<int>(tech,0x588)<0)value=(value*Integer(*l.repeatWeight)).FloorDivide(Integer(32768));
        total+=value*Integer(32768)/divisor;
    }
    return total<Integer(32768)?Integer(32768):total;
}
inline Integer ResourceRatio(int64_t numerator,int64_t denominator){
    numerator=(std::max)(numerator,int64_t(100000));denominator=(std::max)(denominator,int64_t(100000));
    // Retain the native large-denominator approximation and split rounding.
    if(numerator<92233720368548LL)return Integer(numerator)*Integer(100000)/Integer(denominator);
    if(denominator>=10000000000LL)return Integer(numerator)/Integer(denominator/100000);
    int64_t whole=(numerator/100000)*100000;
    return Integer(numerator%100000)*Integer(100000)/Integer(denominator)+
        Integer(whole%denominator)*Integer(100000)/Integer(denominator)+
        Integer(whole/denominator)*Integer(100000);
}
inline Integer Economic(const Locations& l,uint8_t* country){
    auto manager=*l.resources;int count=Read<int>(manager,0x14);
    if(count<0)throw std::runtime_error("invalid resource count");
    auto entries=Read<uint8_t**>(manager,8);auto table=Read<uint8_t*>(country,l.income);
    int tableCount=Read<int>(table,12);if(tableCount<0)throw std::runtime_error("invalid income count");
    Integer total,divisor((std::max)(*l.economicDivisor,int64_t(32768)));
    for(int i=0;i<count;++i){auto resource=entries[i];if(!l.eligible(resource,country))continue;
        int index=Read<int>(resource,0x18);
        int64_t income=index<0 || index>=tableCount?*l.missingResource:Read<int64_t>(Read<uint8_t*>(table,0),size_t(index)*8);
        auto ratio=ResourceRatio(Read<int64_t>(resource,0x160),Read<int64_t>(resource,0x158));
        auto product=DecimalToBinary(ratio)*DecimalToBinary(Integer(income));
        total+=product.FloorDivide(Integer(32768))*Integer(32768)/divisor;
    }
    return total<Integer(32768)?Integer(32768):total;
}
inline Integer Job(const Locations& l,uint8_t* job,uint8_t* pop){
    if(!VirtualPredicate(pop+8,8))return Integer();
    auto owner=Read<uint8_t*>(job,0x18);
    if(VirtualPredicate(owner,8) &&
        (VirtualPredicate(Read<uint8_t*>(owner,0xaa8),8) || !(Read<uint32_t>(owner,0x928)&8)))return Integer();
    int count=Read<int>(job,l.jobPopCount);if(count<0)throw std::runtime_error("invalid job pop count");
    auto pairs=Read<int32_t*>(job,l.jobPops);int32_t amount=0;
    for(int i=0;i<count;++i)if(pairs[size_t(i)*2]==Read<int32_t>(pop,0x10)){amount=pairs[size_t(i)*2+1];break;}
    Integer total=Integer(amount)*Integer(*l.jobBase);
    if(l.hasHappiness(pop)){
        auto extra=total*Integer(*l.happinessFactor)/Integer(100000);
        extra=extra*Integer(Read<int64_t>(pop,l.happiness))/Integer(100000);
        total+=extra;
    }
    return total;
}
inline Integer Loan(const Locations& l,uint8_t* country,uint8_t* lender,int32_t amount){
    auto module=Read<uint8_t*>(country,0x2a90);
    if(!VirtualPredicate(module,0x80))module=*l.nullModule;
    auto relation=l.relation(Read<uint8_t*>(module,0x10)+0x2b78,lender);
    if(Read<int>(relation,0x108)<amount)return Integer();
    return Integer(amount)*Integer(*l.favorFactor)*Integer(Read<int64_t>(lender,0x2f70))/Integer(100000);
}
struct LoanEntry {uint32_t country,padding=0;int64_t value;};
static_assert(sizeof(LoanEntry)==16,"native loan detail entry");
struct Loans {Integer total;std::vector<LoanEntry> entries;};
inline uint8_t* ResolveCountry(const Locations& l,uint32_t id){
    auto db=*l.countryDb;uint32_t index=id&0xffffff;
    if(db && index<Read<uint32_t>(db,0x20)){
        auto p=Read<uint8_t*>(Read<uint8_t*>(db,0x18),8+size_t(index)*16);
        if(p && Read<uint32_t>(p,0x20)==id)return p;
    }
    return *l.nullCountry;
}
inline Loans TotalLoans(const Locations& l,uint8_t* country,uint8_t* resolution,bool details){
    Loans result;if(l.blocked(resolution,country))return result;
    int voters=Read<int>(resolution,0x44),borrowed=Read<int>(country,0x30e4);
    if(voters<0 || borrowed<0)throw std::runtime_error("invalid loan/voter count");
    auto ids=Read<uint32_t*>(resolution,0x38);
    auto present=[&](uint8_t* object){auto id=Read<uint32_t>(object,0x20);for(int i=0;i<voters;++i)if(ids[i]==id)return true;return false;};
    bool side=present(country);auto rows=Read<uint8_t*>(country,0x30d8);uint8_t* row=nullptr;
    for(int i=0;i<borrowed;++i)if(Read<uint32_t>(rows+size_t(i)*48,8)==Read<uint32_t>(resolution,8)){row=rows+size_t(i)*48;break;}
    if(!row)return result;
    int mask=Read<int>(row,0x24);if(mask<0)throw std::runtime_error("invalid loan hash table");
    size_t end=size_t(mask)+Read<uint8_t>(row,0x28)+1;auto buckets=Read<uint8_t*>(row,0x18);
    for(size_t i=0;i<end;++i){auto entry=buckets+i*12;if(!entry[0])continue;
        uint32_t id=Read<uint32_t>(entry,4);auto lender=ResolveCountry(l,id);
        if(present(lender)==side)continue;
        auto value=Integer(Read<int>(entry,8))*Integer(*l.favorFactor)*Integer(Read<int64_t>(lender,0x2f70))/Integer(100000);
        if(value.Zero())continue;result.total+=value;
        if(details)result.entries.push_back({id,0,value.Clamp()});
    }
    return result;
}
inline void CommitLoans(const Locations& l,const Loans& result,int64_t* out,uint8_t* list){
    *out=result.total.Clamp();if(!list || result.entries.empty())return;
    int count=Read<int>(list,0x14),capacity=Read<int>(list,0x10);
    if(count<0 || capacity<count || result.entries.size()>size_t(INT32_MAX-count))throw std::runtime_error("invalid loan detail list");
    int wanted=count+static_cast<int>(result.entries.size());auto data=Read<LoanEntry*>(list,8);
    if(wanted>capacity){
        int grown=capacity;
        while(grown<wanted){float next=static_cast<float>(grown)* *l.growth;
            int candidate=next>=static_cast<float>(INT32_MAX)?INT32_MAX:static_cast<int>(next);
            grown=(std::max)(grown+1,candidate);}
        auto replacement=static_cast<LoanEntry*>(l.allocate(size_t(grown)*sizeof(LoanEntry)));
        if(!replacement)throw std::bad_alloc();
        if(count)memcpy(replacement,data,size_t(count)*sizeof(LoanEntry));
        memcpy(replacement+count,result.entries.data(),result.entries.size()*sizeof(LoanEntry));
        *reinterpret_cast<int*>(list+0x14)=0;
        using Release=void(__fastcall*)(uint8_t*);
        reinterpret_cast<Release>(Read<uint8_t*>(Read<uint8_t*>(list,0),0x18))(list);
        *reinterpret_cast<LoanEntry**>(list+8)=replacement;*reinterpret_cast<int*>(list+0x10)=grown;
    }else memcpy(data+count,result.entries.data(),result.entries.size()*sizeof(LoanEntry));
    *reinterpret_cast<int*>(list+0x14)=wanted;
}
// Once the result/list is being committed, never replay allocation/free side effects on fault.
inline bool CommitLoansChecked(const Locations& l,const Loans& result,int64_t* out,uint8_t* list){
    __try {CommitLoans(l,result,out,list);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
inline bool TotalLoansHook(numericpatch::Context& c,const void* data){
    auto& l=*const_cast<Locations*>(static_cast<const Locations*>(data));bool committing=false;
    try {
        auto result=TotalLoans(l,reinterpret_cast<uint8_t*>(c.rcx),reinterpret_cast<uint8_t*>(c.r8),c.r9!=0);
        committing=true;
        if(!CommitLoansChecked(l,result,reinterpret_cast<int64_t*>(c.rdx),reinterpret_cast<uint8_t*>(c.r9)))l.blocks[4].failed=true;
        c.rax=c.rdx;return true;
    }catch(...){if(committing){l.blocks[4].failed=true;c.rax=c.rdx;return true;}return false;}
}
template<int Kind>bool Hook(numericpatch::Context& c,const void* data){
    try {
        const auto& l=*static_cast<const Locations*>(data);auto country=reinterpret_cast<uint8_t*>(c.rcx);
        Integer value;
        if constexpr(Kind==0)value=Technology(l,country);
        if constexpr(Kind==1)value=Economic(l,country);
        if constexpr(Kind==2)value=Job(l,country,reinterpret_cast<uint8_t*>(c.r8));
        if constexpr(Kind==3)value=Loan(l,country,reinterpret_cast<uint8_t*>(c.r8),static_cast<int32_t>(c.r9));
        *reinterpret_cast<int64_t*>(c.rdx)=value.Clamp();c.rax=c.rdx;return true;
    }catch(...){return false;}
}
inline uint8_t* Relative(uint8_t* p,size_t offset){return locate::RelTarget(p+offset,p+offset+4);}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    const char* patterns[]={numericpatterns::kTechScore,numericpatterns::kEconomicScore,numericpatterns::kJobWeight,numericpatterns::kLoanWeight,numericpatterns::kTotalLoanWeight};
    numericpatch::Handler handlers[]={Hook<0>,Hook<1>,Hook<2>,Hook<3>,TotalLoansHook};
    for(size_t i=0;i<kBlockCount;++i){auto& b=l.blocks[i];b.at=locate::FindOne(image,"diplomatic score producer",patterns[i],error);
        if(!b.at)return false;b.stolen=b.span=i==0?11:5;b.returns=true;b.handler=handlers[i];b.data=&l;}
    l.blocks[0].ripOperand=7;
    auto tech=l.blocks[0].at,economy=l.blocks[1].at,job=l.blocks[2].at,loan=l.blocks[3].at;
    l.techDivisor=reinterpret_cast<int64_t*>(Relative(tech,7));l.techBase=reinterpret_cast<int64_t*>(Relative(tech,0x1c));
    l.repeatWeight=reinterpret_cast<int64_t*>(Relative(tech,0x32));l.technologyCount=locate::Disp32(tech+0x12);l.technologies=locate::Disp32(tech+0x3e);
    l.economicDivisor=reinterpret_cast<int64_t*>(Relative(economy,0x12));l.resources=reinterpret_cast<uint8_t**>(Relative(economy,0x1b));
    l.eligible=reinterpret_cast<Eligible>(Relative(economy,0x6f));l.income=locate::Disp32(economy+0x86);
    l.missingResource=reinterpret_cast<int64_t*>(Relative(economy,0x9e));
    l.jobPopCount=locate::Disp32(job+0x6a);l.jobPops=locate::Disp32(job+0x81);l.happiness=locate::Disp32(job+0x1fb);
    l.jobBase=reinterpret_cast<int64_t*>(Relative(job,0xb6));l.happinessFactor=reinterpret_cast<int64_t*>(Relative(job,0x177));
    l.hasHappiness=reinterpret_cast<Predicate>(Relative(job,0x168));l.favorFactor=reinterpret_cast<int64_t*>(Relative(loan,0x6e));
    l.nullModule=reinterpret_cast<uint8_t**>(Relative(loan,0x40));l.relation=reinterpret_cast<Relation>(Relative(loan,0x53));
    auto total=l.blocks[4].at;l.blocked=reinterpret_cast<Eligible>(Relative(total,0x2f));
    l.countryDb=reinterpret_cast<uint8_t**>(Relative(total,0x113));l.nullCountry=reinterpret_cast<uint8_t**>(Relative(total,0x11f));
    l.growth=reinterpret_cast<float*>(Relative(total,0x137));l.allocate=reinterpret_cast<Allocate>(Relative(total,0x376));
    if(Relative(total,0x1c1)!=reinterpret_cast<const uint8_t*>(l.favorFactor) ||
        Relative(total,0x3eb)!=reinterpret_cast<uint8_t*>(l.countryDb) || Relative(total,0x404)!=reinterpret_cast<uint8_t*>(l.nullCountry)){
        error="loan helpers disagree about factors or country references";return false;}
    return true;
}
} // namespace diploscores
