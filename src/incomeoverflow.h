#pragma once
#include "locate.h"
#include "incomescaling.h"
#include "incomescaling_patterns.h"
namespace incomeoverflow {
struct Array {int64_t* data;int32_t capacity,count;};
struct Table {Array* array;Array storage;void* allocator;};
struct PlanList {void* vtable=nullptr;uint8_t** data=nullptr;int32_t capacity=0,count=0;uint8_t* local[8]{};};
static_assert(sizeof(Table)==32 && offsetof(PlanList,local)==24,"native table/list ABI");
using CalculateFn=Table*(__fastcall*)(uint8_t*,Table*,void*,const Table*,int);
using ListFn=PlanList*(__fastcall*)(uint8_t*,PlanList*,void*);
using FreeFn=void(__fastcall*)(void*);
using AssignFn=void(__fastcall*)(Array*,int,const int64_t*,int);
constexpr size_t kStolen=15;
struct Locations {uint8_t* calculate=nullptr;ListFn lists[4]{};FreeFn free=nullptr;AssignFn assign=nullptr;
    void* allocator=nullptr;size_t baseData=0,baseCount=0;};
inline uint8_t* Relative(uint8_t* p,size_t offset){return locate::RelTarget(p+offset,p+offset+4);}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    auto p=locate::FindOne(image,"complete income target",numericpatterns::kIncomeScaling,error);
    auto scaling=locate::FindOne(image,"income scaling subplans",numericpatterns::kScalingSubplans,error);
    if(!p || !scaling)return false;l.calculate=p;
    const size_t operands[]={0x7e,0x1b5,0x2b0,0x382};
    for(int i=0;i<4;++i)l.lists[i]=reinterpret_cast<ListFn>(Relative(p,operands[i]));
    if(reinterpret_cast<uint8_t*>(l.lists[1])!=scaling || Relative(p,0xb3)!=p || Relative(p,0x1e6)!=p ||
        Relative(p,0x2e3)!=p || Relative(p,0x3c3)!=p){error="income target recursion/list cross-check failed";return false;}
    l.baseData=p[0x64];l.baseCount=p[0x60];l.allocator=Relative(p,0x55);
    l.assign=reinterpret_cast<AssignFn>(Relative(p,0x68));l.free=reinterpret_cast<FreeFn>(Relative(p,0x133));
    if(Relative(p,0x294)!=reinterpret_cast<uint8_t*>(l.free) || Relative(p,0x369)!=reinterpret_cast<uint8_t*>(l.free) ||
        Relative(p,0x520)!=reinterpret_cast<uint8_t*>(l.free)){error="income plan-list cleanup disagreement";return false;}
    return true;
}
struct ScopedList {
    PlanList value;const Locations& loc;
    ScopedList(const Locations& l,int kind,uint8_t* plan,void* scope):loc(l){l.lists[kind](plan,&value,scope);}
    ~ScopedList(){if(value.data && value.data!=value.local)loc.free(value.data);}
    ScopedList(const ScopedList&)=delete;ScopedList& operator=(const ScopedList&)=delete;
};
inline std::vector<int64_t> Evaluate(const Locations& l,uint8_t* plan,void* scope,const std::vector<int64_t>* target,int mode){
    int count=*reinterpret_cast<int*>(plan+l.baseCount);
    if(count<0 || (target && target->size()!=static_cast<size_t>(count)))throw std::runtime_error("inconsistent income tables");
    auto initial=*reinterpret_cast<int64_t**>(plan+l.baseData);incomescaling::Values total;
    total.reserve(count);for(int i=0;i<count;++i)total.emplace_back(initial[i]);
    auto addChildren=[&](int kind,const numeric::Integer& factor){
        ScopedList list(l,kind,plan,scope);if(list.value.count<0)throw std::runtime_error("invalid subplan list");
        for(int n=0;n<list.value.count;++n){auto child=Evaluate(l,list.value.data[n],scope,target,mode);
            if(child.size()!=total.size())throw std::runtime_error("inconsistent subplan table");
            for(size_t i=0;i<total.size();++i)total[i]+=numeric::Integer(child[i])*factor;}
    };
    addChildren(0,numeric::Integer(1));numeric::Integer rounds;
    if(mode==0 && target && !target->empty() && !incomescaling::Reached(total,*target)){
        incomescaling::Values delta(total.size());ScopedList list(l,1,plan,scope);
        if(list.value.count<0)throw std::runtime_error("invalid scaling subplan list");
        for(int n=0;n<list.value.count;++n){auto child=Evaluate(l,list.value.data[n],scope,target,0);
            if(child.size()!=total.size())throw std::runtime_error("inconsistent scaling table");
            if(std::any_of(child.begin(),child.end(),[](int64_t v){return v>0;}))
                for(size_t i=0;i<delta.size();++i)delta[i]+=numeric::Integer(child[i]);
        }
        rounds=incomescaling::Scale(total,delta,*target);
    }
    addChildren(2,numeric::Integer(1));
    if(mode==0)addChildren(3,rounds);
    std::vector<int64_t> result;result.reserve(total.size());for(const auto& value:total)result.push_back(value.Clamp());return result;
}
inline void Calculate(const Locations& l,uint8_t* plan,Table* out,void* scope,const Table* target,int mode,bool& writing){
    std::vector<int64_t> targets;
    if(target){if(target->array->count<0)throw std::runtime_error("invalid target table");
        if(target->array->count)targets.assign(target->array->data,target->array->data+target->array->count);}
    auto values=Evaluate(l,plan,scope,target?&targets:nullptr,mode);
    writing=true;out->array=&out->storage;out->storage={nullptr,0,0};out->allocator=l.allocator;
    l.assign(&out->storage,0,values.data(),static_cast<int>(values.size()));
}
} // namespace incomeoverflow
