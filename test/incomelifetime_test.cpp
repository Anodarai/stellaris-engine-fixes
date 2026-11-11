#include <array>
#include <cstdio>
#include <random>
#include "../src/incomelifetime.h"
namespace {
int failures=0;void Check(bool ok,const char* label){if(!ok && ++failures<20)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t offset,T value){memcpy(p+offset,&value,sizeof(value));}
struct Atomic{void* vtable=nullptr;int64_t* data=nullptr;int capacity=0,count=0;};
struct Categorized{Atomic total;void* vtable=nullptr;Atomic* categories=nullptr;int capacity=0,count=0;};
struct Countries{void* vtable=nullptr;Categorized* data=nullptr;int capacity=0,count=0;};
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    incomelifetime::Locations l;std::string error;if(!incomelifetime::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,32768,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    auto category=l.boundaries[5].at;memcpy(code,category,numericpatterns::kAtomicCategoryReadSize);
    for(size_t off=0;off+4<=numericpatterns::kAtomicCategoryReadSize;++off)if(incomeatomic::Relative(category,off)==category){int32_t rel=int32_t(-intptr_t(off)-4);Set(code,off,rel);}
    memcpy(code+4096,l.tables.readResource,numericpatterns::kAtomicReadResourceSize);
    // The complete native store switch is copied, including its image-relative table.
    auto store=l.tables.store;memcpy(code+8192,store,numericpatterns::kAtomicStoreSize);
    uint32_t tableRva=incomeatomic::Read<uint32_t>(store,0x14);auto sourceTable=image.base+tableRva;
    Set(code+8192,0xc,int32_t(-0x10));Set(code+8192,0x14,uint32_t(512));
    for(size_t i=0;i<6;++i)Set(code+8192,512+i*4,uint32_t(incomeatomic::Read<uint32_t>(sourceTable,i*4)-uint32_t(store-image.base)));
    auto& storeBlock=l.boundaries[2];storeBlock.at=code+8192;std::vector<uint8_t> jump;
    Check(numericpatch::Prepare(storeBlock,code+12288,1024,jump),"prepare native switch with displaced short branch");memcpy(storeBlock.at,jump.data(),jump.size());
    std::array<std::array<int64_t,2>,4> values{};Atomic categories[3];
    for(int i=0;i<3;++i)categories[i]={nullptr,values[i+1].data(),2,2};
    Categorized table{{nullptr,values[0].data(),2,2},nullptr,categories,3,3};
    std::array<std::array<uint8_t,0x140>,3> definitions{};uint8_t* children[2]={definitions[1].data(),definitions[2].data()};
    uint8_t* definitionsList[3]={definitions[0].data(),definitions[1].data(),definitions[2].data()};
    for(int i=0;i<3;++i){Set(definitions[i].data(),8,i);definitions[i][0x134]=i?1:0;}
    Set(definitions[0].data(),0x80,children);Set(definitions[0].data(),0x8c,2);
    std::array<uint8_t,0x20> resource{};Set(resource.data(),0x18,1);
    std::array<uint8_t,0x60> manager{};Set(manager.data(),0x50,definitionsList);Set(manager.data(),0x5c,3);auto managerPtr=manager.data(),nullCategory=definitions[0].data();
    l.tables.categoryManager=&managerPtr;l.tables.nullCategory=&nullCategory;
    using CategoryFn=int64_t*(__fastcall*)(uint8_t*,int64_t*,uint8_t*,uint8_t*);
    using StoreFn=void(__fastcall*)(int64_t*,int64_t,int);
    auto object=reinterpret_cast<uint8_t*>(&table);std::mt19937_64 rng(88117);
    for(int n=0;n<20000;++n){for(auto& row:values)for(auto& v:row)v=int64_t(rng()%1000000000000ULL)-500000000000LL;
        int64_t native=0;reinterpret_cast<CategoryFn>(code)(object,&native,resource.data(),definitions[0].data());
        Check(native==incomeatomic::Category(l.pass,object,1,definitions[0].data()).Clamp(),"complete native recursive category parity");
        reinterpret_cast<CategoryFn>(code+4096)(object,&native,resource.data(),nullptr);
        Check(native==incomeatomic::ReadResource(l.pass,object,resource.data()).Clamp(),"complete native unformatted resource parity");
        int64_t slot=13,value=int64_t(rng());int mode=n%8;
        reinterpret_cast<StoreFn>(code+8192)(&slot,value,mode);Check(slot==value,"native store fallback including taken relocated branch");
        l.pass.Begin({{&slot,1}});int64_t old;l.pass.Add(&slot,INT64_MAX,old);
        reinterpret_cast<StoreFn>(code+8192)(&slot,37,mode);numeric::Sum exact;l.pass.Read(&slot,exact);
        Check(slot==37 && exact.Clamp()==37 && l.pass.End(),"store resets wide carry for every memory-order mode");
    }
    Countries globals[2]={{nullptr,&table,1,1},{}};l.tables.tables[0]=reinterpret_cast<uint8_t*>(&globals[0]);l.tables.tables[1]=reinterpret_cast<uint8_t*>(&globals[1]);
    uint8_t* job=reinterpret_cast<uint8_t*>(uintptr_t(123));l.tables.jobs[0]=&job;values={};
    numericpatch::Context c{};Check(!incomelifetime::Begin(c,&l.begin[0]) && !l.pass.Active(),"begin rejected outside root lifetime");
    l.depth=1;Check(incomelifetime::Begin(c,&l.begin[0]) && c.rcx==uint64_t(job),"begin snapshots sized arrays and replays job load");
    int64_t old;for(auto delta:{INT64_MAX,INT64_MAX})l.pass.Add(&values[1][1],delta,old);
    for(auto delta:{-INT64_MAX,-INT64_MAX})l.pass.Add(&values[2][1],delta,old);values[3][1]=0;l.pass.Set(&values[3][1],31);
    Check(incomeatomic::ReadResource(l.pass,object,resource.data()).Clamp()==31,"resource read uses exact opposite category carries");
    Check(incomeatomic::VisibleCategory(l.pass,object,1,definitions[0].data(),0,0).Clamp()==31,"collapsed tooltip category cancels oversized children");
    std::array<uint8_t,0x28> tooltip{};
    for(uint8_t level:{uint8_t(0),uint8_t(1)}){tooltip[0x21]=level;Check(incomeatomic::FormattedResource(l.tables,l.pass,object,resource.data(),tooltip.data()).Clamp()==31,"formatted total preserves visible and collapsed levels");}
    c={};c.r9=uint64_t(&values[1][1]);c.rax=INT64_MAX;c.rcx=INT64_MAX;c.rdx=0;
    Check(incomelifetime::CompareSubtract(c,&l) && (c.flags&64) && values[1][1]==INT64_MAX,"compare-subtract removes exact amount from oversized balance");
    c.rax=17;c.rcx=2;Check(incomelifetime::CompareSubtract(c,&l) && !(c.flags&64) && c.rax==INT64_MAX,"failed compare returns observed value for native retry");
    for(auto& row:values)for(auto& v:row)l.pass.Set(&v,0);Check(l.pass.End(),"joined boundary drops all registered state");l.depth=0;
    printf("income lifetime: 80000 native read/store fixtures, sizing, reset, category cancellation and compare/retry; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);return failures?1:0;
}
