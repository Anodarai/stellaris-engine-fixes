#pragma once
#include <array>
#include <set>
#include "incomeatomic.h"
#include "atomicproducers_patterns.h"
namespace incomeproducers {
constexpr size_t kCount=sizeof(atomicpatterns::sites)/sizeof(atomicpatterns::sites[0]);
struct Operand {incomeaccumulator::Pass* pass=nullptr;int operation=0;};
struct Locations {std::array<numericpatch::Block,kCount> blocks;std::array<Operand,kCount> operands;};
inline uint8_t* FunctionStart(const locate::Image& image,uint8_t* address){
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(image.base);auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(image.base+dos->e_lfanew);
    auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto rows=reinterpret_cast<RUNTIME_FUNCTION*>(image.base+directory.VirtualAddress);size_t count=directory.Size/sizeof(RUNTIME_FUNCTION);
    uintptr_t rva=uintptr_t(address)-uintptr_t(image.base);size_t lo=0,hi=count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(rows[mid].BeginAddress<=rva)lo=mid+1;else hi=mid;}
    if(!lo || rva>=rows[lo-1].EndAddress)return nullptr;return image.base+rows[lo-1].BeginAddress;
}
inline uint64_t AddFlags(uint64_t prior,uint64_t a,uint64_t b){
    uint64_t result=a+b;unsigned byte=unsigned(result&255);byte^=byte>>4;byte^=byte>>2;byte^=byte>>1;
    return (prior&~uint64_t(0x8d5)) | (result<a?1:0) | ((byte&1)?0:4) | ((a^b^result)&16) |
        (result==0?64:0) | ((result>>56)&128) | (((~(a^b)&(a^result))>>52)&0x800);
}
inline bool Add(numericpatch::Context& c,const void* opaque){
    auto& data=*static_cast<const Operand*>(opaque);int64_t* address=nullptr;uint64_t* source=nullptr;
    switch(data.operation){
        case 0:address=reinterpret_cast<int64_t*>(c.r8);source=&c.rcx;break;
        case 1:address=reinterpret_cast<int64_t*>(c.rdx);source=&c.rax;break;
        case 2:address=reinterpret_cast<int64_t*>(c.rax+c.rcx*8);source=&c.r8;break;
        case 3:address=reinterpret_cast<int64_t*>(c.rax+c.rcx*8);source=&c.rbx;break;
        case 4:address=reinterpret_cast<int64_t*>(c.rax+c.rcx*8);source=&c.rdi;break;
        default:return false;
    }
    uint64_t delta=*source;int64_t old=0;
    if(!data.pass->Add(address,int64_t(delta),old))return false;
    c.flags=AddFlags(c.flags,uint64_t(old),delta);*source=uint64_t(old);return true;
}
inline bool Locate(const locate::Image& image,Locations& l,incomeaccumulator::Pass& pass,std::string& error){
    std::vector<uint8_t*> roots;
    for(const auto& root:atomicpatterns::roots){
        auto hits=root.parent<0?locate::FindInCode(image,root.pattern,4096):
            locate::FindAll(roots[size_t(root.parent)],roots[size_t(root.parent)]+root.extent,locate::Parse(root.pattern),4096);
        if(root.callee>=0){auto expected=roots[size_t(root.callee)];
            hits.erase(std::remove_if(hits.begin(),hits.end(),[&](uint8_t* hit){return incomeatomic::Relative(hit,root.operand)!=expected;}),hits.end());}
        if(hits.size()!=1){error="economic producer anchor was not unique";return false;}
        auto target=root.pdata?FunctionStart(image,hits[0]):incomeatomic::Relative(hits[0],root.operand);
        if(root.slot>=0){uintptr_t absolute=*reinterpret_cast<uintptr_t*>(target+root.slot);
            target=image.base+(absolute-image.pointerBase);}
        if(!target || !FunctionStart(image,target)){error="economic producer anchor did not resolve code";return false;}
        roots.push_back(target);
    }
    std::set<uint8_t*> distinct;
    for(size_t i=0;i<kCount;++i){const auto& site=atomicpatterns::sites[i];
        auto hits=site.root<0?locate::FindInCode(image,site.pattern,2):
            locate::FindAll(roots[size_t(site.root)],roots[size_t(site.root)]+site.extent,locate::Parse(site.pattern),2);
        if(hits.size()!=1){error="economic atomic site was not unique";return false;}
        auto at=hits[0]+site.offset;if(!distinct.insert(at).second){error="duplicate economic atomic site";return false;}
        const char* instructions[]={"F0 49 0F C1 08","F0 48 0F C1 02","F0 4C 0F C1 04 C8","F0 48 0F C1 1C C8","F0 48 0F C1 3C C8"};
        if(!locate::MatchAt(at,locate::Parse(instructions[site.operation]))){error="economic atomic operand mismatch";return false;}
        l.operands[i]={&pass,site.operation};auto& b=l.blocks[i];b.at=at;b.stolen=b.span=site.bytes;b.handler=&Add;b.data=&l.operands[i];
    }
    return true;
}
} // namespace incomeproducers
