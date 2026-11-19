#include "../src/interface/starbase/shipyardreinforce.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace shipyardreinforce;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "shipyard reinforcement check failed: %s\n", what);
        std::exit(1);
    }
}
// Builds SPerQueueCacheData-shaped entries: queue id, then three other fields.
std::vector<uint8_t> Entries(std::initializer_list<uint32_t> queues) {
    std::vector<uint8_t> out;
    uint32_t n = 0;
    for (uint32_t q : queues) {
        const uint32_t entry[4] = {q, 2, 10 + n++, 1};
        const auto* bytes = reinterpret_cast<const uint8_t*>(entry);
        out.insert(out.end(), bytes, bytes + sizeof(entry));
    }
    return out;
}
uint32_t QueueAt(const std::vector<uint8_t>& v, size_t i) {
    uint32_t q;
    memcpy(&q, v.data() + i * kQueueEntrySize, 4);
    return q;
}
int main() {
    Exclusions e;
    Check(e.Allowed(5, 1), "allowed by default");
    Check(e.Set(5, 1, false) && !e.Allowed(5, 1), "unchecked");
    Check(e.Allowed(5, 2), "another owner isn't bound by it");
    Check(e.Set(5, 1, true) && e.Allowed(5, 1) && e.bases.empty(), "checked again");
    Check(e.Set(5, 1, true) && e.bases.empty(), "checking an allowed base");
    Check(e.Set(9, 3, false) && e.Set(5, 1, false) && e.Set(5, 1, false) && e.bases.size() == 2, "set twice");

    // Round trip and transactional parsing.
    const auto text = e.Serialize();
    Check(text == "SEF_NOREINFORCE_1 2 5 1 9 3", "format");
    Exclusions f;
    Check(f.Deserialize(text) && f.bases == e.bases && f.Serialize() == text, "round trip");
    Check(f.Deserialize("SEF_NOREINFORCE_1 0") && f.bases.empty(), "empty");
    f = e;
    for (const char* bad : {"", "SEF_NOREINFORCE_2 0", "SEF_NOREINFORCE_1 1 5", "SEF_NOREINFORCE_1 2 5 1 5 2",
                            "SEF_NOREINFORCE_1 0 junk", "SEF_NOREINFORCE_1 99999", "SEF_NOREINFORCE_1 1 -5 1"})
        Check(!f.Deserialize(bad) && f.bases == e.bases, bad);

    // A full set refuses new bases but still takes changes to listed ones.
    Exclusions full;
    for (uint32_t i = 0; i < kMaxBases; ++i)
        full.bases[i] = 1;
    Check(!full.Set(kMaxBases, 1, false) && full.Set(3, 2, false) && full.Set(4, 1, true), "limit");

    // Filtering keeps order and whole entries.
    const auto queues = Entries({100, 200, 300, 200, 400});
    std::vector<uint8_t> out(queues.size());
    const uint32_t none[1] = {0};
    Check(KeepAllowed(queues.data(), 5, none, 0, out.data()) == 5 && out == queues, "nothing excluded");
    const uint32_t some[] = {200, 400, 999};
    Check(KeepAllowed(queues.data(), 5, some, 3, out.data()) == 2 && QueueAt(out, 0) == 100 && QueueAt(out, 1) == 300,
          "excluded queues dropped");
    Check(memcmp(out.data() + kQueueEntrySize, queues.data() + 2 * kQueueEntrySize, kQueueEntrySize) == 0,
          "entries copied whole");
    const uint32_t all[] = {100, 200, 300, 400};
    Check(KeepAllowed(queues.data(), 5, all, 4, out.data()) == 0, "all excluded");
    Check(KeepAllowed(queues.data(), 0, all, 4, out.data()) == 0, "empty list");

    // Labels by width; the tooltip names the setting and its state.
    Check(std::string(Label(400)) == "Allow automatic fleet reinforcement from this shipyard", "full label");
    Check(std::string(Label(166)) == "Auto-reinforce here" &&
              std::string(Label(221)) == "Allow automatic reinforcement",
          "short labels");
    Check(TooltipText(nullptr, true).find(": on\n") != std::string::npos &&
              TooltipText(nullptr, false).find(": off\n") != std::string::npos &&
              TooltipText("multiplayer", true).find("Not available: multiplayer") != std::string::npos,
          "tooltip");
    std::puts("shipyard reinforcement: PASS");
    return 0;
}
