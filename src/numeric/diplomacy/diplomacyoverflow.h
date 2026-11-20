#pragma once
#include "../numericmath.h"
#include "../numericpatch.h"
#include "diplomacy_patterns.h"
namespace diplomacyoverflow {
using Register = uint64_t numericpatch::Context::*;
// What the game does with a product right after the multiplication, when that instruction is in
// the multiplication's block: the game branches to the instruction after it, so it cannot have a
// block of its own (the entry jump would cover the branch target).
enum class After {
    None,
    Negate, // NEG result
    AddTo   // ADD target,result
};
struct MultiplySpec {
    size_t function, offset, stolen, span;
    Register a, b, result, magic;
    const char* pattern;
    After after = After::None;
    Register target = nullptr;
};
enum class Kind {
    ModifierR9,
    ModifierR8,
    ModifierR9InPlace,
    BaseAdd,
    BaseNaval,
    BaseEconomic,
    BaseTotal,
    FinalBase,
    ModifierTotal,
    Envoys,
    BorrowStore,
    EconomicConversion
};
struct SimpleSpec {
    Kind kind;
    size_t function, offset, stolen, span;
    const char* pattern;
};
}
#include "diplomacy_multiply.h"
#include "diplomacy_simple.h"
namespace diplomacyoverflow {
constexpr size_t kMultiplyCount = sizeof(kMultiply) / sizeof(kMultiply[0]);
constexpr size_t kSimpleCount = sizeof(kSimple) / sizeof(kSimple[0]);
constexpr size_t kBlockCount = kMultiplyCount + kSimpleCount;
struct SimpleData {
    const SimpleSpec* spec;
    const int64_t* baseWeight;
};
struct Locations {
    uint8_t* functions[2]{};
    const int64_t* baseWeight = nullptr;
    numericpatch::Block blocks[kBlockCount];
    SimpleData simple[kSimpleCount];
};
// The product, then the instruction after it (`After`), applied to `out`; `before` holds the inputs.
inline void Product(const MultiplySpec& s, numericpatch::Context& out, const numericpatch::Context& before) {
    int64_t result =
        numeric::MulDiv(static_cast<int64_t>(before.*s.a), s.b ? static_cast<int64_t>(before.*s.b) : 1000000, 100000);
    out.*s.result = static_cast<uint64_t>(result);
    if (s.after == After::Negate)
        out.*s.result = static_cast<uint64_t>(numeric::Negate(result));
    else if (s.after == After::AddTo)
        out.*s.target = static_cast<uint64_t>(numeric::Add(static_cast<int64_t>(before.*s.target), result));
}
inline bool Multiply(numericpatch::Context& c, const void* data) {
    const auto& s = *static_cast<const MultiplySpec*>(data);
    numericpatch::Context before = c;
    if (s.magic)
        c.*s.magic = UINT64_C(0x29f16b11c6d1e109);
    Product(s, c, before);
    return true;
}
inline bool ReplayMultiply(numericpatch::Context& out, const numericpatch::Context& before, const void* data) {
    Product(*static_cast<const MultiplySpec*>(data), out, before);
    return true;
}
// The flags TEST value,value leaves: CF and OF clear, ZF, SF and PF from the value.
inline uint64_t TestFlags(uint64_t flags, uint64_t value) {
    flags &= ~uint64_t(0x8c5);
    if (value == 0)
        flags |= 0x40;
    if (value >> 63)
        flags |= 0x80;
    uint8_t low = static_cast<uint8_t>(value);
    low ^= low >> 4;
    low ^= low >> 2;
    low ^= low >> 1;
    if (!(low & 1))
        flags |= 0x04;
    return flags;
}
inline int64_t& Memory(uint64_t address) {
    return *reinterpret_cast<int64_t*>(address);
}
inline int64_t Stack(numericpatch::Context& c, int offset) {
    return Memory(c.rbp + offset);
}
inline bool Simple(numericpatch::Context& c, const void* data) {
    const auto& d = *static_cast<const SimpleData*>(data);
    switch (d.spec->kind) {
    case Kind::ModifierR9:
        c.r9 = numeric::Add(static_cast<int64_t>(c.rax), 100000);
        break;
    case Kind::ModifierR8:
        c.r8 = numeric::Add(static_cast<int64_t>(c.rax), 100000);
        break;
    case Kind::ModifierR9InPlace:
        c.r9 = numeric::Add(static_cast<int64_t>(c.r9), 100000);
        break;
    case Kind::BaseAdd:
        c.r10 = numeric::Add(Memory(c.r12), static_cast<int64_t>(c.rdx));
        Memory(c.r12) = static_cast<int64_t>(c.r10);
        break;
    case Kind::BaseNaval:
        c.rax = numeric::Add(static_cast<int64_t>(c.rcx), static_cast<int64_t>(c.r10));
        Memory(c.r12) = static_cast<int64_t>(c.rax);
        break;
    case Kind::BaseEconomic:
        c.r10 = numeric::Add(static_cast<int64_t>(c.r10), static_cast<int64_t>(c.rdx));
        Memory(c.r12) = static_cast<int64_t>(c.r10);
        break;
    case Kind::BaseTotal: {
        // Reassemble all six components in 128 bits, retaining cancellation even if an
        // intermediate cached subtotal exceeded int64. Component values are decimal raw.
        numeric::Sum sum;
        sum.Add(*d.baseWeight);
        sum.Add(Stack(c, -0x18));
        sum.Add(Stack(c, -8));
        sum.Add(Stack(c, 0));
        sum.Add(Stack(c, 0x18));
        sum.Add(static_cast<int64_t>(c.r13));
        c.rax = static_cast<uint64_t>(sum.Clamp());
        Memory(c.r12) = static_cast<int64_t>(c.rax);
        break;
    }
    case Kind::FinalBase:
        c.rcx = numeric::Add(Stack(c, 0xd8), static_cast<int64_t>(c.rdx));
        Memory(c.rdi) = static_cast<int64_t>(c.rcx);
        break;
    case Kind::ModifierTotal: {
        numeric::Sum sum;
        sum.Add(Stack(c, 0xc8));
        sum.Add(static_cast<int64_t>(c.r14));
        sum.Add(Stack(c, 0xd0));
        sum.Add(static_cast<int64_t>(c.r15));
        sum.Add(Memory(c.rax));
        sum.Add(static_cast<int64_t>(c.r13));
        sum.Add(static_cast<int64_t>(c.rbx));
        c.rcx = static_cast<uint64_t>(sum.Clamp());
        sum.Add(Stack(c, 0x58));
        sum.Add(100000);
        c.r8 = static_cast<uint64_t>(sum.Clamp());
        break;
    }
    case Kind::Envoys: {
        int64_t count = static_cast<int64_t>(static_cast<int32_t>(c.rax)) + static_cast<int32_t>(c.rcx);
        c.rax = static_cast<uint64_t>(count);
        c.rdi = static_cast<uint64_t>(count * 100000);
        break;
    }
    case Kind::BorrowStore:
        // ADD [R13],RBX; TEST R12,R12. The game also branches here, past the ADD RBX,RDX before
        // it (which is in the preceding multiplication's block).
        Memory(c.r13) = numeric::Add(Memory(c.r13), static_cast<int64_t>(c.rbx));
        c.flags = TestFlags(c.flags, c.r12);
        break;
    case Kind::EconomicConversion:
        c.rcx = static_cast<uint64_t>(numeric::BinaryToDecimal(static_cast<int64_t>(c.r8)));
        break;
    }
    return true;
}
inline bool Locate(const locate::Image& image, Locations& l, std::string& error) {
    l.functions[0] = locate::FindOne(image, "base diplomatic weight", numericpatterns::kBaseWeight, error);
    l.functions[1] = locate::FindOne(image, "final diplomatic weight", numericpatterns::kFinalWeight, error);
    if (!l.functions[0] || !l.functions[1])
        return false;
    l.baseWeight = reinterpret_cast<int64_t*>(locate::RelTarget(l.functions[0] + 0x36, l.functions[0] + 0x3a));
    for (size_t i = 0; i < kMultiplyCount; ++i) {
        const auto& s = kMultiply[i];
        auto& b = l.blocks[i];
        b.at = l.functions[s.function] + s.offset;
        b.stolen = s.stolen;
        b.span = s.span;
        b.handler = &Multiply;
        b.replay = &ReplayMultiply;
        b.data = &s;
        if (!locate::MatchAt(b.at, locate::Parse(s.pattern))) {
            error = "diplomatic multiply block mismatch";
            return false;
        }
    }
    for (size_t i = 0; i < kSimpleCount; ++i) {
        const auto& s = kSimple[i];
        auto& b = l.blocks[kMultiplyCount + i];
        l.simple[i] = {&s, l.baseWeight};
        b.at = l.functions[s.function] + s.offset;
        b.stolen = s.stolen;
        b.span = s.span;
        b.handler = &Simple;
        b.data = &l.simple[i];
        if (!locate::MatchAt(b.at, locate::Parse(s.pattern))) {
            error = "diplomatic aggregate block mismatch";
            return false;
        }
    }
    return true;
}
} // namespace diplomacyoverflow
