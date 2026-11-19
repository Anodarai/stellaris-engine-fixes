// Pure single-player planner. Native adapters must validate and post ordinary commands.
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace starbasetemplates {
enum class Kind { Upgrade, Module, Building };
struct Order {
    Kind kind = Kind::Upgrade;
    size_t slot = 0;
    std::string type;
    bool operator==(const Order& b) const { return kind == b.kind && slot == b.slot && type == b.type; }
};
struct Layout {
    std::string level;
    std::vector<std::string> modules, buildings;
};
struct State {
    uint32_t id = 0, owner = 0;
    bool valid = true, occupied = false, multiplayer = false;
    Layout built;
    // Adapter supplies current level followed by its reachable successors.
    std::vector<std::string> successors;
    // True only after resolving target and current definitions along the same chain.
    bool targetLevelSatisfied = false;
    std::vector<Order> queue;
};
struct Job {
    uint32_t id = 0, owner = 0;
    Layout target;
    bool paused = false;
    std::string reason;
    std::vector<Order> managed;
};
inline bool Valid(const Layout& l) {
    if (l.level.empty() || l.level.size() > 256 || l.modules.size() > 256 || l.buildings.size() > 256)
        return false;
    for (const auto* slots : {&l.modules, &l.buildings})
        for (const auto& s : *slots)
            if (s.size() > 256)
                return false;
    return true;
}
inline bool Contains(const std::vector<Order>& q, const Order& o) {
    for (const auto& x : q)
        if (x == o)
            return true;
    return false;
}
// Returns at most one order per pass. Caller acknowledges execution before another pass.
inline bool Next(Job& j, const State& s, const std::function<bool(const Order&, std::string&)>& legal, Order& out,
                 bool& complete) {
    complete = false;
    if (s.multiplayer || j.paused)
        return false;
    if (!s.valid || s.id != j.id || s.owner != j.owner) {
        j.paused = true;
        j.reason = "Starbase destroyed or ownership changed";
        return false;
    }
    if (s.occupied) {
        j.reason = "Waiting for occupation to end";
        return false;
    }
    j.reason.clear();
    std::vector<Order> candidates;
    bool pending = false;
    if (!s.targetLevelSatisfied && s.built.level != j.target.level) {
        pending = true;
        bool upgrading = false;
        for (const auto& q : s.queue)
            upgrading |= q.kind == Kind::Upgrade;
        bool reachable = false;
        for (const auto& level : s.successors)
            reachable |= level == j.target.level;
        if (!reachable || s.successors.size() < 2) {
            j.reason = "Target level is unreachable";
        } else if (!upgrading)
            candidates.push_back({Kind::Upgrade, 0, s.successors[1]});
    }
    for (Kind kind : {Kind::Module, Kind::Building}) {
        const auto& target = kind == Kind::Module ? j.target.modules : j.target.buildings;
        const auto& built = kind == Kind::Module ? s.built.modules : s.built.buildings;
        for (size_t i = 0; i < target.size(); ++i) {
            if (target[i].empty())
                continue;
            Order want{kind, i, target[i]};
            bool queued = false;
            for (const auto& q : s.queue)
                if (q.kind == kind && q.slot == i) {
                    if (!(q == want)) {
                        j.paused = true;
                        j.reason = "Conflicting manual construction";
                        return false;
                    }
                    queued = true;
                }
            if (queued) {
                pending = true;
                continue;
            }
            if (i < built.size() && built[i] == target[i])
                continue;
            pending = true;
            // The adapter must enforce actual current slot availability and all native rules.
            candidates.push_back(want);
        }
    }
    for (const auto& o : candidates) {
        std::string reason;
        if (legal(o, reason)) {
            out = o;
            return true;
        }
        if (j.reason.empty())
            j.reason = reason;
    }
    complete = !pending;
    if (pending && j.reason.empty())
        j.reason = "Waiting for construction";
    return false;
}
inline void WriteLayout(std::ostream& o, const Layout& l) {
    o << std::quoted(l.level) << ' ';
    for (const auto* slots : {&l.modules, &l.buildings}) {
        o << slots->size() << ' ';
        for (const auto& s : *slots)
            o << std::quoted(s) << ' ';
    }
}
inline bool ReadLayout(std::istream& in, Layout& l) {
    if (!(in >> std::quoted(l.level)))
        return false;
    for (auto* slots : {&l.modules, &l.buildings}) {
        size_t count;
        if (!(in >> count) || count > 256)
            return false;
        slots->resize(count);
        for (auto& s : *slots)
            if (!(in >> std::quoted(s)))
                return false;
    }
    return Valid(l);
}
struct Controller {
    std::map<std::string, Layout> library;
    std::map<uint32_t, Job> jobs;
    // Only one unacknowledged command per base. Never persisted: reconcile after loading.
    std::map<uint32_t, Order> inFlight;
    bool Save(const std::string& name, const State& s) {
        if (s.multiplayer || !s.valid || name.empty() || name.size() > 128 || !Valid(s.built))
            return false;
        Controller candidate = *this;
        candidate.library[name] = s.built;
        if (candidate.library.size() > 4096 || candidate.Serialize(true).size() > 1024 * 1024)
            return false;
        library.swap(candidate.library);
        return true;
    }
    bool Apply(const std::string& name, const State& s) {
        auto it = library.find(name);
        if (s.multiplayer || !s.valid || it == library.end() || inFlight.count(s.id))
            return false;
        Controller candidate = *this;
        candidate.jobs[s.id] = Job{s.id, s.owner, it->second, false, {}, {}};
        // Leave room for acknowledgement records as all planned slots are queued.
        if (candidate.jobs.size() > 4096 || candidate.Serialize(false).size() > 480 * 1024)
            return false;
        jobs.swap(candidate.jobs);
        return true;
    }
    // Active jobs hold their own copy of the layout, so removal never changes them.
    bool Remove(const std::string& name) { return library.erase(name) != 0; }
    void Stop(uint32_t id) { jobs.erase(id); } // Already posted/paid orders are preserved.
    void Cancelled(uint32_t id) {
        auto it = jobs.find(id);
        if (it != jobs.end()) {
            it->second.paused = true;
            it->second.reason = "Construction cancelled";
        }
    }
    void Acknowledge(uint32_t id) { inFlight.erase(id); }
    static bool Built(const State& s, const Order& o) {
        if (o.kind == Kind::Upgrade) {
            // An upgrade may already have been followed by another manual upgrade.
            return s.built.level == o.type ||
                   (s.targetLevelSatisfied &&
                    std::find(s.successors.begin(), s.successors.end(), o.type) == s.successors.end());
        }
        const auto& slots = o.kind == Kind::Module ? s.built.modules : s.built.buildings;
        return o.slot < slots.size() && slots[o.slot] == o.type;
    }
    void Observe(const State& s) {
        auto it = jobs.find(s.id);
        if (it == jobs.end())
            return;
        auto& orders = it->second.managed;
        for (auto p = orders.begin(); p != orders.end();) {
            auto flight = inFlight.find(s.id);
            const bool awaiting = flight != inFlight.end() && flight->second == *p;
            if (Built(s, *p)) {
                if (awaiting)
                    inFlight.erase(flight);
                p = orders.erase(p);
            } else if (Contains(s.queue, *p)) {
                if (awaiting)
                    inFlight.erase(flight);
                ++p;
            } else if (awaiting)
                ++p;
            else {
                Cancelled(s.id);
                ++p;
            }
        }
    }
    bool Tick(const State& s, const std::function<bool(const Order&, std::string&)>& legal,
              const std::function<bool(const Order&)>& post) {
        auto it = jobs.find(s.id);
        if (it == jobs.end() || inFlight.count(s.id))
            return false;
        Order order;
        bool complete;
        if (!Next(it->second, s, legal, order, complete)) {
            if (complete)
                jobs.erase(it);
            return false;
        }
        inFlight[s.id] = order;
        if (!post(order)) {
            inFlight.erase(s.id);
            it->second.reason = "Command rejected";
            return false;
        }
        it->second.managed.push_back(order);
        return true;
    }
    std::string Serialize(bool global) const {
        std::ostringstream out;
        out << "SEF_STARBASE_2 " << global << ' ' << (global ? library.size() : jobs.size()) << ' ';
        if (global)
            for (const auto& entry : library) {
                out << std::quoted(entry.first) << ' ';
                WriteLayout(out, entry.second);
            }
        else
            for (const auto& entry : jobs) {
                const auto& j = entry.second;
                out << j.id << ' ' << j.owner << ' ' << j.paused << ' ' << std::quoted(j.reason) << ' ';
                WriteLayout(out, j.target);
                out << j.managed.size() << ' ';
                for (const auto& o : j.managed)
                    out << int(o.kind) << ' ' << o.slot << ' ' << std::quoted(o.type) << ' ';
            }
        return out.str();
    }
    // Transactional parse. Caller supplies the payload from the successful-save sidecar.
    bool Deserialize(const std::string& text, bool global) {
        if (text.size() > 1024 * 1024)
            return false;
        std::istringstream in(text);
        std::string magic;
        int mode;
        size_t count;
        if (!(in >> magic >> mode >> count) || (magic != "SEF_STARBASE_1" && magic != "SEF_STARBASE_2") ||
            mode != int(global) || count > 4096)
            return false;
        Controller parsed;
        for (size_t i = 0; i < count; ++i) {
            if (global) {
                std::string name;
                Layout l;
                if (!(in >> std::quoted(name)) || name.empty() || name.size() > 128 || !ReadLayout(in, l) ||
                    !parsed.library.emplace(name, l).second)
                    return false;
            } else {
                Job j;
                int paused;
                if (!(in >> j.id >> j.owner >> paused >> std::quoted(j.reason)) || paused < 0 || paused > 1 ||
                    j.reason.size() > 4096 || !ReadLayout(in, j.target))
                    return false;
                j.paused = paused != 0;
                if (magic == "SEF_STARBASE_2") {
                    size_t n;
                    if (!(in >> n) || n > 1024)
                        return false;
                    for (size_t k = 0; k < n; ++k) {
                        int kind;
                        Order o;
                        if (!(in >> kind >> o.slot >> std::quoted(o.type)) || kind < 0 || kind > 2 || o.slot > 255 ||
                            o.type.empty() || o.type.size() > 256)
                            return false;
                        o.kind = Kind(kind);
                        j.managed.push_back(o);
                    }
                }
                if (!parsed.jobs.emplace(j.id, j).second)
                    return false;
            }
        }
        in >> std::ws;
        if (!in.eof())
            return false;
        if (global)
            library.swap(parsed.library);
        else {
            jobs.swap(parsed.jobs);
            inFlight.clear();
        }
        return true;
    }
};
} // namespace starbasetemplates
