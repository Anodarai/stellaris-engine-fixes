#include <array>
#include <cstdio>
#include <random>
#include "../src/designcache.h"
namespace {
int failures=0;
void Check(bool b,const char* name){if(!b && ++failures<=20)printf("FAIL %s\n",name);}
template<class T>void Set(uint8_t* p,size_t off,T v){memcpy(p+off,&v,sizeof(v));}
using C=numericpatch::Context;
constexpr uint64_t C::* registers[]={&C::rax,&C::rcx,&C::rdx,&C::rbx,nullptr,&C::rbp,&C::rsi,&C::rdi,
    &C::r8,&C::r9,&C::r10,&C::r11,&C::r12,&C::r13,&C::r14,&C::r15};
numericpatch::Code Prologue(){
    numericpatch::Code c;c.Put({0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57});
    C sample{};
    for(uint8_t reg=0;reg<16;++reg)if(reg!=1 && reg!=4){
        uint8_t off=static_cast<uint8_t>(reinterpret_cast<uint8_t*>(&(sample.*registers[reg]))-reinterpret_cast<uint8_t*>(&sample));
        c.Put({static_cast<uint8_t>(0x48|(reg>=8?4:0)),0x8b,static_cast<uint8_t>(0x41|((reg&7)<<3)),off});
    }
    c.Put({0x48,0x8b,0x49,static_cast<uint8_t>(offsetof(C,rcx))});return c;
}
void Tail(numericpatch::Code& c,uint64_t C::* result){
    uint8_t reg=0;while(reg<16 && registers[reg]!=result)++reg;
    c.Put({static_cast<uint8_t>(0x48|(reg>=8?1:0)),0x8b,static_cast<uint8_t>(0xc0|(reg&7))});
    c.Put({0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0x5b,0xc3});
}

}
using shipstats::Read;
struct Contribution {size_t offset;int64_t value;int mode;bool operator==(const Contribution& b)const{return offset==b.offset && value==b.value && mode==b.mode;}};
std::vector<Contribution> captured;uint8_t* captureBase=nullptr;
void __fastcall Capture(uint8_t* stat,int64_t* value,uint32_t,void*,int mode){captured.push_back({size_t(stat-captureBase),*value,mode});}
void ModifierTests(designcache::Locations& l){
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    memcpy(code,l.functions[5],numericpatterns::kDesignAddModifierSize);code[4096]=0xff;code[4097]=0x25;memset(code+4098,0,4);void* hook=reinterpret_cast<void*>(&Capture);memcpy(code+4102,&hook,8);
    auto types=l.stats.modifierTypes;std::vector<uint8_t> definitions(0x1000*0xb0);auto cell=reinterpret_cast<uint8_t**>(code+4200);*cell=definitions.data();l.stats.modifierTypes=cell;
    for(size_t off=0;off+4<=numericpatterns::kDesignAddModifierSize;++off){auto target=shipstats::Relative(l.functions[5],off);uint8_t* to=nullptr;
        if(target==l.functions[1])to=code+4096;else if(target==reinterpret_cast<uint8_t*>(types))to=code+4200;
        if(to){int32_t rel=static_cast<int32_t>(to-code-off-4);memcpy(code+off,&rel,4);}}
    std::array<uint8_t,0x340> stats{};std::array<uint8_t,0xc0> mods{};captureBase=stats.data();
    std::vector<int> ids;for(const auto& spec:designcache::kModifiers)ids.push_back(spec.id);std::vector<int64_t> values(ids.size()*2);
    Set(mods.data(),0x10,ids.data());Set(mods.data(),0x1c,int(ids.size()));Set(mods.data(),0x38,values.data());std::mt19937_64 rng(83217);
    using Fn=void(__fastcall*)(uint8_t*,uint8_t*,uint32_t,void*,int);
    for(int n=0;n<10000;++n){for(size_t i=0;i<ids.size();++i){values[i*2]=int64_t(rng()%1000000000)-500000000;Set(definitions.data(),size_t(ids[i])*0xb0+0xa8,uint32_t(rng()%2)*32);}
        int count=int(rng()%1000);captured.clear();reinterpret_cast<Fn>(code)(stats.data(),mods.data(),2,nullptr,count);auto expected=captured;captured.clear();
        designcache::AddModifiers(l,stats.data(),mods.data(),2,count,[&](uint8_t* stat,int64_t value,int mode){Capture(stat,&value,2,nullptr,mode);});
        Check(captured==expected,"complete native design modifier contribution order and values");}
    std::fill(values.begin(),values.end(),0);definitions.assign(definitions.size(),0);*cell=definitions.data();
    for(size_t i=0;i<ids.size();++i)if(ids[i]==0x66)values[i*2]=INT64_MAX;
    std::array<uint8_t,0x110> record{};designcache::Pass pass(stats.data(),record.data());pass.poweredComponents=INT32_MAX;
    {designcache::Scope scope(pass);designcache::AddModifiers(l,stats.data(),mods.data(),2,INT32_MAX,[](uint8_t*,int64_t,int){});
        Check(pass.stats[13].values[0]==numeric::Integer(INT64_MAX)*numeric::Integer(INT32_MAX),"power per component remains exact beyond int64");}
    l.stats.modifierTypes=types;VirtualFree(code,0,MEM_RELEASE);
}
void TestBlock(designcache::Locations& l,int index){
    auto& live=l.blocks[index];auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,16384,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    auto body=Prologue();size_t start=body.b.size();body.b.insert(body.b.end(),live.at,live.at+live.span);
    Tail(body,index?&C::rbx:&C::rax);size_t invalid=body.b.size();body.Put({0x48,0xbb});body.U64(uint64_t(INT64_MIN));Tail(body,&C::rbx);
    if(index){
        for(size_t offset:{size_t(0x26e),size_t(0x277)}){size_t at=start+offset-0xab;int32_t delta=static_cast<int32_t>(invalid-at-6);memcpy(body.b.data()+at+2,&delta,4);}
        int32_t rel=static_cast<int32_t>(12000-(start+0x1e7-0xab+4));memcpy(body.b.data()+start+0x1e7-0xab,&rel,4);
    }
    memcpy(code,body.b.data(),body.b.size());memcpy(code+4096,body.b.data(),body.b.size());
    if(index){int32_t rel=static_cast<int32_t>(12000-(4096+start+0x1e7-0xab+4));memcpy(code+4096+start+0x1e7-0xab,&rel,4);l.excessScale=reinterpret_cast<int64_t*>(code+12000);}
    numericpatch::Block block;block.at=code+start;block.stolen=live.stolen;block.span=live.span;block.handler=live.handler;block.data=&l;
    std::vector<uint8_t> jump;Check(numericpatch::Prepare(block,code+8192,1024,jump),"prepare design arithmetic stub");memcpy(block.at,jump.data(),jump.size());
    using Fn=int64_t(__fastcall*)(C*);auto fixed=reinterpret_cast<Fn>(code),native=reinterpret_cast<Fn>(code+4096);
    std::array<uint8_t,0x340> stats{};std::array<uint8_t,0x48*12> records{};auto stat=stats.data()+(index?0x1a0:0);
    Set(stat,0x10,records.data());Set(stat,0x1c,12);std::mt19937_64 rng(9182);
    for(int n=0;n<20000;++n){
        for(size_t i=0;i<12;++i){int mode=int(i%3);int64_t v=int64_t(rng()%100000000)-30000000;
            if(index){if(mode==2)v=-int64_t(rng()%100000);else if(mode==0)v=int64_t(rng()%100000000)+100000;else v=int64_t(rng()%50000);}
            Set(records.data(),i*0x48,v);Set(records.data(),i*0x48+8,mode);}
        C c{};c.r15=reinterpret_cast<uint64_t>(stat);c.r13=reinterpret_cast<uint64_t>(stats.data());c.rbp=100000;
        if(index)*const_cast<int64_t*>(l.excessScale)=int64_t(rng()%200000)+100000;
        int64_t a=native(&c);if(!index)a=Read<int64_t>(stat,0);
        int64_t b=fixed(&c);if(!index)b=Read<int64_t>(stat,0);
        Check(a==b,"native design finalizer/excess-power arithmetic parity");
    }
    C c{};c.r15=reinterpret_cast<uint64_t>(stat);c.r13=reinterpret_cast<uint64_t>(stats.data());c.rbp=100000;
    records.fill(0);Set(records.data(),0,INT64_MAX);Set(records.data(),0x48,INT64_MAX);Set(records.data(),0x90,-INT64_MAX);
    if(!index){fixed(&c);Check(Read<int64_t>(stat,0)==INT64_MAX,"design flat contributions cancel before final narrowing");
        Set(records.data(),0x48+8,1);Set(records.data(),0x90,INT64_MIN);Set(records.data(),0x90+8,1);Set(records.data(),0xd8,int64_t(-99999));Set(records.data(),0xd8+8,1);
        fixed(&c);Check(Read<int64_t>(stat,0)==0,"oversized design multiplier cancels to zero");}
    else {*const_cast<int64_t*>(l.excessScale)=200000;Check(fixed(&c)==50000,"excess power ratio stays one half at huge power");
        records.fill(0);Check(fixed(&c)==INT64_MIN,"nonpositive power skips even negative bonus threshold");}
    VirtualFree(code,0,MEM_RELEASE);
}
void ScopeTests(designcache::Locations& l){
    using numeric::Integer;std::array<uint8_t,0x340> stats{};std::array<uint8_t,0x110> record{};
    designcache::Pass pass(stats.data(),record.data());
    {designcache::Scope scope(pass);designcache::Add(l,stats.data(),INT64_MAX,0);designcache::Add(l,stats.data(),INT64_MAX,0);designcache::Add(l,stats.data(),-INT64_MAX,0);
        Check(designcache::GetTerms(stats.data()).Value()==Integer(INT64_MAX),"rebuild captures exact contributions before native metadata merging");
        std::array<uint8_t,0x340> nestedStats{};designcache::Pass nested(nestedStats.data(),record.data());
        {designcache::Scope nestedScope(nested);designcache::Add(l,stats.data(),-1,0);Check(designcache::GetTerms(stats.data()).Value()==Integer(INT64_MAX-1),"nested rebuild finds enclosing stat owner");}
        Check(designcache::current==&pass,"nested rebuild restores owner");}
    Check(!designcache::current,"rebuild drops pointer state at end");
    designcache::Pass reused(stats.data(),record.data());Check(reused.stats[0].Value().Zero(),"reused address has no stale wide sum");
    std::array<uint8_t,0x48> description{};Set(stats.data(),0x10,description.data());Set(stats.data(),0x1c,1);Set(description.data(),0x40,uint32_t(71));
    {designcache::Scope scope(reused);
        auto add=[&](const numeric::Integer& exact){int64_t value=exact.Clamp();reused.Remember(stats.data(),0,exact);
            designcache::Description(stats.data(),71,0,&value,true);*reinterpret_cast<int64_t*>(description.data())+=value;};
        add(Integer(INT64_MAX));add(Integer(INT64_MAX));add(-Integer(INT64_MAX));add(-Integer(INT64_MAX));add(Integer(29));
        Check(Read<int64_t>(description.data(),0)==29,"per-source metadata retains opposite oversized contributions");
        add(Integer(INT64_MAX)*Integer(100));add(-(Integer(INT64_MAX)*Integer(100)));
        Check(Read<int64_t>(description.data(),0)==29,"per-source metadata receives exact component/power contributions");
    }
}
bool __fastcall ComponentValid(uint8_t* p){return p[0x60]!=0;}
void GroupTests(){
    std::array<uint8_t,0x18> output{};std::array<uint8_t,0x48> entry{};
    Set(output.data(),8,entry.data());Set(output.data(),0x14,1);Set(entry.data(),0x40,uint32_t(22));
    designcache::GroupPass pass{output.data()};
    {designcache::GroupScope scope(pass);
        for(auto amount:{INT64_MAX,INT64_MAX,-INT64_MAX,-INT64_MAX,int64_t(73)}){
            designcache::GroupDescription(output.data(),22,0,&amount);*reinterpret_cast<int64_t*>(entry.data())+=amount;}
        Check(Read<int64_t>(entry.data(),0)==73,"grouped descriptions retain cancellation across oversized sums");}
    Check(!designcache::group,"grouped descriptions discard pointer state at return");
}
void SpecialTests(designcache::Locations& l){
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,32768,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    struct Binding{const uint8_t* from;uint8_t* to;};std::vector<Binding> bindings;
    auto& components=l.components;const int64_t* saved[10];
    for(size_t i=0;i<10;++i){saved[i]=components.defines[i];bindings.push_back({reinterpret_cast<const uint8_t*>(saved[i]),code+18000+i*8});components.defines[i]=reinterpret_cast<int64_t*>(code+18000+i*8);}
    uint8_t* sources[]={l.functions[4],components.functions[0],components.functions[1],components.mean};
    size_t sizes[]={numericpatterns::kDesignAddSpecialSize,numericpatterns::kWeaponDamageSize,numericpatterns::kStrikeCraftDamageSize,numericpatterns::kComponentRangeMeanSize};
    for(size_t i=0;i<4;++i)bindings.push_back({sources[i],code+i*4096});
    // Modifier/list bookkeeping has no numeric effect on these nine cache fields.
    code[19000]=0xc3;
    for(size_t offset:{size_t(0x4a),size_t(0x6c),size_t(0x45f)})bindings.push_back({shipstats::Relative(l.functions[4],offset),code+19000});
    for(size_t i=0;i<4;++i){auto target=code+i*4096;memcpy(target,sources[i],sizes[i]);
        for(size_t off=0;off+4<=sizes[i];++off)for(auto& b:bindings)if(shipstats::Relative(sources[i],off)==b.from){int32_t rel=static_cast<int32_t>(b.to-target-off-4);memcpy(target+off,&rel,4);}}
    using Fn=void(__fastcall*)(uint8_t*,uint8_t*);std::array<uint8_t,0x600> design{};std::array<uint8_t,0x1400> weapon{},strike{};
    void* vt[9]{};vt[8]=reinterpret_cast<void*>(&ComponentValid);Set(weapon.data(),0,vt);Set(strike.data(),0,vt);weapon[0x60]=strike[0x60]=1;strike[0x1e1]=2;
    std::mt19937_64 rng(71134);
    for(int n=0;n<10000;++n){design.fill(0);designcache::SpecialPass pass{design.data()};designcache::SpecialScope scope(pass);
        for(auto d:components.defines)*const_cast<int64_t*>(d)=int64_t(rng()%200000)+100000;
        for(auto c:{weapon.data(),strike.data()})for(size_t o=0x11c8;o<0x1330;o+=8)Set(c,o,int64_t(rng()%200000)-10000);
        Set(strike.data(),0x11c8,int(rng()%20));Set(strike.data(),0x11e8,int64_t(rng()%200000)+100000);weapon[0x1330]=strike[0x11e0]=0;
        for(auto c:{weapon.data(),strike.data(),weapon.data()}){
            designcache::AddSpecial(l,design.data(),c);reinterpret_cast<Fn>(code)(design.data(),c);
            for(size_t i=0;i<9;++i)Check(pass.sums[i].Clamp()==Read<int64_t>(design.data(),0x350+i*8),"native special cache contributions parity");}
        auto raw=pass.sums;Set(design.data(),0x494,2);Set(design.data(),0x4ac,1);designcache::PublishSpecial(pass);
        for(size_t i=0;i<9;++i)Check(Read<int64_t>(design.data(),0x350+i*8)==(i==6?raw[i]:componentmath::Divide(raw[i],numeric::Integer(300000))).Clamp(),"special averages preserve military weighted sum");
    }
    for(size_t i=0;i<10;++i)components.defines[i]=saved[i];VirtualFree(code,0,MEM_RELEASE);
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    designcache::Locations l;std::string error;if(!designcache::Locate(image,l,error)){puts(error.c_str());return 1;}
    TestBlock(l,0);TestBlock(l,1);ScopeTests(l);ModifierTests(l);SpecialTests(l);GroupTests();
    printf("design cache: 40000 native arithmetic, 10000 modifier and 30000 special-component fixtures; %d failures\n",failures);
    FreeLibrary(module);return failures?1:0;
}
