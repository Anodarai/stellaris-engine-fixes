#include "../src/interface/starbase/starbasetemplates.h"
#include <cstdio>
#include <cstdlib>
using namespace starbasetemplates;
void Check(bool ok) {
    if (!ok) {
        std::fprintf(stderr, "starbase template check failed\n");
        std::exit(1);
    }
}
int main() {
    Controller c;
    State source;
    source.id = 17;
    source.owner = 9;
    source.built = {"citadel", {"anchorage", "anchorage", "anchorage"}, {"logistics"}};
    Check(c.Save("Naval \"reserve\"", source));
    State s;
    s.id = 23;
    s.owner = 9;
    s.built.level = "outpost";
    s.successors = {"outpost", "starport", "starhold", "citadel"};
    Check(c.Apply("Naval \"reserve\"", s));
    Order posted;
    int posts = 0;
    auto post = [&](const Order& o) {
        posted = o;
        ++posts;
        return true;
    };
    bool affordable = true;
    auto legal = [&](const Order& o, std::string& reason) {
        if (!affordable) {
            reason = "Insufficient alloys";
            return false;
        }
        if (o.kind == Kind::Upgrade)
            return true;
        const auto& slots = o.kind == Kind::Module ? s.built.modules : s.built.buildings;
        if (o.slot >= slots.size()) {
            reason = "Slot locked";
            return false;
        }
        if (o.kind == Kind::Building && s.built.modules[0] != "anchorage")
            return false;
        return true;
    };
    Check(c.Tick(s, legal, post));
    Check(posted.type == "starport");
    Check(!c.Tick(s, legal, post) && posts == 1);
    s.queue.push_back(posted);
    c.Acknowledge(s.id);
    Check(!c.Tick(s, legal, post)); // Upgrade queued; all component slots still locked.
    Controller restored;
    Check(restored.Deserialize(c.Serialize(true), true));
    Check(restored.Deserialize(c.Serialize(false), false));
    Check(!restored.Tick(s, legal, post)); // Loaded queue prevents duplicate upgrade.
    Check(restored.jobs.at(23).target.modules.size() == 3);
    auto before = restored.Serialize(false);
    Check(!restored.Deserialize(before + "garbage", false));
    Check(restored.Serialize(false) == before);
    Check(!restored.Deserialize("SEF_STARBASE_1 0 99999999", false));
    s.queue.clear();
    s.built.level = "starport";
    s.built.modules.resize(2);
    s.built.buildings.resize(1);
    s.successors = {"starport", "starhold", "citadel"};
    affordable = false;
    Check(!c.Tick(s, legal, post));
    affordable = true;
    Check(c.Tick(s, legal, post) && posted.type == "starhold");
    c.Acknowledge(s.id);
    s.queue.push_back(posted);
    Check(c.Tick(s, legal, post) && posted.kind == Kind::Module && posted.slot == 0);
    c.Acknowledge(s.id);
    s.queue.push_back(posted);
    Check(c.Tick(s, legal, post) && posted.kind == Kind::Module && posted.slot == 1);
    c.Acknowledge(s.id);
    s.queue.push_back(posted);
    Check(!c.Tick(s, legal, post)); // Third slot locked; building prerequisite unmet.
    c.Cancelled(s.id);
    Check(c.jobs.at(s.id).paused);
    Check(!c.Tick(s, legal, post));
    c.Stop(s.id);
    Check(c.jobs.empty());
    Check(c.Apply("Naval \"reserve\"", s));
    s.queue = {{Kind::Module, 0, "shipyard"}};
    Check(!c.Tick(s, legal, post) && c.jobs.at(s.id).paused);
    s.queue.clear();
    Check(c.Apply("Naval \"reserve\"", s));
    s.owner = 10;
    Check(!c.Tick(s, legal, post) && c.jobs.at(s.id).paused);
    s.owner = 9;
    Check(c.Apply("Naval \"reserve\"", s));
    s.built = source.built;
    s.targetLevelSatisfied = true;
    Check(!c.Tick(s, legal, post) && c.jobs.empty());
    s.multiplayer = true;
    Check(!c.Save("MP", s) && !c.Apply("Naval \"reserve\"", s));
    // A queued older order must not acknowledge a different order just submitted.
    Controller tracked;
    State t;
    t.id = 70;
    t.owner = 9;
    t.built = {"citadel", {"", ""}, {}};
    t.successors = {"citadel"};
    t.targetLevelSatisfied = true;
    State blueprint = t;
    blueprint.built.modules = {"anchorage", "shipyard"};
    Check(tracked.Save("mixed", blueprint) && tracked.Apply("mixed", t));
    auto always = [](const Order&, std::string&) { return true; };
    Check(tracked.Tick(t, always, post));
    t.queue.push_back(posted);
    tracked.Observe(t);
    Check(tracked.inFlight.empty() && tracked.Tick(t, always, post));
    tracked.Observe(t);
    Check(tracked.inFlight.count(t.id) == 1 && !tracked.Tick(t, always, post));
    Controller loaded;
    Check(loaded.Deserialize(tracked.Serialize(false), false));
    loaded.Observe(t); // Second post had not reached the native queue when saved.
    Check(loaded.jobs.at(t.id).paused && !loaded.Tick(t, always, post));
    t.queue.push_back(posted);
    tracked.Observe(t);
    Check(tracked.inFlight.empty());
    Check(loaded.Deserialize(tracked.Serialize(false), false));
    loaded.Observe(t);
    Check(!loaded.jobs.at(t.id).paused && !loaded.Tick(t, always, post));
    t.queue.erase(t.queue.begin()); // Cancel an acknowledged template order.
    loaded.Observe(t);
    Check(loaded.jobs.at(t.id).paused && !loaded.Tick(t, always, post));
    t.built.modules = blueprint.built.modules;
    t.queue.clear();
    tracked.Observe(t);
    Check(tracked.jobs.at(t.id).managed.empty());
    Check(!tracked.Tick(t, always, post) && tracked.jobs.empty());
    // Live regression: an empty Citadel can store only its first empty building
    // slot (and no module entries), despite all six/four slots being unlocked.
    // Native validation, not these sparse arrays, determines available capacity.
    Controller sparse;
    State empty;
    empty.id = 1245;
    empty.owner = 0;
    empty.built = {"citadel", {}, {""}};
    empty.successors = {"citadel"};
    empty.targetLevelSatisfied = true;
    State defense = empty;
    defense.built.modules.assign(6, "hangar");
    defense.built.buildings = {"command", "disruption", "jammer", "quarters"};
    Check(sparse.Save("Defense", defense) && sparse.Apply("Defense", empty));
    auto nativeCapacity = [](const Order& o, std::string&) {
        return o.kind == Kind::Module ? o.slot < 6 : o.kind == Kind::Building && o.slot < 4;
    };
    for (int i = 0; i < 10; ++i) {
        Check(sparse.Tick(empty, nativeCapacity, post));
        Check(posted.kind == (i < 6 ? Kind::Module : Kind::Building));
        Check(posted.slot == size_t(i < 6 ? i : i - 6));
        empty.queue.push_back(posted);
        sparse.Observe(empty);
    }
    Check(!sparse.Tick(empty, nativeCapacity, post));
    empty.built = defense.built;
    empty.queue.clear();
    sparse.Observe(empty);
    Check(!sparse.Tick(empty, nativeCapacity, post) && sparse.jobs.empty());
    std::puts("starbasetemplates_test: passed");
}
