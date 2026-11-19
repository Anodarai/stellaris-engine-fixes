#pragma once
#include "starbasetemplates.h"
#include <cstddef>

namespace starbasetemplates {
// Windows x64 ABI confirmed against native UI call sites. Posting consumes the
// heap command even on rejection; validation failures remain ours to destroy.
struct CommandApi {
    using Component = void* (*)(void*, uint32_t, void*, const void*, int);
    using Upgrade = void* (*)(void*, uint32_t, void*, const void*);
    void* (*allocate)(size_t) = nullptr;
    Component module = nullptr, building = nullptr;
    Upgrade upgrade = nullptr;
    void (*post)(void*, bool) = nullptr;
};
enum class Submission { Invalid, Valid, Posted };
inline Submission Submit(const CommandApi& api, uint32_t country, void* starbase, const Order& order,
                         const void* definition, bool post) {
    if (!starbase || !definition || !api.allocate || !api.module || !api.building || !api.upgrade || !api.post ||
        order.slot > 255 || country == UINT32_MAX)
        return Submission::Invalid;
    auto* memory = api.allocate(0x30);
    if (!memory)
        return Submission::Invalid;
    void* command;
    if (order.kind == Kind::Upgrade)
        command = api.upgrade(memory, country, starbase, definition);
    else
        command = (order.kind == Kind::Module ? api.module : api.building)(memory, country, starbase, definition,
                                                                           static_cast<int>(order.slot));
    // Constructors initialize the full object or raise a native allocation exception.
    const auto* table = *reinterpret_cast<uintptr_t**>(command);
    auto destroy = reinterpret_cast<void* (*)(void*, unsigned)>(table[0]);
    auto valid = reinterpret_cast<bool (*)(void*, void*)>(table[8]);
    if (!valid(command, nullptr)) {
        destroy(command, 1);
        return Submission::Invalid;
    }
    if (!post) {
        destroy(command, 1);
        return Submission::Valid;
    }
    api.post(command, false);
    return Submission::Posted; // Submission, not proof of execution. Reconcile on the command pass.
}
}
