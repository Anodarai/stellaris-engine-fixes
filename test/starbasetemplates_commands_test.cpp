#include "../src/interface/starbase/starbasetemplates_commands.h"
#include <cstdlib>
#include <cstdio>
using namespace starbasetemplates;
namespace {
int allocated = 0, destroyed = 0, posted = 0, lastKind = -1, lastSlot = -1;
bool allow = true;
uintptr_t table[9]{};
void Check(bool value) {
    if (!value)
        std::abort();
}
void* Alloc(size_t size) {
    Check(size == 48);
    ++allocated;
    return std::calloc(1, size);
}
void* Destroy(void* p, unsigned flags) {
    Check(flags == 1);
    ++destroyed;
    std::free(p);
    return nullptr;
}
bool CommandValid(void*, void* tooltip) {
    Check(!tooltip);
    return allow;
}
void* Construct(void* p, uint32_t country, void* base, const void* definition, int slot) {
    Check(country == 42 && base && definition);
    *static_cast<uintptr_t**>(p) = table;
    lastSlot = slot;
    return p;
}
void* Module(void* p, uint32_t c, void* b, const void* d, int s) {
    lastKind = 1;
    return Construct(p, c, b, d, s);
}
void* Building(void* p, uint32_t c, void* b, const void* d, int s) {
    lastKind = 2;
    return Construct(p, c, b, d, s);
}
void* Upgrade(void* p, uint32_t c, void* b, const void* d) {
    lastKind = 0;
    return Construct(p, c, b, d, 0);
}
void Post(void* p, bool validate) {
    Check(!validate);
    ++posted;
    Destroy(p, 1);
}
}
int main() {
    table[0] = reinterpret_cast<uintptr_t>(&Destroy);
    table[8] = reinterpret_cast<uintptr_t>(&CommandValid);
    CommandApi api{Alloc, Module, Building, Upgrade, Post};
    int base = 0, definition = 0;
    for (Kind kind : {Kind::Upgrade, Kind::Module, Kind::Building}) {
        Order order{kind, 2, "type"};
        Check(Submit(api, 42, &base, order, &definition, false) == Submission::Valid);
        Check(lastKind == static_cast<int>(kind));
        Check(lastSlot == (kind == Kind::Upgrade ? 0 : 2));
        Check(Submit(api, 42, &base, order, &definition, true) == Submission::Posted);
        allow = false;
        Check(Submit(api, 42, &base, order, &definition, true) == Submission::Invalid);
        allow = true;
    }
    Check(posted == 3 && allocated == 9 && destroyed == 9);
    Check(Submit(api, 42, &base, {Kind::Module, 256, "x"}, &definition, true) == Submission::Invalid);
    Check(allocated == 9);
    std::puts("starbasetemplates_commands_test: passed");
}
