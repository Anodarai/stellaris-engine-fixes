#include <array>
#include <random>
#include <cstdio>
#include "../src/resourceledger.h"
namespace {
int failures=0;resourceledger::Locations* locations;
void Check(bool b,const char* label){if(!b && ++failures<=15)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t off,T v){memcpy(p+off,&v,sizeof(v));}
void Jump(uint8_t* p,const void* f){uint8_t j[]={0xff,0x25,0,0,0,0};memcpy(p,j,6);memcpy(p+6,&f,8);}
struct Effect {int index;int64_t amount;void* updates;bool operator==(const Effect& b)const{return index==b.index && amount==b.amount && updates==b.updates;}};
std::vector<Effect> effects;
bool __fastcall Valid(uint8_t* p){return p[8]!=0;}
bool __fastcall Available(uint8_t* p){return p[8]!=0;}
void __fastcall Read(uint8_t* economy,void* table,int){memcpy(resourceledger::Values(table),*reinterpret_cast<void**>(economy+0x30),32);}
void __fastcall Write(uint8_t* economy,void* table,int flag){memcpy(*reinterpret_cast<void**>(economy+0x30),resourceledger::Values(table),32);Set(economy,0x38,flag);}
void __fastcall Clamp(uint8_t*,void*){}
void __fastcall Situation(uint8_t*,uint8_t*,uint8_t* resource,int64_t amount,void* updates){
    effects.push_back({resourceledger::Index(*locations,resource),amount,updates});
}
void __fastcall Convert(uint8_t* converter,void* table,uint8_t* resource,int64_t amount){resourceledger::Convert(*locations,converter,table,resource,amount);}
void __fastcall ConvertIndex(uint8_t* converter,void* table,int index,int64_t amount){auto resource=resourceledger::Resource(*locations,index);if(Available(resource))Convert(converter,table,resource,amount);}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto mod=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!mod)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mod)&~uintptr_t(3)));
    resourceledger::Locations l;locations=&l;std::string error;if(!resourceledger::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,32768,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));if(!code)return 2;
    std::array<uint8_t,64> economy{},registry{};std::array<uint8_t,128> converter{};
    std::array<std::array<uint8_t,64>,4> resources{};std::array<uint8_t*,4> pointers{};
    std::array<void*,20> economyVT{},converterVT{},resourceVT{};
    economyVT[18]=reinterpret_cast<void*>(&Read);economyVT[19]=reinterpret_cast<void*>(&Write);
    converterVT[1]=reinterpret_cast<void*>(&Valid);resourceVT[7]=reinterpret_cast<void*>(&Available);
    Set(economy.data(),0,economyVT.data());Set(economy.data(),l.converter,converter.data());Set(converter.data(),0,converterVT.data());
    for(int i=0;i<4;++i){pointers[i]=resources[i].data();Set(pointers[i],0,resourceVT.data());Set(pointers[i],l.resourceIndex,i);pointers[i][8]=1;}
    Set(registry.data(),8,pointers.data());Set(registry.data(),0x14,4);uint8_t* registryPointer=registry.data();uint8_t* nullResource=pointers[0];
    struct Binding{uint8_t* from;uint8_t* to;};std::vector<Binding> binds;
    binds.push_back({reinterpret_cast<uint8_t*>(l.resources),code+8192});Set(code,8192,registryPointer);
    binds.push_back({reinterpret_cast<uint8_t*>(l.nullResource),code+8200});Set(code,8200,nullResource);
    auto bind=[&](uint8_t* from,const void* target){auto p=code+4096+binds.size()*32;Jump(p,target);binds.push_back({from,p});};
    bind(reinterpret_cast<uint8_t*>(l.clamp),reinterpret_cast<void*>(&Clamp));bind(reinterpret_cast<uint8_t*>(l.situation),reinterpret_cast<void*>(&Situation));
    bind(l.convert,reinterpret_cast<void*>(&Convert));bind(resourceledger::Relative(l.batches[0].at,0x124),reinterpret_cast<void*>(&ConvertIndex));
    // Fake __chkstk is safe for this four-resource fixture; RAX must retain allocation size.
    code[8000]=0xc3;binds.push_back({resourceledger::Relative(l.batches[0].at,0x58),code+8000});
    binds.push_back({static_cast<uint8_t*>(l.allocatorVtable),code+8240});
    l.resources=&registryPointer;l.nullResource=&nullResource;l.clamp=Clamp;l.situation=Situation;l.allocatorVtable=code+8240;
    size_t sizes[]={numericpatterns::kAddBatchSize,numericpatterns::kRemoveBatchSize,numericpatterns::kAddSize,numericpatterns::kRemoveSize};
    uint8_t* sources[]={l.batches[0].at,l.batches[1].at,l.add,l.remove};
    for(int fn=0;fn<4;++fn){auto from=sources[fn],p=code+fn*1024;memcpy(p,from,sizes[fn]);
        for(size_t off=0;off+4<=sizes[fn];++off)for(const auto& b:binds)if(locate::RelTarget(from+off,from+off+4)==b.from){int32_t delta=static_cast<int32_t>(b.to-p-off-4);memcpy(p+off,&delta,4);}
    }
    using AddBatchFn=void(__fastcall*)(uint8_t*,void*,int,void*);
    using RemoveBatchFn=void(__fastcall*)(uint8_t*,void*);
    std::array<uint8_t,0x20> situationTable{};
    Check(reinterpret_cast<uintptr_t>(situationTable.data())>UINT32_MAX,"situation table exercises high pointer bits");
    std::mt19937_64 rng(8833);
    std::array<int64_t,4> original{},fixed{},amounts{};resourceledger::Table input(amounts.data(),4,nullptr);
    for(int n=0;n<20000;++n){
        for(int i=0;i<4;++i){original[i]=fixed[i]=static_cast<int64_t>(rng()%1000000000000ULL)-100000000000LL;amounts[i]=static_cast<int64_t>(rng()%10000000000ULL)-5000000000LL;pointers[i][8]=static_cast<uint8_t>(rng()%2);}
        converter[8]=static_cast<uint8_t>(rng()%2);bool remove=n%2!=0;
        void* updates=n%3!=0?situationTable.data():nullptr;
        effects.clear();Set(economy.data(),0x30,original.data());
        if(remove)reinterpret_cast<RemoveBatchFn>(code+1024)(economy.data(),&input);
        else reinterpret_cast<AddBatchFn>(code)(economy.data(),&input,37,updates);
        auto nativeEffects=effects;int nativeFlag=*reinterpret_cast<int*>(economy.data()+0x38);
        effects.clear();Set(economy.data(),0x30,fixed.data());
        numericpatch::Context context{};
        context.rcx=reinterpret_cast<uintptr_t>(economy.data());
        context.rdx=reinterpret_cast<uintptr_t>(&input);
        context.r8=37;
        context.r9=reinterpret_cast<uintptr_t>(updates);
        auto batchHook=remove?resourceledger::RemoveBatchHook:resourceledger::BatchHook;
        Check(batchHook(context,&l) && !l.batches[0].failed && !l.batches[1].failed,"batch hook completes");
        Check(original==fixed,"native batch signed balance parity");
        Check(nativeEffects==effects,"native batch situation parity including the full update-table pointer");
        Check(nativeFlag==*reinterpret_cast<int*>(economy.data()+0x38),"native batch write flag parity");
        // Exercise complete single-resource entry points as well as the batch bodies.
        original=fixed;effects.clear();Set(economy.data(),0x30,original.data());
        reinterpret_cast<resourceledger::ChangeFn>(code+(remove?3072:2048))(economy.data(),pointers[0],amounts[0]);nativeEffects=effects;
        effects.clear();Set(economy.data(),0x30,fixed.data());resourceledger::ChangeProgress progress{};
        resourceledger::Change(l,economy.data(),pointers[0],amounts[0],remove,progress);
        Check(original==fixed,"native single-resource balance parity");
        Check(nativeEffects==effects,"native single-resource situation parity including a null update-table pointer");
    }
    converter[8]=0;fixed.fill(INT64_MAX);amounts.fill(1);bool started=false;resourceledger::Batch(l,economy.data(),&input,false,0,nullptr,started);Check(fixed[0]==INT64_MAX,"batch addition saturates");
    converter[8]=1;for(auto p:pointers)p[8]=1;fixed.fill(-100);amounts.fill(INT64_MIN);started=false;
    resourceledger::Batch(l,economy.data(),&input,true,0,nullptr,started);Check(fixed[0]==INT64_MAX-99,"batch removes full negative minimum");
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(mod);printf("resource batches: 20000 native fixtures and extreme signed changes; %d failures\n",failures);return failures?1:0;
}
