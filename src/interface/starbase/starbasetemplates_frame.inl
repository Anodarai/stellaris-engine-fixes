// Part of starbasetemplates_runtime.inl: the frame pass, from CheckTick's hook. At most every 100 ms
// it runs auto-upgrade and steps the staged construction of applied templates.

void StarbaseTemplateFrameImpl() {
    if (!g_starbaseTemplatesOn || StoreMultiplayer())
        return;
    const auto now = GetTickCount64();
    if (now - g_starbaseLastPass < 100)
        return;
    g_starbaseLastPass = now;
    devstats::Scope perfPass(g_perfFramePass);
    const uint32_t local = StarbaseLocalCountry();
    if (local == UINT32_MAX)
        return;
    std::lock_guard<std::mutex> lock(g_starbaseTemplateLock);
    StarbaseAutoUpgradePass(local);
    ReinforcePrune();
    auto& controller = g_starbaseTemplates;
    // Stop does not revoke posted commands. Reconcile them even after removing a job.
    for (auto p = controller.inFlight.begin(); p != controller.inFlight.end();) {
        if (controller.jobs.count(p->first)) {
            ++p;
            continue;
        }
        auto state = g_starbaseTemplateReader.Snapshot(p->first, p->second.type);
        if (!state.valid || starbasetemplates::Contains(state.queue, p->second) ||
            starbasetemplates::Controller::Built(state, p->second)) {
            g_starbasePostedAt.erase(p->first);
            p = controller.inFlight.erase(p);
        } else
            ++p;
    }
    if (controller.jobs.empty())
        return;
    devstats::Scope perfJob(g_perfJobStep);
    auto job = controller.jobs.upper_bound(g_starbaseLastId);
    if (job == controller.jobs.end())
        job = controller.jobs.begin();
    const uint32_t id = job->first;
    g_starbaseLastId = id;
    auto state = g_starbaseTemplateReader.Snapshot(id, job->second.target.level);
    if (!state.valid || state.owner != job->second.owner || state.owner != local) {
        job->second.paused = true;
        job->second.reason = "Starbase unavailable or ownership changed";
        return;
    }
    if (!StarbaseSupported(state)) {
        job->second.paused = true;
        job->second.reason = "Unsupported starbase type";
        return;
    }
    controller.Observe(state);
    if (controller.inFlight.count(id)) {
        auto [it, inserted] = g_starbasePostedAt.emplace(id, now);
        if (!inserted && now - it->second > 10000) {
            // Never retry an unacknowledged order: it might still be in the session.
            job->second.paused = true;
            job->second.reason = "Order was not acknowledged; check construction queue";
        }
        return;
    }
    g_starbasePostedAt.erase(id);
    if (job->second.paused)
        return;
    if (state.occupied)
        return;
    const auto& c = g_starbaseTemplateReader.construction;
    starbasetemplates::CommandApi api{reinterpret_cast<void* (*)(size_t)>(c.allocate),
                                      reinterpret_cast<starbasetemplates::CommandApi::Component>(c.module),
                                      reinterpret_cast<starbasetemplates::CommandApi::Component>(c.building),
                                      reinterpret_cast<starbasetemplates::CommandApi::Upgrade>(c.upgrade),
                                      reinterpret_cast<void (*)(void*, bool)>(c.post)};
    auto submit = [&](const starbasetemplates::Order& order, bool post) {
        auto* base = starbasetemplates::Reference(g_starbaseTemplateReader.selection.starbaseDatabase, id,
                                                  g_starbaseTemplateReader.selection.objectReference);
        auto* definition = g_starbaseTemplateReader.FindDefinition(order.kind, order.type);
        return StarbaseSubmitGuarded(api, local, base, order, definition, post);
    };
    controller.Tick(
        state,
        [&](const starbasetemplates::Order& order, std::string& reason) {
            // The stored component arrays can omit empty trailing slots. Their lengths
            // are not unlocked capacities. Native IsValid/CanSet checks the actual
            // current capacity, including level/modifier changes and slot restrictions.
            reason = "Waiting for resources or construction requirements";
            return submit(order, false) == starbasetemplates::Submission::Valid;
        },
        [&](const starbasetemplates::Order& order) {
            return submit(order, true) == starbasetemplates::Submission::Posted;
        });
}

void StarbaseTemplateFrame() {
    devstats::Scope perf(g_perfFrame);
    try {
        StarbaseTemplateFrameImpl();
    } catch (const std::exception& e) {
        g_starbaseTemplatesOn = false;
        Log(LogLevel::Error, "starbase_templates", "%s; disabled", e.what());
    } catch (...) {
        StarbaseDisable("unknown exception in the frame pass");
    }
}
