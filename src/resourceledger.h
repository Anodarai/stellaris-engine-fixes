// Stockpile arithmetic, before the engine's ordinary capacity/situation handling.
#pragma once
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include "locate.h"
#include "numericmath.h"
#include "resourceledger_patterns.h"
#include "resourcebatch_patterns.h"
#include "numericpatch.h"

namespace resourceledger {
using ChangeFn = void(__fastcall*)(uint8_t*, uint8_t*, int64_t);
using ConvertFn = void(__fastcall*)(uint8_t*, void*, uint8_t*, int64_t);
using ClampFn = void(__fastcall*)(uint8_t*, void*);
// The optional fifth argument points to a table of pending situation updates.
using SituationFn = void(__fastcall*)(uint8_t*, uint8_t*, uint8_t*, int64_t, void*);
constexpr size_t kStolen = 15;
struct Locations {
    uint8_t *add = nullptr, *remove = nullptr, *convert = nullptr;
    uint8_t** resources = nullptr;
    void* allocatorVtable = nullptr;
    ClampFn clamp = nullptr;
    SituationFn situation = nullptr;
    size_t resourceIndex = 0, converter = 0, country = 0;
    size_t conversions = 0, conversionCount = 0;
    size_t recordSize = 0, recordFlags = 0, pairs = 0, pairCount = 0;
    uint8_t** nullResource = nullptr;
    numericpatch::Block batches[2];
    // Set by a fault inside Batch: from then on the batch hooks only check slots (CheckBatch).
    std::atomic<bool> batchChecksOnly{false};
};
inline bool BatchHook(numericpatch::Context&,const void*);
inline bool RemoveBatchHook(numericpatch::Context&,const void*);
inline uint8_t* Relative(uint8_t* p, size_t operand) { return locate::RelTarget(p+operand,p+operand+4); }
inline bool Locate(const locate::Image& image, Locations& loc, std::string& error) {
    using namespace numericpatterns;
    loc.add = locate::FindOne(image,"stockpile AddResource",kAdd,error);
    if (!loc.add) return false;
    loc.remove = locate::FindOne(image,"stockpile RemoveResource",kRemove,error);
    if (!loc.remove) return false;
    loc.convert = locate::FindOne(image,"resource converter",kConvert,error);
    if (!loc.convert) return false;
    uint8_t* a = loc.add; uint8_t* r = loc.remove;
    if (Relative(a,0xe2) != loc.convert || Relative(r,0xda) != loc.convert ||
        Relative(a,0x170) != Relative(r,0x111) || Relative(a,0x12b) != Relative(r,0xf4) ||
        Relative(a,0x27) != Relative(r,0x27) || Relative(a,0x8f) != Relative(r,0x8f)) {
        error = "stockpile call/global cross-check failed"; return false;
    }
    loc.resources = reinterpret_cast<uint8_t**>(Relative(a,0x27));
    loc.allocatorVtable = Relative(a,0x8f);
    loc.clamp = reinterpret_cast<ClampFn>(Relative(a,0x170));
    loc.situation = reinterpret_cast<SituationFn>(Relative(a,0x12b));
    loc.resourceIndex = a[0xeb]; loc.converter = a[0xbe]; loc.country = a[0x126];
    loc.conversions = loc.convert[0x66]; loc.conversionCount = loc.convert[0x58];
    loc.recordSize = loc.convert[0x62]; loc.recordFlags = loc.convert[0x6f];
    loc.pairs = loc.convert[0x91]; loc.pairCount = loc.convert[0x95];
    if (loc.resourceIndex != loc.convert[0xb] || a[0xeb] >= 128 ||
        loc.conversions >= 128 || loc.conversionCount >= 128) {
        error = "resource/converter layout disagreement"; return false;
    }
    const char* batchPatterns[]={kAddBatch,kRemoveBatch};
    for(int i=0;i<2;++i){auto& b=loc.batches[i];b.at=locate::FindOne(image,"batch stockpile",batchPatterns[i],error);
        if(!b.at)return false;b.stolen=b.span=5;b.handler=i?RemoveBatchHook:BatchHook;b.data=&loc;b.returns=true;}
    // Both batch prologues begin with one complete five-byte home-slot store.
    auto batch=loc.batches[0].at;
    loc.nullResource=reinterpret_cast<uint8_t**>(Relative(batch,0x143));
    return true;
}

struct Array { int64_t* values; int32_t capacity, count; };
struct Allocator { void* vtable; int64_t* values; int32_t count; int32_t padding; };
struct Table {
    Array* array;
    Array storage;
    Allocator* allocator;
    Allocator adapter;
    Table(int64_t* values, int count, void* vt) : array(&storage), storage{values,count,count},
        allocator(&adapter), adapter{vt,values,count,0} {}
};
static_assert(offsetof(Table,adapter) == 0x20 && offsetof(Table,storage) == 8, "game stack table ABI");
inline int Index(const Locations& loc, const uint8_t* resource) {
    return *reinterpret_cast<const int*>(resource+loc.resourceIndex);
}
inline int64_t* Values(void* table) { return **reinterpret_cast<int64_t***>(table); }
inline int Count(void* table) { return (*reinterpret_cast<Array**>(table))->count; }
inline int StockpileCount(const Locations& loc) { return *reinterpret_cast<const int*>(*loc.resources+0x14); }
// The native code guards its reads of a resource's slot but not its writes: the converter's
// negative-amount path, AddResource and the batches write straight through an out-of-range slot.
// The hooks never hand such a resource to the game; its change is skipped and counted here.
// After a fault inside a batch, a batch with such an entry is skipped whole and counted in
// skippedBatches as well.
inline std::atomic<uint64_t> skipped{0}, skippedBatches{0};
inline bool InTable(const Locations& loc, const uint8_t* resource, int count) {
    int index = Index(loc,resource);
    return index >= 0 && index < count;
}
inline void RequireInTable(const Locations& loc, const uint8_t* resource, int count) {
    if (!InTable(loc,resource,count)) throw std::out_of_range("stockpile resource outside its table");
}
inline void SubtractMagnitude(int64_t& balance, uint64_t amount) {
    numeric::Sum total; total.Add(balance);
    // The only unsigned input above INT64_MAX is the magnitude of INT64_MIN.
    if (amount > static_cast<uint64_t>(INT64_MAX)) { total.Subtract(INT64_MAX); total.Subtract(1); }
    else total.Subtract(static_cast<int64_t>(amount));
    balance = total.Clamp();
}

// The engine's large-number decimal division intentionally truncates its separated terms.
// Retain that rounding (including its coarse large-denominator branch) without overflow.
inline int64_t ConvertedAvailable(int64_t value, int64_t rate) {
    if (value >= -92233720368547LL && value <= 92233720368547LL)
        return numeric::MulDiv(value,100000,rate);
    if (rate < 10000000000LL) {
        int64_t whole = (value / 100000) * 100000;
        numeric::Sum sum;
        // Each term is representable unless the final available amount is already enormous.
        sum.Add(numeric::MulDiv(whole / rate,100000,1));
        sum.Add(numeric::MulDiv(value % 100000,100000,rate));
        sum.Add(numeric::MulDiv(whole % rate,100000,rate));
        return sum.Clamp();
    }
    return value / (rate / 100000);
}

inline void ConvertMagnitude(const Locations& loc, uint8_t* converter, void* table,
                             uint8_t* resource, uint64_t remaining) {
    int index = Index(loc,resource), count = Count(table);
    RequireInTable(loc,resource,count);
    int64_t* values = Values(table);
    int64_t balance = values[index];
    if (balance >= 0 && remaining <= static_cast<uint64_t>(balance)) {
        SubtractMagnitude(values[index],remaining); return;
    }
    int records = *reinterpret_cast<int*>(converter+loc.conversionCount);
    auto data = *reinterpret_cast<uint8_t**>(converter+loc.conversions);
    auto record = index >= 0 && index < records ? data + size_t(index)*loc.recordSize : nullptr;
    if (record && (record[loc.recordFlags]&1)) {
        if (balance > 0) { values[index] = 0; remaining -= static_cast<uint64_t>(balance); }
        auto pairs = *reinterpret_cast<uint8_t**>(record+loc.pairs);
        int pairCount = *reinterpret_cast<int*>(record+loc.pairCount);
        for (int n=0;n<pairCount && remaining;++n) {
            uint8_t* other = *reinterpret_cast<uint8_t**>(pairs+size_t(n)*16);
            if (other == resource) continue;
            int otherIndex = Index(loc,other);
            int64_t rate = *reinterpret_cast<int64_t*>(pairs+size_t(n)*16+8);
            if (rate <= 0) rate = 100000;
            int64_t available = ConvertedAvailable(otherIndex >= 0 && otherIndex < count ? values[otherIndex] : 0,rate);
            if (available <= 0) continue;
            int64_t used = static_cast<uint64_t>(available) < remaining ? available : static_cast<int64_t>(remaining);
            values[otherIndex] = numeric::Subtract(values[otherIndex],numeric::MulDiv(used,rate,100000));
            remaining -= static_cast<uint64_t>(used);
        }
    }
    SubtractMagnitude(values[index],remaining);
}
inline void Convert(const Locations& loc, uint8_t* converter, void* table, uint8_t* resource, int64_t amount) {
    if (amount < 0) {
        RequireInTable(loc,resource,Count(table));
        int64_t& balance = Values(table)[Index(loc,resource)];
        balance = numeric::Subtract(balance,amount);
    } else ConvertMagnitude(loc,converter,table,resource,static_cast<uint64_t>(amount));
}
struct ChangeProgress {
    // The exception filter reads these even if a native call does not return.
    const char* volatile stage = "enter hook";
    volatile bool effectsStarted = false;
    bool skipped = false;  // the resource's slot is outside the table: nothing was changed
};

inline void Change(const Locations& loc, uint8_t* economy, uint8_t* resource, int64_t amount,
                   bool remove, ChangeProgress& progress) {
    progress.stage = "prepare stockpile table";
    int count = StockpileCount(loc);
    if (!InTable(loc,resource,count)) {
        progress.stage = "resource outside the stockpile table; skipped";
        progress.skipped = true;
        skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::vector<int64_t> values(count);
    Table table(values.data(),count,loc.allocatorVtable);
    auto vtable = *reinterpret_cast<uint8_t***>(economy);
    using TableFn = void(__fastcall*)(uint8_t*,void*,bool);
    progress.stage = "read stockpile";
    reinterpret_cast<TableFn>(vtable[0x90/8])(economy,&table,false);
    progress.stage = "check converter";
    uint8_t* converter = *reinterpret_cast<uint8_t**>(economy+loc.converter);
    using ValidFn = bool(__fastcall*)(uint8_t*);
    auto valid = reinterpret_cast<ValidFn>((*reinterpret_cast<void***>(converter))[1]);
    bool conversion = valid(converter);
    progress.stage = "calculate balance";
    int index = Index(loc,resource);
    if (conversion) {
        if (remove) Convert(loc,converter,&table,resource,amount);
        else if (amount < 0) ConvertMagnitude(loc,converter,&table,resource,numeric::Magnitude(amount));
        else values[index] = numeric::Add(values[index],amount);
        progress.stage = "update situations";
        progress.effectsStarted = true;
        int64_t effectAmount = remove ? numeric::Negate(amount) : (amount/100000)*100000;
        loc.situation(converter,*reinterpret_cast<uint8_t**>(economy+loc.country),resource,effectAmount,nullptr);
    } else {
        values[index] = remove ? numeric::Subtract(values[index],amount) :
            numeric::Add(values[index],(amount/100000)*100000);
    }
    progress.stage = "clamp stockpile";
    progress.effectsStarted = true;
    loc.clamp(economy,&table);
    progress.stage = "write stockpile";
    reinterpret_cast<TableFn>(vtable[0x98/8])(economy,&table,false);
}
inline uint8_t* Resource(const Locations& loc,int index){
    auto resources=*loc.resources;
    return index>=0 && index<*reinterpret_cast<int*>(resources+0x14)
        ? (*reinterpret_cast<uint8_t***>(resources+8))[index] : *loc.nullResource;
}
// An input entry without a slot in the stockpile table (past the input's end, or a resource whose
// slot is out of range) is skipped and counted, with its situation update; the rest go through.
inline void Batch(const Locations& loc,uint8_t* economy,void* input,bool remove,int writeFlag,void* situationUpdates,bool& effects){
    int count=StockpileCount(loc);
    std::vector<int64_t> values(count);Table table(values.data(),count,loc.allocatorVtable);
    using TableFn=void(__fastcall*)(uint8_t*,void*,int);
    auto vt=*reinterpret_cast<void***>(economy);
    reinterpret_cast<TableFn>(vt[0x90/8])(economy,&table,0);
    auto converter=*reinterpret_cast<uint8_t**>(economy+loc.converter);
    using Predicate=bool(__fastcall*)(uint8_t*);
    bool valid=reinterpret_cast<Predicate>((*reinterpret_cast<void***>(converter))[1])(converter);
    int inputs=Count(input);
    if(!valid){int n=(std::min)(count,inputs);
        if(n<count)skipped.fetch_add(uint64_t(count-n),std::memory_order_relaxed);
        for(int i=0;i<n;++i)values[i]=remove?numeric::Subtract(values[i],Values(input)[i]):numeric::Add(values[i],Values(input)[i]);}
    else for(int i=0;i<inputs;++i){int64_t amount=Values(input)[i];auto resource=Resource(loc,i);
        if(!InTable(loc,resource,count)){skipped.fetch_add(1,std::memory_order_relaxed);continue;}
        auto available=reinterpret_cast<Predicate>((*reinterpret_cast<void***>(resource))[7]);
        if(available(resource)){
            bool subtract=remove ? amount>=0 : amount<0;
            if(subtract)ConvertMagnitude(loc,converter,&table,resource,numeric::Magnitude(amount));
            else {
                numeric::Sum sum;sum.Add(values[Index(loc,resource)]);
                if(remove)sum.Subtract(amount);else sum.Add(amount);
                values[Index(loc,resource)]=sum.Clamp();
            }
        }
        // Retain the native second predicate evaluation before situation handling.
        resource=Resource(loc,i);available=reinterpret_cast<Predicate>((*reinterpret_cast<void***>(resource))[7]);
        if(available(resource)){effects=true;loc.situation(converter,*reinterpret_cast<uint8_t**>(economy+loc.country),resource,
            remove?numeric::Negate(amount):amount,remove?nullptr:situationUpdates);}
    }
    effects=true;loc.clamp(economy,&table);reinterpret_cast<TableFn>(vt[0x98/8])(economy,&table,remove?0:writeFlag);
}
// How many entries of a batch the game's own code would take outside the stockpile table, counted
// as Batch skips them: with the converter valid it writes through each input entry's resource
// slot; without, it reads the input up to the table's count. The converter is asked only when its
// answer decides the count.
inline int OutsideTable(const Locations& loc,uint8_t* economy,void* input){
    int count=StockpileCount(loc),inputs=Count(input),outside=0;
    for(int i=0;i<inputs;++i)if(!InTable(loc,Resource(loc,i),count))++outside;
    int missing=inputs<count?count-inputs:0;
    if(!outside && !missing)return 0;
    auto converter=*reinterpret_cast<uint8_t**>(economy+loc.converter);
    using Predicate=bool(__fastcall*)(uint8_t*);
    return reinterpret_cast<Predicate>((*reinterpret_cast<void***>(converter))[1])(converter)?outside:missing;
}
// After a fault, the game's own code runs a batch only when every entry has a slot in the table
// (false: run it). Otherwise, or if the check itself faults, the whole batch is skipped (true).
inline bool CheckBatch(const Locations& loc,uint8_t* economy,void* input){
    int outside=-1;
    __try{outside=OutsideTable(loc,economy,input);}__except(EXCEPTION_EXECUTE_HANDLER){}
    if(!outside)return false;
    skipped.fetch_add(outside>0?uint64_t(outside):1,std::memory_order_relaxed);
    skippedBatches.fetch_add(1,std::memory_order_relaxed);
    return true;
}
inline bool InvokeBatch(numericpatch::Context& c,const void* data,bool remove){
    auto& l=*const_cast<Locations*>(static_cast<const Locations*>(data));
    auto economy=reinterpret_cast<uint8_t*>(c.rcx);auto input=reinterpret_cast<void*>(c.rdx);
    if(!l.batchChecksOnly.load(std::memory_order_relaxed)){
        bool effects=false;
        __try{Batch(l,economy,input,remove,static_cast<int>(c.r8),reinterpret_cast<void*>(c.r9),effects);return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){
            l.batchChecksOnly.store(true,std::memory_order_relaxed);
            if(effects)return true;  // part of the batch is done: the game must not repeat it
        }
    }
    return CheckBatch(l,economy,input);
}
inline bool BatchHook(numericpatch::Context& c,const void* data){return InvokeBatch(c,data,false);}
inline bool RemoveBatchHook(numericpatch::Context& c,const void* data){
    return InvokeBatch(c,data,true);
}
} // namespace resourceledger
