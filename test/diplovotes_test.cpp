#include <array>
#include <cstdio>
#include <random>
#include "../src/diplovotes.h"
namespace {
int failures=0;
void Check(bool ok,const char* message){if(!ok && ++failures<20)printf("FAIL %s\n",message);}
template<class T>void Set(uint8_t* p,size_t offset,T value){memcpy(p+offset,&value,sizeof(value));}
void Jump(uint8_t* p,const void* target){uint8_t bytes[]={0xff,0x25,0,0,0,0};memcpy(p,bytes,6);memcpy(p+6,&target,8);}
bool __fastcall Blocked(uint8_t*,uint8_t*){return true;}
bool __fastcall Eligible(uint8_t*,uint8_t* country){return country[0x40]!=0;}
int64_t* __fastcall NoLoan(uint8_t*,int64_t* out,uint8_t*,void*){*out=0;return out;}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_IMAGE_RESOURCE);if(!module)return 2;
    auto image=locate::Describe(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(module)&~uintptr_t(3)));
    diplovotes::Locations l;std::string error;if(!diplovotes::Locate(image,l,error)){puts(error.c_str());return 1;}
    auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,65536,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!code)return 2;
    struct Binding{const uint8_t* from;uint8_t* to;};std::vector<Binding> bindings;
    size_t sizes[]={numericpatterns::kFederationVoteSize,numericpatterns::kVotesNeededSize,numericpatterns::kVoteSplitSize,
        numericpatterns::kBorrowedVotesSize,numericpatterns::kVotesForSize,numericpatterns::kVotesAgainstSize};
    for(int i=0;i<6;++i)bindings.push_back({l.blocks[i].at,code+size_t(i)*4096});
    bindings.push_back({reinterpret_cast<uint8_t*>(l.scores.countryDb),code+30000});
    bindings.push_back({reinterpret_cast<uint8_t*>(l.scores.nullCountry),code+30008});
    bindings.push_back({reinterpret_cast<uint8_t*>(l.modifierTypes),code+30016});
    bindings.push_back({l.scores.blocks[4].at,code+31000});Jump(code+31000,reinterpret_cast<void*>(&NoLoan));
    bindings.push_back({reinterpret_cast<uint8_t*>(l.eligible),code+31032});Jump(code+31032,reinterpret_cast<void*>(&Eligible));
    l.scores.countryDb=reinterpret_cast<uint8_t**>(code+30000);l.scores.nullCountry=reinterpret_cast<uint8_t**>(code+30008);
    l.modifierTypes=reinterpret_cast<uint8_t**>(code+30016);l.scores.blocked=Blocked;l.eligible=Eligible;
    for(int i=0;i<6;++i){auto source=l.blocks[i].at;auto target=code+size_t(i)*4096;memcpy(target,source,sizes[i]);
        for(size_t off=0;off+4<=sizes[i];++off)for(auto b:bindings)if(diploscores::Relative(source,off)==b.from){
            int32_t rel=int32_t(b.to-target-off-4);Set(target,off,rel);}
    }
    std::array<std::array<uint8_t,0x3200>,4> countries{};std::array<uint8_t,0x3200> nullCountry{};
    std::array<uint8_t,0x40> db{};std::array<uint8_t,72> entries{};std::array<uint8_t,0x100> resolution{},proposal{};
    std::array<uint8_t,0xa00> definition{};std::array<uint8_t,0x300> federationType{};
    std::array<uint8_t,0x200> community{},federation{};std::array<uint8_t,0x100> modifier{};std::array<uint8_t,0x160> types{};
    std::array<uint32_t,4> ids{};int modifierId=0;int64_t modifierValue[4][2]{};
    Set(db.data(),0x18,entries.data());Set(db.data(),0x20,uint32_t(4));*l.scores.countryDb=db.data();*l.scores.nullCountry=nullCountry.data();*l.modifierTypes=types.data();
    Set(resolution.data(),0x18,definition.data());Set(definition.data(),0x9d0,modifier.data());Set(modifier.data(),0x48,0);
    Set(resolution.data(),0x38,ids.data());Set(resolution.data(),0x50,ids.data()+2);Set(resolution.data(),0x44,2);Set(resolution.data(),0x5c,2);
    Set(community.data(),0x188,ids.data());Set(community.data(),0x194,4);Set(proposal.data(),0x40,definition.data());Set(federation.data(),0x80,federationType.data());
    for(int i=0;i<4;++i){ids[i]=uint32_t(0x03000000+i);Set(entries.data(),8+size_t(i)*16,countries[i].data());
        Set(countries[i].data(),0x20,ids[i]);Set(countries[i].data(),0x31b0,&modifierId);Set(countries[i].data(),0x31bc,1);Set(countries[i].data(),0x31d8,modifierValue[i]);}
    using SideFn=int64_t*(__fastcall*)(uint8_t*,int64_t*);
    using SplitFn=int64_t*(__fastcall*)(int64_t*,uint8_t*,uint8_t*);
    using FedFn=int64_t*(__fastcall*)(uint8_t*,int64_t*,uint8_t*,uint8_t*);
    using NeedFn=int64_t*(__fastcall*)(int64_t*,uint8_t*,bool,bool);
    std::mt19937_64 rng(91641);
    for(int n=0;n<12000;++n){
        for(int i=0;i<4;++i){Set(countries[i].data(),0x2f70,int64_t(rng()%10000000000)-3000000000);countries[i][0x40]=uint8_t(rng()%2);
            modifierValue[i][0]=int64_t(rng()%300000)-150000;}
        Set(types.data(),0xa0,int64_t(rng()%100000)-50000);Set(types.data(),0xa8,uint32_t(rng()%2?32:0));
        int64_t a,b;
        reinterpret_cast<SideFn>(code+4*4096)(resolution.data(),&a);b=diplovotes::Side(l,resolution.data(),false).Clamp();Check(a==b,"native votes for parity");
        reinterpret_cast<SideFn>(code+5*4096)(resolution.data(),&a);b=diplovotes::Side(l,resolution.data(),true).Clamp();Check(a==b,"native votes against parity");
        int64_t split[3],fixed[3];reinterpret_cast<SplitFn>(code+2*4096)(split,community.data(),resolution.data());
        diplovotes::PublishSplit(diplovotes::Split(l,community.data(),resolution.data()),fixed);
        Check(memcmp(split,fixed,sizeof(split))==0,"native coupled vote split parity");
        Set(definition.data(),0x50,int(rng()%2?0x40ed:0));Set(proposal.data(),0x24,ids[rng()%4]);federationType[0x2be]=uint8_t(rng()%2);
        reinterpret_cast<FedFn>(code)(countries[0].data(),&a,proposal.data(),federation.data());
        Check(a==diplovotes::Federation(l,countries[0].data(),proposal.data(),federation.data()).Clamp(),"native federation vote parity");
        std::array<int64_t,4> totals{0,0,int64_t(rng()%1000000000000000LL)-500000000000000LL,int64_t(rng()%1000000000000000LL)-500000000000000LL};
        bool ignore=rng()%2!=0,unanimous=rng()%2!=0;
        reinterpret_cast<NeedFn>(code+4096)(&a,reinterpret_cast<uint8_t*>(totals.data()),ignore,unanimous);
        Check(a==diplovotes::Needed(reinterpret_cast<uint8_t*>(totals.data()),ignore,unanimous).Clamp(),"native required vote rounding parity");
    }
    using numeric::Integer;std::array<Integer,3> huge{Integer(INT64_MAX)*Integer(3),Integer(INT64_MAX),Integer(INT64_MAX)*Integer(4)};int64_t out[3];
    diplovotes::PublishSplit(huge,out);Check(out[2]==INT64_MAX && out[0]==INT64_MAX/4*3+2 && out[1]==INT64_MAX/4,"wide vote split retains three-to-one ratio");
    Set(definition.data(),0x50,0);federationType[0x2be]=0;Set(countries[0].data(),0x2f70,int64_t(9000000000000000000LL));
    Check(diplovotes::Federation(l,countries[0].data(),proposal.data(),federation.data()).Clamp()==9000000000000000000LL,"federation weight no longer narrows to int32");
    std::array<uint8_t,0x1600> frame{};std::array<uint8_t,0x40> ballot{},votes{};
    Set(ballot.data(),8,proposal.data());Set(ballot.data(),0x20,votes.data());Set(ballot.data(),0x2c,4);
    for(int i=0;i<4;++i){Set(votes.data(),size_t(i)*16+8,ids[i]);Set(votes.data(),size_t(i)*16+12,i==3?1:0);
        Set(countries[i].data(),0x2f70,int64_t(9000000000000000000LL));}
    numericpatch::Context decision{};decision.rbp=uint64_t(frame.data());int64_t required=123;
    Set(frame.data(),0x14b8,ballot.data());Set(frame.data(),0x14c8,federation.data());frame[0x14b0]=1;
    decision.rcx=uint64_t(&required);decision.r9=0;
    Check(diplovotes::Decision(decision,&l) && int64_t(decision.r12)>0 && required==0,"oversized federation majority passes without independent clamps");
    decision.r9=1;
    Check(diplovotes::Decision(decision,&l) && int64_t(decision.r12)<0 && int64_t(decision.r15)>0,"oversized federation unanimity rejects the same ballot");
    printf("diplomatic votes: 60000 native consumer fixtures and extreme ratios; %d failures\n",failures);
    VirtualFree(code,0,MEM_RELEASE);FreeLibrary(module);return failures?1:0;
}
