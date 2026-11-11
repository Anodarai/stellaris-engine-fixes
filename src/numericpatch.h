// Small reviewed arithmetic blocks, with a full register snapshot and native fallback.
// Only the entry's complete, position-independent instructions are displaced.
#pragma once
#include "locate.h"
#include <atomic>

namespace numericpatch {
inline thread_local uint64_t dispatchStack=0;
struct Context {
    uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rbx,rdx,rcx,rax,flags;
    uint64_t Rsp() const { return dispatchStack?dispatchStack:reinterpret_cast<uint64_t>(this+1); }
};
static_assert(sizeof(Context)==128,"stub snapshot ABI");
using Handler = bool(*)(Context&,const void*);
using ReplayHandler = bool(*)(Context&,const Context&,const void*);
struct Block {
    uint8_t* at = nullptr;
    size_t stolen = 0, span = 0;
    Handler handler = nullptr;
    ReplayHandler replay = nullptr;
    const void* data = nullptr;
    bool returns = false;
    // Optional reviewed RIP-relative disp32 in the displaced prologue.
    size_t ripOperand = 0;
    // Reviewed short conditional branch in a displaced entry (zero means none).
    size_t shortBranch = 0;
    std::atomic<bool> failed{false};
};
inline bool __fastcall Dispatch(Context* context, Block* block) {
    if (block->failed.load(std::memory_order_relaxed)) return false;
    auto previousStack=dispatchStack;
    dispatchStack=reinterpret_cast<uint64_t>(context+(block->replay?2:1));
    bool accepted=false;
    __try {
        Context result=*context;
        bool changed=block->replay ? block->replay(result,context[1],block->data) : block->handler(result,block->data);
        if(changed){*context=result;accepted=true;}
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { block->failed.store(true,std::memory_order_relaxed); }
    dispatchStack=previousStack;return accepted;
}
struct Code {
    std::vector<uint8_t> b;
    size_t relocation = 0;
    void Put(std::initializer_list<uint8_t> x) { b.insert(b.end(),x.begin(),x.end()); }
    void U64(uint64_t v) { auto p=reinterpret_cast<uint8_t*>(&v); b.insert(b.end(),p,p+8); }
    void Jump(const void* target) { Put({0xff,0x25,0,0,0,0}); U64(reinterpret_cast<uint64_t>(target)); }
    void Save() {
        Put({0x9c,0x50,0x51,0x52,0x53,0x55,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,
             0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57});
    }
    void Restore() {
        for (uint8_t i=0;i<6;++i) Put({0x0f,0x10,static_cast<uint8_t>(0x44+i*8),0x24,static_cast<uint8_t>(32+i*16)});
        Put({0x48,0x89,0xdc}); // mov rsp,rbx (the ABI preserves this snapshot pointer)
        Put({0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,
             0x5f,0x5e,0x5d,0x5b,0x5a,0x59,0x58,0x9d});
    }
};
inline Code Emit(Block& block) {
    Code c;
    c.Save();
    if(block.replay) {
        // The reviewed replay grammar contains only register arithmetic: no stack accesses,
        // calls, branches, memory operands, or flag reads before its first CMP.
        // PUSH does not alter registers/flags; replay can run with the saved input frame below it.
        c.b.insert(c.b.end(),block.at,block.at+block.span);
        c.Save();
    }
    c.Put({0x48,0x89,0xe3,0x48,0x83,0xe4,0xf0,0x48,0x81,0xec,0x80,0,0,0});
    for(uint8_t i=0;i<6;++i) c.Put({0x0f,0x11,static_cast<uint8_t>(0x44+i*8),0x24,static_cast<uint8_t>(32+i*16)});
    c.Put({0x48,0x89,0xd9,0x48,0xba}); c.U64(reinterpret_cast<uint64_t>(&block));
    c.Put({0x48,0xb8}); c.U64(reinterpret_cast<uint64_t>(&Dispatch));
    c.Put({0xff,0xd0});
    if(block.replay) {
        c.Restore();c.Put({0x48,0x8d,0xa4,0x24,0x80,0,0,0});c.Jump(block.at+block.span);return c;
    }
    c.Put({0x84,0xc0,0x0f,0x84});
    size_t branch=c.b.size(); c.Put({0,0,0,0});
    c.Restore(); if(block.returns) c.Put({0xc3}); else c.Jump(block.at+block.span);
    int32_t delta=static_cast<int32_t>(c.b.size()-branch-4); memcpy(c.b.data()+branch,&delta,4);
    c.Restore();
    if(block.ripOperand)c.relocation=c.b.size()+block.ripOperand;
    if(block.shortBranch){
        auto conditional=block.at+block.shortBranch;
        c.b.insert(c.b.end(),block.at,conditional);
        c.Put({static_cast<uint8_t>(conditional[0]^1),14});
        c.Jump(conditional+2+static_cast<int8_t>(conditional[1]));
        c.b.insert(c.b.end(),conditional+2,block.at+block.stolen);
    }else c.b.insert(c.b.end(),block.at,block.at+block.stolen);
    c.Jump(block.at+block.stolen);
    return c;
}
// Prepare all blocks before installing any entry jumps. Caller owns Block lifetime.
inline bool Prepare(Block& block, uint8_t* stub, size_t capacity, std::vector<uint8_t>& jump) {
    if(block.stolen<5 || block.span<block.stolen) return false;
    if(block.shortBranch && (block.ripOperand || block.shortBranch+2>block.stolen ||
        (block.at[block.shortBranch]&0xf0)!=0x70))return false;
    auto code=Emit(block);
    if(code.relocation){
        auto target=locate::RelTarget(block.at+block.ripOperand,block.at+block.ripOperand+4);
        intptr_t rel=reinterpret_cast<intptr_t>(target)-reinterpret_cast<intptr_t>(stub+code.relocation+4);
        if(rel<INT32_MIN || rel>INT32_MAX)return false;
        int32_t value=static_cast<int32_t>(rel);memcpy(code.b.data()+code.relocation,&value,4);
    }
    intptr_t delta=reinterpret_cast<intptr_t>(stub)-reinterpret_cast<intptr_t>(block.at+5);
    if(code.b.size()>capacity || delta<INT32_MIN || delta>INT32_MAX) return false;
    memcpy(stub,code.b.data(),code.b.size());
    FlushInstructionCache(GetCurrentProcess(),stub,code.b.size());
    jump.assign(block.stolen,0x90); jump[0]=0xe9;
    int32_t rel=static_cast<int32_t>(delta); memcpy(jump.data()+1,&rel,4);
    return true;
}
} // namespace numericpatch
