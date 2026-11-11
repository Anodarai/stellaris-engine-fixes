#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <fstream>
#include "../src/incomeoverflow.h"
namespace {
int failures=0,listAllocations=0,listFrees=0;
void Check(bool ok,const char* label){if(!ok && ++failures<=20)printf("FAIL %s\n",label);}
template<class T>void Set(uint8_t* p,size_t offset,T value){memcpy(p+offset,&value,sizeof(value));}
void Jump(uint8_t* p,const void* target){uint8_t op[]={0xff,0x25,0,0,0,0};memcpy(p,op,6);memcpy(p+6,&target,8);}
struct Plan {std::array<uint8_t,0x200> bytes{};std::array<int64_t,3> base{};std::vector<uint8_t*> children[4];};
template<int kind>incomeoverflow::PlanList* __fastcall List(uint8_t* object,incomeoverflow::PlanList* out,void*){
    auto& plan=*reinterpret_cast<Plan*>(object);out->data=out->local;out->capacity=8;out->count=static_cast<int>(plan.children[kind].size());
    if(out->count>8){out->data=static_cast<uint8_t**>(malloc(sizeof(void*)*out->count));out->capacity=out->count;++listAllocations;}
    for(int i=0;i<out->count;++i)out->data[i]=plan.children[kind][i];return out;
}
void __fastcall Free(void* p){++listFrees;free(p);}
void __fastcall ArrayFree(void*,void* p){free(p);}
void __fastcall Assign(incomeoverflow::Array* a,int,const int64_t* data,int count){
    a->data=count?static_cast<int64_t*>(malloc(sizeof(int64_t)*count)):nullptr;a->capacity=a->count=count;
    if(count)memcpy(a->data,data,sizeof(int64_t)*count);
}
numeric::Integer Parse(const std::string& s){numeric::Integer result;bool negative=!s.empty() && s[0]=='-';
    for(size_t i=negative?1:0;i<s.size();++i)result=result*numeric::Integer(10)+numeric::Integer(s[i]-'0');
    return negative?-result:result;
}
void Oracle(const char* path){std::ifstream in(path);std::string sa,sb,se;char operation;size_t n=0;
    while(in>>operation>>sa>>sb>>se){auto a=Parse(sa),b=Parse(sb),expected=Parse(se),actual=a;
        if(operation=='+')actual=a+b;if(operation=='-')actual=a-b;if(operation=='*')actual=a*b;
        if(operation=='/')actual=a/b;if(operation=='%')actual=a%b;
        if(operation=='f')actual=a.FloorDivide(b);
        if(operation=='c')actual=numeric::Integer(a.Clamp());
        if(operation=='a')actual+=actual;
        if(operation=='s')actual-=actual;
        Check(actual==expected,"independent exact integer oracle");++n;}
    Check(in.eof() && n>0,"complete integer oracle");printf("Exact integer oracle: %zu cases\n",n);
}
void Native(const char* path){
    auto module=LoadLibraryExA(path,nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module){Check(false,"map game");return;}
    auto img=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    incomeoverflow::Locations l;std::string error;if(!incomeoverflow::Locate(img,l,error)){puts(error.c_str());Check(false,"locate");return;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,16384,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!code){Check(false,"allocate code");return;}
    memcpy(code,l.calculate,numericpatterns::kIncomeScalingSize);
    struct Binding{uint8_t* from;uint8_t* to;};std::vector<Binding> bindings;
    auto bind=[&](uint8_t* from,const void* target){auto stub=code+4096+bindings.size()*32;Jump(stub,target);bindings.push_back({from,stub});};
    incomeoverflow::ListFn lists[]={List<0>,List<1>,List<2>,List<3>};
    for(int i=0;i<4;++i){bind(reinterpret_cast<uint8_t*>(l.lists[i]),reinterpret_cast<void*>(lists[i]));l.lists[i]=lists[i];}
    bind(reinterpret_cast<uint8_t*>(l.assign),reinterpret_cast<void*>(&Assign));bind(reinterpret_cast<uint8_t*>(l.free),reinterpret_cast<void*>(&Free));
    void* allocatorVT[3]={nullptr,nullptr,reinterpret_cast<void*>(&ArrayFree)};void* allocatorObject=allocatorVT;
    memcpy(code+8200,&allocatorObject,sizeof(allocatorObject));
    bindings.push_back({static_cast<uint8_t*>(l.allocator),code+8200});
    // The list vtable is only stored during cleanup, never called by this routine.
    auto listVT=incomeoverflow::Relative(l.calculate,0x115);bindings.push_back({listVT,code+8256});
    for(size_t off=0;off+4<=numericpatterns::kIncomeScalingSize;++off)for(const auto& b:bindings)
        if(locate::RelTarget(l.calculate+off,l.calculate+off+4)==b.from){int32_t delta=static_cast<int32_t>(b.to-code-off-4);memcpy(code+off,&delta,4);}
    l.allocator=code+8200;l.assign=Assign;l.free=Free;
    auto native=reinterpret_cast<incomeoverflow::CalculateFn>(code);
    std::array<Plan,20> plans;for(auto& p:plans){Set(p.bytes.data(),l.baseData,p.base.data());Set(p.bytes.data(),l.baseCount,3);}
    std::array<int64_t,3> targetValues{};incomeoverflow::Table target{};target.array=&target.storage;target.storage={targetValues.data(),3,3};
    std::mt19937_64 rng(0x719329);
    for(int n=0;n<12000;++n){
        for(auto& p:plans){for(auto& list:p.children)list.clear();for(auto& v:p.base)v=static_cast<int64_t>(rng()%81)-30;}
        for(auto& t:targetValues)t=static_cast<int64_t>(rng()%301)-100;
        for(int kind=0;kind<4;++kind){int count=static_cast<int>(rng()%4);
            for(int j=0;j<count;++j){auto& child=plans[1+kind*4+j];plans[0].children[kind].push_back(child.bytes.data());if(kind==1)child.base[0]=1+rng()%20;}}
        // Add one nested mandatory plan; the graph remains a tree with stable pointers.
        plans[1].children[0].push_back(plans[19].bytes.data());
        int mode=n%3==0?1:0;const auto* input=n%5==0?nullptr:&target;
        incomeoverflow::Table a{},b{};native(plans[0].bytes.data(),&a,nullptr,input,mode);bool writing=false;
        incomeoverflow::Calculate(l,plans[0].bytes.data(),&b,nullptr,input,mode,writing);
        Check(a.array->count==b.array->count && !memcmp(a.array->data,b.array->data,24),"complete native recursive income parity");
        free(a.array->data);free(b.array->data);
    }
    for(auto& p:plans){for(auto& c:p.children)c.clear();p.base={0,0,0};}
    auto& root=plans[0];root.children[1].push_back(plans[1].bytes.data());plans[1].base={1,1,0};
    root.children[3].push_back(plans[2].bytes.data());plans[2].base={0,2,-2};
    targetValues={INT64_MAX,INT64_MAX,0};incomeoverflow::Table out{};bool writing=false;
    incomeoverflow::Calculate(l,root.bytes.data(),&out,nullptr,&target,0,writing);
    Check(out.array->data[0]==INT64_MAX && out.array->data[1]==INT64_MAX && out.array->data[2]==INT64_MIN,"round count beyond int64 and scaled optional results");free(out.array->data);
    root.base={INT64_MIN,0,0};plans[1].base={1,0,0};plans[2].base={0,1,0};targetValues={INT64_MAX,INT64_MAX,INT64_MAX};
    incomeoverflow::Calculate(l,root.bytes.data(),&out,nullptr,&target,0,writing);
    Check(out.array->data[0]==INT64_MAX && out.array->data[1]==INT64_MAX,"full signed-range threshold distance");free(out.array->data);
    // Both accepted scaling children contain a positive resource, but their net growth is zero.
    root.base={0,0,0};root.children[1].push_back(plans[3].bytes.data());plans[1].base={1,-1,0};plans[3].base={-1,1,0};plans[2].base={7,0,0};
    incomeoverflow::Calculate(l,root.bytes.data(),&out,nullptr,&target,0,writing);
    Check(out.array->data[0]==7 && out.array->data[1]==0,"unreachable mixed-sign loop terminates after one round");free(out.array->data);
    for(auto& c:root.children)c.clear();root.base={1,2,3};
    for(int i=1;i<20;++i){plans[i].base={i,-i,1};root.children[0].push_back(plans[i].bytes.data());}
    incomeoverflow::Table nativeOut{};native(root.bytes.data(),&nativeOut,nullptr,nullptr,1);
    incomeoverflow::Calculate(l,root.bytes.data(),&out,nullptr,nullptr,1,writing);
    Check(!memcmp(nativeOut.array->data,out.array->data,24),"heap-backed child list parity");
    free(nativeOut.array->data);free(out.array->data);
    // Native prologue trampoline remains executable for fallback.
    memcpy(code+2048,code,incomeoverflow::kStolen);Jump(code+2048+incomeoverflow::kStolen,code+incomeoverflow::kStolen);
    reinterpret_cast<incomeoverflow::CalculateFn>(code+2048)(root.bytes.data(),&out,nullptr,nullptr,1);free(out.array->data);
    Check(listAllocations>0 && listAllocations==listFrees,"native and replacement list ownership");
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);
    printf("income scaling: 12000 full native recursive fixtures, exact extreme counts and unreachable targets\n");
}
}
int main(int argc,char** argv){if(argc<2)return 2;
    if(argc==3 && !strcmp(argv[1],"--oracle"))Oracle(argv[2]);else for(int i=1;i<argc;++i)Native(argv[i]);
    printf("%s (%d failures)\n",failures?"FAIL":"PASS",failures);return failures?1:0;
}
