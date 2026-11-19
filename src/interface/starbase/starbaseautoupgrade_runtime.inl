// Part of starbasetemplates_runtime.inl: defense platform auto-upgrade (starbaseautoupgrade.h). Posts
// the defenses tab's Upgrade command for each starbase that has it on, and gives the toggle icon its
// look and tooltip.

enum class PlatformUpgrade { Posted, Refused, Fault };
// The defenses tab's Upgrade button, as its native handler does it: the command queues an upgrade
// for every platform that can be upgraded and hasn't one queued. IsValid refuses it when there is
// none, or when the country can't afford all of them at once.
PlatformUpgrade PostPlatformUpgrade(uint32_t country, uint32_t starbase) {
    using L = starbasetemplates::PlatformUpgradeLocations;
    __try {
        auto* command = static_cast<uint8_t*>(
            reinterpret_cast<void* (*)(size_t)>(g_starbaseTemplateReader.construction.allocate)(L::kSize));
        if (!command)
            return PlatformUpgrade::Refused;
        memset(command, 0, L::kSize);
        *reinterpret_cast<uintptr_t**>(command) = g_platformUpgrade.vtable;
        *reinterpret_cast<uint32_t*>(command + 0x08) = 0xffffffff;
        *reinterpret_cast<uint32_t*>(command + 0x10) = 0xffff0000;
        *reinterpret_cast<uint32_t*>(command + L::kCountry) = country;
        *reinterpret_cast<uint32_t*>(command + L::kStarbase) = starbase;
        auto valid = reinterpret_cast<bool (*)(void*, void*)>(g_platformUpgrade.vtable[8]);
        if (!valid(command, nullptr)) {
            reinterpret_cast<void* (*)(void*, unsigned)>(g_platformUpgrade.vtable[0])(command, 1);
            return PlatformUpgrade::Refused;
        }
        // Posting consumes the command, as for the native button.
        reinterpret_cast<void (*)(void*, bool)>(g_starbaseTemplateReader.construction.post)(command, false);
        return PlatformUpgrade::Posted;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return PlatformUpgrade::Fault;
    }
}

// Caller holds the template lock. Runs up to kMaxPerPass of the bases due today, leaving the rest
// for the next pass; a base that was lost or destroyed leaves the schedule. One summary log line
// per pass: each line is flushed to disk.
void StarbaseAutoUpgradePass(uint32_t local) {
    if (!g_autoUpgradeOn || g_autoUpgrade.bases.empty())
        return;
    const int64_t today = StarbaseToday();
    if (today < 0)
        return;
    const auto due = g_autoUpgrade.TakeDue(today, starbaseautoupgrade::kMaxPerPass);
    if (due.empty())
        return;
    g_starbaseViews[0].panel.dirty = true; // the icon and tooltip show the last run
    std::string queued, refused;
    auto note = [](std::string& list, uint32_t id) {
        char text[12];
        sprintf_s(text, " %08X", id);
        list += text;
    };
    for (uint32_t id : due) {
        const auto* entry = g_autoUpgrade.Find(id);
        const uint32_t owner = StarbaseOwner(id);
        if (!entry || owner == UINT32_MAX || owner != entry->owner) {
            Log(LogLevel::Info, "starbase_autoupgrade", "starbase %08X lost or destroyed; auto-upgrade removed", id);
            g_autoUpgrade.Disable(id);
            g_autoUpgradeLast.erase(id);
            continue;
        }
        if (owner != local)
            continue; // playing another empire: keep the schedule, post nothing
        const auto result = PostPlatformUpgrade(local, id);
        if (result == PlatformUpgrade::Fault) {
            g_autoUpgradeOn = false;
            Log(LogLevel::Error, "starbase_autoupgrade", "native command fault; auto-upgrade off");
            return;
        }
        g_autoUpgradeLast[id] = result == PlatformUpgrade::Posted;
        note(result == PlatformUpgrade::Posted ? queued : refused, id);
    }
    if (!queued.empty() || !refused.empty())
        Log(LogLevel::Info, "starbase_autoupgrade", "upgrades queued:%s; nothing to upgrade or not affordable:%s",
            queued.empty() ? " none" : queued.c_str(), refused.empty() ? " none" : refused.c_str());
}

// The toggle icon's look for the selected starbase: dimmed when off, green when on, amber when on
// but the last run was refused (nothing to upgrade, or not affordable), red briefly after a click
// that couldn't change it (not your starbase, say). Set on every refresh: cheap, and survives
// anything the game does to the sprite.
void StarbaseAutoUpgradeLook(uint32_t id) {
    auto* button = g_autoUpgradeToggle.button;
    if (!button)
        return;
    const auto* entry = g_autoUpgradeOn ? g_autoUpgrade.Find(id) : nullptr;
    const auto last = g_autoUpgradeLast.find(id);
    const bool refused = GetTickCount64() - g_autoUpgradeRefusedAt < 1500;
    const bool skipped = entry && last != g_autoUpgradeLast.end() && !last->second;
    const float color[4] = {refused   ? 1.0f
                            : skipped ? 1.0f
                            : entry   ? 0.45f
                                      : 1.0f,
                            refused   ? 0.35f
                            : skipped ? 0.75f
                                      : 1.0f,
                            refused   ? 0.35f
                            : skipped ? 0.3f
                            : entry   ? 0.45f
                                      : 1.0f,
                            1.0f};
    reinterpret_cast<void (*)(uint8_t*, const float*)>(g_buttonSetModulate)(button, color);
    reinterpret_cast<void (*)(uint8_t*, float)>(g_buttonSetAlpha)(button, entry || refused ? 1.0f : 0.4f);
}

// Replaces a game CString's text. On Windows its heap buffer comes from the game's operator new
// and is freed by its operator delete (g_str.gameFree) unless kind is 1.
void StarbaseSetGameString(game::String* s, const std::string& text) {
    FreeGameString(*s);
    if (text.size() > 0xF) {
        auto* buffer = static_cast<char*>(
            reinterpret_cast<void* (*)(size_t)>(g_starbaseTemplateReader.construction.allocate)(text.size() + 1));
        if (!buffer)
            return;
        s->ptr = buffer;
        s->cap = text.size();
    }
    memcpy(s->cap > 0xF ? s->ptr : s->buf, text.c_str(), text.size() + 1);
    s->size = text.size();
}

bool StarbaseAutoUpgradeTooltipCpp(uint8_t* tooltip) {
    namespace au = starbaseautoupgrade;
    auto* view = g_autoUpgradeToggle.view;
    if (!view || !g_starbaseTemplatesOn)
        return false;
    const auto id = starbasetemplates::ReadField<uint32_t>(view, g_starbaseTemplateReader.selection.selectedReference);
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
        const uint32_t owner = StarbaseOwner(id);
        const char* unavailable = !g_autoUpgradeOn                  ? "turned off after an error (see the log)"
                                  : StoreMultiplayer()              ? "multiplayer"
                                  : owner == UINT32_MAX             ? "no starbase selected"
                                  : owner != StarbaseLocalCountry() ? "not your starbase"
                                                                    : nullptr;
        const auto* entry = g_autoUpgradeOn ? g_autoUpgrade.Find(id) : nullptr;
        const auto last = g_autoUpgradeLast.find(id);
        text = au::TooltipText(unavailable, entry,
                               entry ? FormatDate(static_cast<int32_t>(entry->nextDay * 24)) : std::string(),
                               last == g_autoUpgradeLast.end() ? au::LastRun::None
                               : last->second                  ? au::LastRun::Queued
                                                               : au::LastRun::Refused);
    }
    StarbaseSetGameString(reinterpret_cast<game::String*>(tooltip), text); // CToolTip::_Instant (+0)
    return true;
}
// CTooltipHandler: GetToolTip(const CGuiObject*, CToolTip&) and a scalar deleting destructor. The
// handler is a static; nothing owns it, so the destructor frees nothing.
bool StarbaseAutoUpgradeTooltipGuarded(uint8_t* tooltip) {
    try {
        return StarbaseAutoUpgradeTooltipCpp(tooltip);
    } catch (...) {
        return false;
    }
}
bool StarbaseAutoUpgradeTooltip(void*, const void*, uint8_t* tooltip) {
    __try {
        return StarbaseAutoUpgradeTooltipGuarded(tooltip);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* StarbaseTooltipDestroy(void* self, unsigned) {
    return self;
}
const void* const kAutoUpgradeTooltipVtable[] = {reinterpret_cast<const void*>(&StarbaseAutoUpgradeTooltip),
                                                 reinterpret_cast<const void*>(&StarbaseTooltipDestroy)};
struct {
    const void* const* vtable;
} g_autoUpgradeTooltip{kAutoUpgradeTooltipVtable};
