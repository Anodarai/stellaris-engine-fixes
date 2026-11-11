#include <array>
#include <cstdio>
#include <random>
#include "../src/incomeatomic.h"
namespace {
int failures=0;void Check(bool b,const char* name){if(!b && ++failures<=20)printf("FAIL %s\n",name);}
template<class T>void Set(uint8_t* p,size_t off,T v){memcpy(p+off,&v,sizeof(v));}
struct Atomic {uint8_t* vtable=nullptr;int64_t* data=nullptr;int capacity=0,count=0;};
struct Categorized {Atomic total;uint8_t* vtable=nullptr;Atomic* categories=nullptr;int capacity=0,count=0;};
struct Array {int64_t* data;int capacity,count;};
struct Table {Array* data=nullptr;uint64_t reserved[3]{};};
struct Output {Table total;uint8_t* vtable=nullptr;Table* categories=nullptr;int capacity=0,count=0;};
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    incomeatomic::Locations l;std::string error;if(!incomeatomic::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    struct Binding{const uint8_t* from;uint8_t* to;};std::vector<Binding> bindings{{reinterpret_cast<uint8_t*>(l.resources),code+4096},{reinterpret_cast<uint8_t*>(l.nullResource),code+4104},{l.drain,code}};
    uint8_t* functions[]={l.drain,l.categorizedDrain};size_t sizes[]={numericpatterns::kAtomicDrainSize,numericpatterns::kAtomicCategorizedDrainSize};
    for(int i=0;i<2;++i){auto target=code+i*2048;memcpy(target,functions[i],sizes[i]);
        for(size_t off=0;off+4<=sizes[i];++off)for(const auto& b:bindings)if(incomeatomic::Relative(functions[i],off)==b.from){int32_t rel=static_cast<int32_t>(b.to-target-off-4);memcpy(target+off,&rel,4);}}
    l.resources=reinterpret_cast<uint8_t**>(code+4096);l.nullResource=reinterpret_cast<uint8_t**>(code+4104);
    std::array<uint8_t,0x20> manager{},nullResource{};std::array<std::array<uint8_t,0x20>,4> resources{};uint8_t* ptrs[4];
    for(int i=0;i<4;++i){ptrs[i]=resources[i].data();Set(ptrs[i],0x18,3-i);}Set(nullResource.data(),0x18,0);
    Set(manager.data(),8,ptrs);Set(manager.data(),0x14,4);*l.resources=manager.data();*l.nullResource=nullResource.data();
    std::array<std::array<int64_t,6>,3> inputs{},outputs{},savedInput{},savedOutput{},expected{};
    Atomic atomics[3];Array arrays[3]={{outputs[0].data(),6,6},{outputs[1].data(),6,6},{outputs[2].data(),6,6}};
    Table tables[3];for(int i=0;i<3;++i){atomics[i].data=inputs[i].data();atomics[i].capacity=atomics[i].count=6;tables[i].data=&arrays[i];}
    Categorized categorized{atomics[0],nullptr,atomics+1,2,2};Output destination{tables[0],nullptr,tables+1,2,2};
    using Fn=void(__fastcall*)(uint8_t*,uint8_t*);incomeaccumulator::Pass pass;std::mt19937_64 rng(731);
    for(int n=0;n<20000;++n){for(int i=0;i<3;++i)for(int j=0;j<6;++j){inputs[i][j]=int64_t(rng()%1000000000000ULL)-500000000000LL;outputs[i][j]=int64_t(rng()%1000000000000ULL)-500000000000LL;}
        savedInput=inputs;savedOutput=outputs;bool categories=n%2!=0;
        reinterpret_cast<Fn>(code+(categories?2048:0))(reinterpret_cast<uint8_t*>(&categorized),reinterpret_cast<uint8_t*>(&destination));expected=outputs;
        inputs=savedInput;outputs=savedOutput;auto plan=incomeatomic::Plan(l,reinterpret_cast<uint8_t*>(&categorized),reinterpret_cast<uint8_t*>(&destination),categories);
        bool writing=false;incomeatomic::Drain(pass,plan,writing);Check(outputs==expected && writing,"native atomic drain parity including repeated null-resource mapping");
        for(int i=0;i<(categories?3:1);++i)for(auto v:inputs[i])Check(v==0,"drain resets exactly the consumed tables");}
    inputs={};outputs={};inputs[0][0]=INT64_MAX;inputs[0][1]=-INT64_MAX;
    pass.Begin({{inputs[0].data(),6}});int64_t old;pass.Add(inputs[0].data(),INT64_MAX,old);pass.Add(inputs[0].data()+1,-INT64_MAX,old);
    Set(resources[0].data(),0x18,0);Set(resources[1].data(),0x18,0);outputs[0][0]=29;
    auto plan=incomeatomic::Plan(l,reinterpret_cast<uint8_t*>(&categorized),reinterpret_cast<uint8_t*>(&destination),false);bool writing=false;
    incomeatomic::Drain(pass,plan,writing);Check(outputs[0][0]==29 && pass.End(),"oversized positive/negative drains cancel before destination narrowing");
    inputs[0][0]=17;writing=false;bool rejected=false;try{incomeatomic::Drain(pass,{{inputs[0].data(),inputs[0].data()}},writing);}catch(...){rejected=true;}
    Check(rejected && !writing && inputs[0][0]==17,"invalid overlapping drain rejected before mutation");
    printf("income atomic boundaries: 20000 complete native drain fixtures, exact carry cancellation and mutation validation; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);return failures?1:0;
}
