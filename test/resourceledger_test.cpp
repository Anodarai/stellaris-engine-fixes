#include <array>
#include <cstdio>
#include <random>
#include "../src/resourceledger.h"

namespace {
int failures=0;
void Check(bool good,const char* label) { if(!good && ++failures<=20) printf("FAIL %s\n",label); }
template<class T> void Set(uint8_t* p,size_t at,T value) { memcpy(p+at,&value,sizeof(value)); }
void Relative(uint8_t* operand,const void* target) {
    int32_t d=static_cast<int32_t>(static_cast<const uint8_t*>(target)-operand-4); memcpy(operand,&d,4);
}
void Jump(uint8_t* p,const void* target) { const uint8_t op[]={0xff,0x25,0,0,0,0};memcpy(p,op,6);memcpy(p+6,&target,8); }
struct Fixture {
    std::array<uint8_t,128> converter{};
    std::array<std::array<uint8_t,64>,4> resources{};
    std::array<uint8_t,4*0x38> records{};
    std::array<uint64_t,8> pairs{};
    std::array<int64_t,4> values{};
    resourceledger::Table table{values.data(),4,nullptr};
    explicit Fixture(const resourceledger::Locations& l) {
        Set(converter.data(),l.conversions,records.data()); Set(converter.data(),l.conversionCount,4);
        for(int i=0;i<4;++i) { Set(resources[i].data(),l.resourceIndex,i); pairs[i*2]=reinterpret_cast<uint64_t>(resources[i].data()); pairs[i*2+1]=100000; }
        records[l.recordFlags]=1;
        Set(records.data(),l.pairs,pairs.data()); Set(records.data(),l.pairCount,4);
    }
};
}
int main(int argc,char** argv) {
    if(argc!=2) { puts("usage: resourceledger_test executable"); return 2; }
    HMODULE module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if(!module) return 2;
    std::string error;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    resourceledger::Locations l;
    if(!resourceledger::Locate(image,l,error)) { puts(error.c_str());return 1; }
    Check(l.resourceIndex==0x18 && l.conversions==0x50 && l.conversionCount==0x5c && l.country==0x10,"derived fields");
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    if(!code) return 2;
    memcpy(code,l.convert,numericpatterns::kConvertSize);
    // Only external references in this leaf routine are reads of the immutable zero constant.
    int relocations=0;
    for(size_t i=0;i+7<numericpatterns::kConvertSize;++i) {
        if((code[i]==0x48 || code[i]==0x4c) && code[i+1]==0x8d && (code[i+2]&0xc7)==5) {
            Relative(code+i+3,code+4096);++relocations;i+=6;
        }
    }
    Check(relocations==2,"converter zero references");
    memcpy(code+2048,code,15); Jump(code+2063,code+15);
    using Fn=resourceledger::ConvertFn;
    auto original=reinterpret_cast<Fn>(code+2048);
    std::mt19937_64 random(0x73c601);
    Fixture old(l),fixed(l);
    for(int n=0;n<100000;++n) {
        for(int i=0;i<4;++i) old.values[i]=fixed.values[i]=static_cast<int64_t>(random()%1000000000000ULL)-100000000000LL;
        for(int i=0;i<4;++i) old.pairs[i*2+1]=fixed.pairs[i*2+1]=1000+random()%1000000;
        old.records[l.recordFlags]=fixed.records[l.recordFlags]=static_cast<uint8_t>(n&1);
        int64_t amount=static_cast<int64_t>(random()%1000000000000ULL)-100000000000LL;
        original(old.converter.data(),&old.table,old.resources[0].data(),amount);
        resourceledger::Convert(l,fixed.converter.data(),&fixed.table,fixed.resources[0].data(),amount);
        Check(old.values==fixed.values,"ordinary signed converter parity");
    }
    fixed.records[l.recordFlags]=0;
    fixed.values[0]=INT64_MAX-5;
    resourceledger::Convert(l,fixed.converter.data(),&fixed.table,fixed.resources[0].data(),-10);
    Check(fixed.values[0]==INT64_MAX,"negative removal saturates before addition");
    fixed.values[0]=INT64_MIN+5;
    resourceledger::Convert(l,fixed.converter.data(),&fixed.table,fixed.resources[0].data(),10);
    Check(fixed.values[0]==INT64_MIN,"removal saturates before subtraction");
    fixed.values[0]=-100;
    resourceledger::Convert(l,fixed.converter.data(),&fixed.table,fixed.resources[0].data(),INT64_MIN);
    Check(fixed.values[0]==INT64_MAX-99,"minimum negative removal preserves full magnitude");
    fixed.values[0]=100;
    resourceledger::ConvertMagnitude(l,fixed.converter.data(),&fixed.table,fixed.resources[0].data(),uint64_t(1)<<63);
    Check(fixed.values[0]==INT64_MIN+100,"minimum addition removes full magnitude");
    fixed.records[l.recordFlags]=1; fixed.values={0,INT64_MAX,0,0}; fixed.pairs[3]=100001;
    resourceledger::ConvertMagnitude(l,fixed.converter.data(),&fixed.table,fixed.resources[0].data(),uint64_t(1)<<63);
    Check(fixed.values[0]<0 && fixed.values[1]>=0,"extreme converter stays signed and bounded");
    numeric::Sum sum;sum.Add(INT64_MAX);sum.Add(INT64_MAX);sum.Subtract(INT64_MAX);
    Check(sum.Clamp()==INT64_MAX,"wide cancellation");
    printf("resource ledger: 100000 native parity cases, boundaries; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE); FreeLibrary(module);return failures?1:0;
}
