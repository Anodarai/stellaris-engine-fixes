// Which shipyards automatic fleet reinforcement may build at. The game picks shipyards for the
// fleet's Reinforce button, the fleet manager's Reinforce All and their tooltips; the DLL hides
// the shipyards whose starbase is unchecked from that choice. Nothing here touches game memory.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

namespace shipyardreinforce {
inline constexpr size_t kMaxBases = 8192;
// CFleetTemplateManager::CalcBuildLocations' candidate list: SPerQueueCacheData (GOG PDB), 16 bytes,
// the construction queue's id first.
inline constexpr size_t kQueueEntrySize = 16;

// Starbases whose shipyard is left out, with the owner that unchecked it. Every other shipyard is
// allowed, so the default is on and a base that changes hands is allowed again.
struct Exclusions {
    std::map<uint32_t, uint32_t> bases; // starbase id -> owner

    bool Allowed(uint32_t id, uint32_t owner) const {
        auto it = bases.find(id);
        return it == bases.end() || it->second != owner;
    }
    bool Set(uint32_t id, uint32_t owner, bool allowed) {
        if (allowed) {
            bases.erase(id);
            return true;
        }
        if (!bases.count(id) && bases.size() >= kMaxBases)
            return false;
        bases[id] = owner;
        return true;
    }
    std::string Serialize() const {
        std::ostringstream out;
        out << "SEF_NOREINFORCE_1 " << bases.size();
        for (const auto& [id, owner] : bases)
            out << ' ' << id << ' ' << owner;
        return out.str();
    }
    // Transactional: on failure the current set is unchanged.
    bool Deserialize(const std::string& text) {
        // Unsigned extraction would take "-5" as 2^32 - 5.
        if (text.size() > 1024 * 1024 || text.find_first_of("-+") != std::string::npos)
            return false;
        std::istringstream in(text);
        std::string magic;
        size_t count;
        if (!(in >> magic >> count) || magic != "SEF_NOREINFORCE_1" || count > kMaxBases)
            return false;
        std::map<uint32_t, uint32_t> parsed;
        for (size_t i = 0; i < count; ++i) {
            uint32_t id, owner;
            if (!(in >> id >> owner) || !parsed.emplace(id, owner).second)
                return false;
        }
        in >> std::ws;
        if (!in.eof())
            return false;
        bases.swap(parsed);
        return true;
    }
};

// Copies the entries of `queues` whose queue id isn't in `excluded` to `out`, in order, and returns
// how many it kept. `out` has room for `count` entries.
inline size_t KeepAllowed(const uint8_t* queues, size_t count, const uint32_t* excluded, size_t excludedCount,
                          uint8_t* out) {
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t queue;
        memcpy(&queue, queues + i * kQueueEntrySize, sizeof(queue));
        bool skip = false;
        for (size_t j = 0; j < excludedCount && !skip; ++j)
            skip = excluded[j] == queue;
        if (skip)
            continue;
        memcpy(out + kept * kQueueEntrySize, queues + i * kQueueEntrySize, kQueueEntrySize);
        ++kept;
    }
    return kept;
}

// The checkbox's label for a label `width` pixels wide (cg_16b in vanilla is about 7 px a letter).
// The tooltip always has the full sentence.
inline const char* Label(int width) {
    if (width >= 380)
        return "Allow automatic fleet reinforcement from this shipyard";
    if (width >= 210)
        return "Allow automatic reinforcement";
    if (width >= 175)
        return "Allow auto-reinforcement";
    return "Auto-reinforce here";
}

// `unavailable` (why it can't be changed here) wins; otherwise `allowed` is the box's state.
inline std::string TooltipText(const char* unavailable, bool allowed) {
    std::string text = "Allow automatic fleet reinforcement from this shipyard";
    if (unavailable)
        return text + "\nNot available: " + unavailable;
    if (allowed)
        return text + ": on\n"
                      "Reinforce and Reinforce All may build ships here.\n"
                      "Click to keep them away from this shipyard.";
    return text + ": off\n"
                  "Reinforce and Reinforce All build at other shipyards, or say none is available.\n"
                  "Ships you queue here yourself are not affected.\n"
                  "Click to allow it again.";
}
} // namespace shipyardreinforce
