// Pure monthly schedule for automatic defense platform upgrades. The DLL decides which bases are
// due and posts the game's own upgrade command for them; nothing here touches game memory.
#pragma once
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace starbaseautoupgrade {
inline constexpr int64_t kMonthDays = 30; // game months are always 30 days
inline constexpr size_t kMaxBases = 8192;
// Bases run per frame pass (every 100 ms). Bases enabled while paused share a day, and each run is
// a native IsValid and a posted command, so a large schedule is spread over a few passes.
inline constexpr size_t kMaxPerPass = 16;

struct Entry {
    uint32_t owner = 0;
    int64_t nextDay = 0; // game day number of the next run
};

struct Schedule {
    std::map<uint32_t, Entry> bases;

    bool Enabled(uint32_t id) const { return bases.count(id) != 0; }
    const Entry* Find(uint32_t id) const {
        auto it = bases.find(id);
        return it == bases.end() ? nullptr : &it->second;
    }
    // The first run is on the day it is enabled, then the same day of every later month.
    bool Enable(uint32_t id, uint32_t owner, int64_t today) {
        if (today < 0 || (!bases.count(id) && bases.size() >= kMaxBases))
            return false;
        bases[id] = Entry{owner, today};
        return true;
    }
    bool Disable(uint32_t id) { return bases.erase(id) != 0; }
    // At most `limit` bases due on `today`, in id order; the rest stay due for the next call.
    // Each one's next run moves a whole number of months past `today`, so months skipped (a long
    // pause in the date, say) run once, not repeatedly, and a base taken a day late keeps its day.
    std::vector<uint32_t> TakeDue(int64_t today, size_t limit = SIZE_MAX) {
        std::vector<uint32_t> due;
        for (auto& [id, entry] : bases) {
            if (due.size() >= limit)
                break;
            if (entry.nextDay > today)
                continue;
            due.push_back(id);
            entry.nextDay += ((today - entry.nextDay) / kMonthDays + 1) * kMonthDays;
        }
        return due;
    }
    std::string Serialize() const {
        std::ostringstream out;
        out << "SEF_AUTOUPGRADE_1 " << bases.size();
        for (const auto& [id, entry] : bases)
            out << ' ' << id << ' ' << entry.owner << ' ' << entry.nextDay;
        return out.str();
    }
    // Transactional: on failure the current schedule is unchanged.
    bool Deserialize(const std::string& text) {
        if (text.size() > 1024 * 1024)
            return false;
        std::istringstream in(text);
        std::string magic;
        size_t count;
        if (!(in >> magic >> count) || magic != "SEF_AUTOUPGRADE_1" || count > kMaxBases)
            return false;
        std::map<uint32_t, Entry> parsed;
        for (size_t i = 0; i < count; ++i) {
            uint32_t id;
            Entry entry;
            if (!(in >> id >> entry.owner >> entry.nextDay) || entry.nextDay < 0 || !parsed.emplace(id, entry).second)
                return false;
        }
        in >> std::ws;
        if (!in.eof())
            return false;
        bases.swap(parsed);
        return true;
    }
};
// What the last run did, this session (runs aren't saved).
enum class LastRun { None, Queued, Refused };

// The toggle icon's tooltip. `unavailable` (the reason it can't be toggled here) wins; otherwise
// `entry` null means off, and `nextDate` is the entry's next run, formatted.
inline std::string TooltipText(const char* unavailable, const Entry* entry, const std::string& nextDate, LastRun last) {
    if (unavailable)
        return std::string("Auto-upgrade defense platforms\nNot available: ") + unavailable;
    if (!entry)
        return "Auto-upgrade defense platforms: off\n"
               "Click to upgrade this starbase's defense platforms today,\n"
               "then on the same day every month.";
    std::string text = "Auto-upgrade defense platforms: on\nNext run: " + nextDate + "\nLast run: ";
    text += last == LastRun::Queued ? "upgrades queued"
            : last == LastRun::Refused
                ? "nothing queued (no platform to upgrade,\nor not enough resources for all of them)"
                : "not this session";
    return text + "\nClick to turn off.";
}
} // namespace starbaseautoupgrade
