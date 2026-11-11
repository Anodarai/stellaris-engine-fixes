// Native atomic table boundaries. The pass registry is populated only after the
// engine has resized every table and before its graph dispatch starts workers.
#pragma once
#include <map>
#include "incomeaccumulator.h"
#include "numericpatch.h"
#include "atomiclifetime_patterns.h"
#include "atomicread_patterns.h"
namespace incomeatomic {
template<class T>T Read(const uint8_t* p,size_t offset){return *reinterpret_cast<const T*>(p+offset);}
struct Locations {
    uint8_t* roots[2]{};uint8_t* getters[2]{};uint8_t* tables[2]{};uint8_t** jobs[2]{};
    uint8_t* graph=nullptr;uint8_t* add=nullptr;uint8_t* drain=nullptr;uint8_t* categorizedDrain=nullptr;uint8_t* store=nullptr;
    uint8_t** resources=nullptr;uint8_t** nullResource=nullptr;
    uint8_t* readResource=nullptr;
    uint8_t** categoryManager=nullptr;uint8_t** nullCategory=nullptr;
};
inline void TableRanges(uint8_t* table,std::vector<incomeaccumulator::Range>& ranges){
    int count=Read<int>(table,0x14);if(count<0)throw std::runtime_error("invalid atomic resource count");
    if(count)ranges.push_back({Read<int64_t*>(table,8),size_t(count)});
}
inline std::vector<incomeaccumulator::Range> Ranges(const Locations& l){
    std::vector<incomeaccumulator::Range> ranges;
    for(auto array:l.tables){int count=Read<int>(array,0x14);if(count<0)throw std::runtime_error("invalid economic country count");
        auto data=Read<uint8_t*>(array,8);
        for(int i=0;i<count;++i){auto country=data+size_t(i)*0x30;TableRanges(country,ranges);
            int categories=Read<int>(country,0x2c);if(categories<0)throw std::runtime_error("invalid economic category count");
            auto category=Read<uint8_t*>(country,0x20);for(int j=0;j<categories;++j)TableRanges(category+size_t(j)*0x18,ranges);}
    }
    return ranges;
}
struct DrainEntry {int64_t* source;int64_t* destination;};
inline void PlanDrain(const Locations& l,uint8_t* source,uint8_t* destination,std::vector<DrainEntry>& plan){
    int count=Read<int>(source,0x14);if(count<0)throw std::runtime_error("invalid atomic drain count");
    auto input=Read<int64_t*>(source,8);auto array=Read<uint8_t*>(destination,0);auto output=Read<int64_t*>(array,0);int outputCount=Read<int>(array,12);
    auto manager=*l.resources;auto resourceData=Read<uint8_t**>(manager,8);int resourceCount=Read<int>(manager,0x14);
    for(int i=0;i<count;++i){auto resource=i<resourceCount?resourceData[i]:*l.nullResource;int index=Read<int>(resource,0x18);
        if(index<0 || index>=outputCount)throw std::runtime_error("invalid atomic drain resource index");
        plan.push_back({input+i,output+index});}
}
inline std::vector<DrainEntry> Plan(const Locations& l,uint8_t* source,uint8_t* destination,bool categorized){
    std::vector<DrainEntry> result;
    if(categorized){int count=Read<int>(source,0x2c);if(count<0)throw std::runtime_error("invalid atomic category drain count");
        auto input=Read<uint8_t*>(source,0x20);auto output=Read<uint8_t*>(destination,0x28);
        for(int i=0;i<count;++i)PlanDrain(l,input+size_t(i)*0x18,output+size_t(i)*0x20,result);}
    PlanDrain(l,source,destination,result);return result;
}
inline void Drain(incomeaccumulator::Pass& pass,const std::vector<DrainEntry>& plan,bool& writing){
    // Allocate and validate all destinations before resetting any source. In the
    // native ownership model source atomic arrays and destination ordinary arrays
    // are disjoint; reject an invalid alias before crossing the mutation boundary.
    std::map<int64_t*,numeric::Sum> totals;
    for(const auto& item:plan)totals.try_emplace(item.destination);
    for(const auto& item:plan)if(totals.find(item.source)!=totals.end())throw std::runtime_error("aliased atomic drain buffers");
    for(auto& item:totals)item.second.Add(*item.first);
    writing=true;
    for(const auto& item:plan){numeric::Sum value;
        if(!pass.Take(item.source,value))value.Add(InterlockedExchange64(reinterpret_cast<volatile LONG64*>(item.source),0));
        totals.find(item.destination)->second.Merge(value);}
    for(const auto& item:totals)*item.first=item.second.Clamp();
}
inline numeric::Sum ReadValue(incomeaccumulator::Pass& pass,uint8_t* table,int index){
    int count=Read<int>(table,0x14);if(index<0 || index>=count)throw std::runtime_error("invalid atomic resource read index");
    auto address=Read<int64_t*>(table,8)+index;numeric::Sum value;
    if(!pass.Read(address,value))value.Add(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(address),0,0));
    return value;
}
inline numeric::Sum ReadResource(incomeaccumulator::Pass& pass,uint8_t* table,uint8_t* resource){
    int index=Read<int>(resource,0x18);auto value=ReadValue(pass,table,index);
    int count=Read<int>(table,0x2c);if(count<0)throw std::runtime_error("invalid atomic category read count");
    auto data=Read<uint8_t*>(table,0x20);for(int i=0;i<count;++i)value.Merge(ReadValue(pass,data+size_t(i)*0x18,index));
    return value;
}
inline numeric::Sum Category(incomeaccumulator::Pass& pass,uint8_t* table,int index,uint8_t* category,size_t depth=0){
    int count=Read<int>(table,0x2c),id=Read<int>(category,8);
    if(count<0 || id<0 || id>=count || depth>size_t(count))throw std::runtime_error("invalid economic category tree");
    auto value=ReadValue(pass,Read<uint8_t*>(table,0x20)+size_t(id)*0x18,index);
    int children=Read<int>(category,0x8c);if(children<0)throw std::runtime_error("invalid category child count");
    auto data=Read<uint8_t**>(category,0x80);
    for(int i=0;i<children;++i)value.Merge(Category(pass,table,index,data[i],depth+1));return value;
}
inline numeric::Sum VisibleCategory(incomeaccumulator::Pass& pass,uint8_t* table,int index,uint8_t* category,int ordinal,uint8_t level){
    int count=Read<int>(table,0x2c);if(ordinal<0 || ordinal>=count)throw std::runtime_error("invalid visible category index");
    auto value=ReadValue(pass,Read<uint8_t*>(table,0x20)+size_t(ordinal)*0x18,index);
    int children=Read<int>(category,0x8c);if(children<0)throw std::runtime_error("invalid visible category children");
    auto data=Read<uint8_t**>(category,0x80);
    for(int i=0;i<children;++i)if(Read<uint8_t>(data[i],0x134)>level)value.Merge(Category(pass,table,index,data[i]));
    return value;
}
inline numeric::Sum FormattedResource(const Locations& l,incomeaccumulator::Pass& pass,uint8_t* table,uint8_t* resource,uint8_t* tooltip){
    int index=Read<int>(resource,0x18);auto total=ReadValue(pass,table,index);int count=Read<int>(table,0x2c);
    if(count<0)throw std::runtime_error("invalid formatted category count");
    auto manager=*l.categoryManager;auto categories=Read<uint8_t**>(manager,0x50);int available=Read<int>(manager,0x5c);
    uint8_t level=Read<uint8_t>(tooltip,0x21);
    for(int i=0;i<count;++i){auto category=i<available?categories[i]:*l.nullCategory;
        if(Read<uint8_t>(category,0x134)<=level)total.Merge(VisibleCategory(pass,table,index,category,i,level));}
    return total;
}
inline uint8_t* Relative(uint8_t* p,size_t offset){return locate::RelTarget(p+offset,p+offset+4);}
inline bool Locate(const locate::Image& image,Locations& l,std::string& error){
    l.roots[0]=locate::FindOne(image,"income table pass",numericpatterns::kIncomePassTables,error);
    l.roots[1]=locate::FindOne(image,"income budget pass",numericpatterns::kIncomePassBudget,error);
    l.graph=locate::FindOne(image,"joined economic graph",numericpatterns::kIncomeExecuteGraph,error);
    l.add=locate::FindOne(image,"atomic resource table add",numericpatterns::kAtomicAddTable,error);
    l.drain=locate::FindOne(image,"atomic resource table drain",numericpatterns::kAtomicDrain,error);
    l.categorizedDrain=locate::FindOne(image,"atomic categorized drain",numericpatterns::kAtomicCategorizedDrain,error);
    l.store=locate::FindOne(image,"atomic resource store",numericpatterns::kAtomicStore,error);
    l.readResource=locate::FindOne(image,"atomic resource read",numericpatterns::kAtomicReadResource,error);
    if(!l.roots[0] || !l.roots[1] || !l.graph || !l.add || !l.drain || !l.categorizedDrain || !l.store || !l.readResource)return false;
    l.categoryManager=reinterpret_cast<uint8_t**>(Relative(l.readResource,0x353));l.nullCategory=reinterpret_cast<uint8_t**>(Relative(l.readResource,0x369));
    l.getters[0]=Relative(l.roots[0],0x35);l.getters[1]=Relative(l.roots[0],0x45);
    for(int i=0;i<2;++i){if(!locate::MatchAt(l.getters[i],locate::Parse(numericpatterns::kIncomeTablesGetter))){error="economic table getter cross-check failed";return false;}
        l.tables[i]=Relative(l.getters[i],0x23);if(Relative(l.getters[i],0x2f)!=l.tables[i]){error="economic array getter globals disagree";return false;}}
    l.jobs[0]=reinterpret_cast<uint8_t**>(Relative(l.roots[0],0xe1));l.jobs[1]=reinterpret_cast<uint8_t**>(Relative(l.roots[1],0xdf));
    if(Relative(l.roots[1],0x3b)!=l.getters[0] || Relative(l.roots[1],0x47)!=l.getters[1] ||
       Relative(l.roots[0],0xe6)!=l.graph || Relative(l.roots[1],0xe4)!=l.graph){error="economic pass graph/getter cross-check failed";return false;}
    l.resources=reinterpret_cast<uint8_t**>(Relative(l.drain,0x23));l.nullResource=reinterpret_cast<uint8_t**>(Relative(l.drain,0x3a));
    if(Relative(l.categorizedDrain,0x63)!=reinterpret_cast<uint8_t*>(l.resources) || Relative(l.categorizedDrain,0x7a)!=reinterpret_cast<uint8_t*>(l.nullResource)){
        error="economic drain resource globals disagree";return false;}
    return true;
}
} // namespace incomeatomic
