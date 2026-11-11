#include <array>
#include <cstdio>
#include <random>
#include "../src/diploscores.h"
namespace {
int failures=0,allocations=0,releases=0;
void Check(bool ok,const char* label){if(!ok && ++failures<=20)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t offset,T value){memcpy(p+offset,&value,sizeof(value));}
void Jump(uint8_t* p,const void* target){uint8_t op[]={0xff,0x25,0,0,0,0};memcpy(p,op,6);memcpy(p+6,&target,8);}
bool __fastcall Blocked(uint8_t* p,uint8_t*){return p[0x70]!=0;}
void* __fastcall Allocate(size_t bytes){++allocations;return malloc(bytes);}
void* __fastcall Copy(void* out,const void* in,size_t bytes){if(bytes)memmove(out,in,bytes);return out;}
void __fastcall Rotate(void* first,void* middle,void* last){Check(first==middle || middle==last,"native append rotation empty");}
struct List {void** vtable;diploscores::LoanEntry* data;int capacity,count;};
void __fastcall Release(uint8_t* object){auto& l=*reinterpret_cast<List*>(object);if(l.data){++releases;free(l.data);}l.data=nullptr;l.capacity=l.count=0;}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    diploscores::Locations l;std::string error;if(!diploscores::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,16384,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!code)return 2;
    auto original=l.blocks[4].at;memcpy(code,original,numericpatterns::kTotalLoanWeightSize);
    struct Binding{uint8_t* from;uint8_t* to;};std::vector<Binding> bindings;
    auto bindCall=[&](const void* from,const void* to){auto at=code+4096+bindings.size()*32;Jump(at,to);bindings.push_back({reinterpret_cast<uint8_t*>(const_cast<void*>(from)),at});};
    bindCall(reinterpret_cast<void*>(l.blocked),reinterpret_cast<void*>(&Blocked));bindCall(reinterpret_cast<void*>(l.allocate),reinterpret_cast<void*>(&Allocate));
    bindCall(diploscores::Relative(original,0x39d),reinterpret_cast<void*>(&Copy));bindCall(diploscores::Relative(original,0x43e),reinterpret_cast<void*>(&Rotate));
    l.blocked=Blocked;l.allocate=Allocate;
    auto bindCell=[&](const void* from,size_t offset){bindings.push_back({reinterpret_cast<uint8_t*>(const_cast<void*>(from)),code+offset});};
    bindCell(l.favorFactor,8192);bindCell(l.countryDb,8200);bindCell(l.nullCountry,8208);bindCell(l.growth,8216);
    l.favorFactor=reinterpret_cast<int64_t*>(code+8192);l.countryDb=reinterpret_cast<uint8_t**>(code+8200);l.nullCountry=reinterpret_cast<uint8_t**>(code+8208);l.growth=reinterpret_cast<float*>(code+8216);
    Set(code,8192,int64_t(10000));Set(code,8216,float(1.5));
    for(size_t off=0;off+4<=numericpatterns::kTotalLoanWeightSize;++off)for(const auto& b:bindings)if(diploscores::Relative(original,off)==b.from){int32_t rel=static_cast<int32_t>(b.to-code-off-4);memcpy(code+off,&rel,4);}
    l.blocks[4].at=code+2048;memcpy(l.blocks[4].at,code,numericpatterns::kTotalLoanWeightSize);
    for(size_t off=0;off+4<=numericpatterns::kTotalLoanWeightSize;++off)for(const auto& b:bindings)if(diploscores::Relative(code,off)==b.to){int32_t rel=static_cast<int32_t>(b.to-l.blocks[4].at-off-4);memcpy(l.blocks[4].at+off,&rel,4);}
    std::vector<uint8_t> jump;Check(numericpatch::Prepare(l.blocks[4],code+6144,1024,jump),"prepare total loan hook");memcpy(l.blocks[4].at,jump.data(),jump.size());
    using Fn=int64_t*(__fastcall*)(uint8_t*,int64_t*,uint8_t*,List*);
    std::array<std::array<uint8_t,0x3200>,8> countries{};std::array<uint8_t,0x3200> borrower{},nullCountry{};
    std::array<uint8_t,0x80> resolution{},db{},rows{};std::array<uint8_t,136> dbEntries{};
    std::array<uint8_t,108> buckets{};std::array<uint32_t,9> voters{};
    Set(code,8200,db.data());Set(code,8208,nullCountry.data());Set(db.data(),0x18,dbEntries.data());Set(db.data(),0x20,uint32_t(8));
    Set(borrower.data(),0x20,uint32_t(99));Set(nullCountry.data(),0x20,UINT32_MAX);Set(borrower.data(),0x30d8,rows.data());Set(borrower.data(),0x30e4,1);
    Set(resolution.data(),8,uint32_t(123));Set(resolution.data(),0x38,voters.data());Set(rows.data(),8,uint32_t(123));Set(rows.data(),0x18,buckets.data());Set(rows.data(),0x24,7);buckets[96]=1;
    for(size_t i=0;i<8;++i){Set(countries[i].data(),0x20,uint32_t(0x07000000+i));Set(dbEntries.data(),8+i*16,countries[i].data());}
    void* vt[4]={nullptr,nullptr,nullptr,reinterpret_cast<void*>(&Release)};
    std::mt19937_64 rng(71687);
    for(int n=0;n<16000;++n){
        resolution[0x70]=rng()%8==0;int count=0;
        for(size_t i=0;i<8;++i){Set(countries[i].data(),0x2f70,int64_t(rng()%1000000000)-300000000);buckets[i*12]=rng()%2;
            uint32_t id=uint32_t(0x07000000+i);Set(buckets.data(),i*12+4,(rng()%10==0)?id+0x01000000:id);Set(buckets.data(),i*12+8,int32_t(rng()%40)-10);
            if(rng()%2)voters[count++]=id;}
        if(rng()%2)voters[count++]=99;Set(resolution.data(),0x44,count);
        *const_cast<int64_t*>(l.favorFactor)=int64_t(rng()%30000)-5000;
        int old=int(rng()%5),capacity=n%3==0?old:old+10;List a{vt,nullptr,capacity,old},b{vt,nullptr,capacity,old};
        a.data=static_cast<diploscores::LoanEntry*>(Allocate(size_t(capacity)*16));b.data=static_cast<diploscores::LoanEntry*>(Allocate(size_t(capacity)*16));
        for(int i=0;i<old;++i)a.data[i]=b.data[i]={uint32_t(i),0,int64_t(i+100)};
        int64_t native=0,patched=0;bool detail=n%4!=0;
        reinterpret_cast<Fn>(code)(borrower.data(),&native,resolution.data(),detail?&a:nullptr);
        reinterpret_cast<Fn>(l.blocks[4].at)(borrower.data(),&patched,resolution.data(),detail?&b:nullptr);
        Check(native==patched,"native total loan arithmetic");Check(a.count==b.count,"native total loan detail count");
        for(int i=0;i<a.count && i<b.count;++i)Check(a.data[i].country==b.data[i].country && a.data[i].value==b.data[i].value,"native total loan detail order/value");
        Release(reinterpret_cast<uint8_t*>(&a));Release(reinterpret_cast<uint8_t*>(&b));
    }
    resolution[0x70]=0;Set(resolution.data(),0x44,0);voters[0]=99;Set(resolution.data(),0x44,1);buckets.fill(0);buckets[96]=1;
    *const_cast<int64_t*>(l.favorFactor)=INT64_MAX;
    for(int i=0;i<3;++i){buckets[i*12]=1;Set(buckets.data(),i*12+4,uint32_t(0x07000000+i));Set(buckets.data(),i*12+8,int32_t(2));Set(countries[i].data(),0x2f70,i==2?-INT64_MAX:INT64_MAX);}
    auto result=diploscores::TotalLoans(l,borrower.data(),resolution.data(),true);Check(result.total.Clamp()==INT64_MAX,"wide loan sum saturates positive");
    buckets[0]=0;result=diploscores::TotalLoans(l,borrower.data(),resolution.data(),true);Check(result.total.Zero() && result.entries.size()==2,"oversized positive and negative loans cancel");
    List detail{vt,nullptr,0,0};int64_t out=1;reinterpret_cast<Fn>(l.blocks[4].at)(borrower.data(),&out,resolution.data(),&detail);
    Check(out==0 && detail.count==2 && detail.data[0].value==INT64_MAX && detail.data[1].value==INT64_MIN,"wide total and bounded detail ABI");Release(reinterpret_cast<uint8_t*>(&detail));
    Check(allocations==releases,"native allocator ownership balanced");
    printf("total diplomatic loans: 16000 full native fixtures, detail allocation/order and exact cancellation; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);return failures?1:0;
}
