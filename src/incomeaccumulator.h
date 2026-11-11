// Wide economic accumulation with an explicit lifetime: one complete, joined budget pass.
// The owner registers already-sized persistent tables before dispatching workers and clears
// the registry only after all workers/reducers have returned. Nothing is retained by address
// between passes, so allocator reuse, table moves and country/category resizing cannot alias.
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <stdexcept>
#include <vector>
#include "numericmath.h"
namespace incomeaccumulator {
struct Range {int64_t* data;size_t count;};
class Pass {
    struct Region {uintptr_t begin,end;size_t offset;};
    std::vector<Region> regions;
    std::vector<numeric::Sum> values;
    std::array<SRWLOCK,256> locks{};
    std::atomic<bool> active{false};
    inline static std::atomic<uint64_t> nextGeneration{0};
    uint64_t generation=0; // Published with active, immutable while workers run.
    struct Cached {const Pass* owner=nullptr;uint64_t generation=0;const Region* region=nullptr;};
    inline static thread_local Cached cache;
    struct Guard {
        SRWLOCK* lock;explicit Guard(SRWLOCK* p):lock(p){AcquireSRWLockExclusive(lock);}
        ~Guard(){ReleaseSRWLockExclusive(lock);}
    };
    numeric::Sum* Find(int64_t* address){
        if(!active.load(std::memory_order_acquire))return nullptr;
        uintptr_t at=reinterpret_cast<uintptr_t>(address);const Region* r=nullptr;
        if(cache.owner==this && cache.generation==generation && cache.region && at>=cache.region->begin && at<cache.region->end)r=cache.region;
        else {
            auto it=std::upper_bound(regions.begin(),regions.end(),at,[](uintptr_t a,const Region& b){return a<b.begin;});
            if(it==regions.begin())return nullptr;--it;if(at>=it->end)return nullptr;r=&*it;
            cache={this,generation,r};
        }
        if((at-r->begin)%8)return nullptr;
        return &values[r->offset+(at-r->begin)/8];
    }
    SRWLOCK* Lock(int64_t* p){return &locks[(reinterpret_cast<uintptr_t>(p)>>3)&255];}
    static void Publish(int64_t* address,const numeric::Sum& value){InterlockedExchange64(reinterpret_cast<volatile LONG64*>(address),value.Clamp());}
public:
    bool Active()const{return active.load(std::memory_order_acquire);}
    void Begin(const std::vector<Range>& ranges){
        if(active.load())throw std::runtime_error("nested economic pass");
        std::vector<Region> next;size_t total=0;
        for(const auto& range:ranges){if(!range.count)continue;uintptr_t begin=reinterpret_cast<uintptr_t>(range.data);
            if(!begin || begin%8 || range.count>(UINTPTR_MAX-begin)/8 || range.count>SIZE_MAX-total)throw std::runtime_error("invalid economic table range");
            next.push_back({begin,begin+range.count*8,total});total+=range.count;}
        std::sort(next.begin(),next.end(),[](const Region& a,const Region& b){return a.begin<b.begin;});
        for(size_t i=1;i<next.size();++i)if(next[i].begin<next[i-1].end)throw std::runtime_error("overlapping economic tables");
        std::vector<numeric::Sum> sums(total);
        for(const auto& r:next)for(size_t at=r.begin;at<r.end;at+=8)sums[r.offset+(at-r.begin)/8].Add(*reinterpret_cast<int64_t*>(at));
        regions=std::move(next);values=std::move(sums);generation=nextGeneration.fetch_add(1,std::memory_order_relaxed)+1;active.store(true,std::memory_order_release);
    }
    // Caller must have joined every native worker before End; nonzero wide residuals are
    // reported so an incomplete drain is detectable instead of silently losing a carry.
    bool End(){
        active.store(false,std::memory_order_release);bool representable=true;
        for(const auto& sum:values){numeric::Sum raw;raw.Add(sum.Clamp());
            if(sum.low!=raw.low || sum.high!=raw.high)representable=false;}
        regions.clear();values.clear();return representable;
    }
    bool Add(int64_t* address,int64_t delta,int64_t& old){auto p=Find(address);if(!p)return false;
        Guard guard(Lock(address));old=p->Clamp();p->Add(delta);Publish(address,*p);return true;}
    bool Add(int64_t* address,const numeric::Sum& delta,int64_t& old){auto p=Find(address);if(!p)return false;
        Guard guard(Lock(address));old=p->Clamp();p->Merge(delta);Publish(address,*p);return true;}
    bool Set(int64_t* address,int64_t value){auto p=Find(address);if(!p)return false;
        Guard guard(Lock(address));*p=numeric::Sum{};p->Add(value);Publish(address,*p);return true;}
    bool Read(int64_t* address,numeric::Sum& value){auto p=Find(address);if(!p)return false;
        Guard guard(Lock(address));value=*p;return true;}
    bool Take(int64_t* address,numeric::Sum& value){auto p=Find(address);if(!p)return false;
        Guard guard(Lock(address));value=*p;*p=numeric::Sum{};Publish(address,*p);return true;}
    bool CompareSubtract(int64_t* address,int64_t expected,int64_t amount,int64_t& old,bool& exchanged){
        auto p=Find(address);if(!p)return false;Guard guard(Lock(address));old=p->Clamp();exchanged=old==expected;
        if(exchanged){p->Subtract(amount);Publish(address,*p);}return true;
    }
};
} // namespace incomeaccumulator
