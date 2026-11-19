// Part of shipdesignlibrary_runtime.inl: loading a saved design. Looks up the hull, section and
// component templates by key, starts a new design of the hull the way the hull picker does, and
// places what fits through the designer; DesignPopup lists what couldn't be placed.

// --- Lookups by key ----------------------------------------------------------------------------
uint8_t* FindShipSize(const std::string& key) {
    auto* db = ReadField<uint8_t*>(g_designLib.shipSizes);
    if (!db)
        return nullptr;
    int n = 0;
    auto** sizes = DesignArray<uint8_t*>(db, sdl::kShipSizeDbData, sdl::kShipSizeDbCount, n, 100000);
    for (int i = 0; i < n; ++i) {
        auto* size = ReadField<uint8_t*>(sizes, size_t(i) * 8);
        if (size && DesignKeyIs(size, sdl::kShipSizeKey, key))
            return size;
    }
    return nullptr;
}
uint8_t* FindTemplateIn(uint8_t* array, size_t keyOffset, const std::string& key) {
    int n = 0;
    auto** items = DesignArray<uint8_t*>(array, 8, 0x14, n, 100000);
    for (int i = 0; i < n; ++i) {
        auto* item = ReadField<uint8_t*>(items, size_t(i) * 8);
        if (item && DesignKeyIs(item, keyOffset, key))
            return item;
    }
    return nullptr;
}
uint8_t* FindSectionTemplate(const std::string& key) {
    auto* db = ReadField<uint8_t*>(g_designLib.templates);
    return db ? FindTemplateIn(db + sdl::kTemplatesSections, sdl::kSectionKey, key) : nullptr;
}
// Component templates are listed by component set and by kind (weapon, utility, strike craft).
uint8_t* FindComponentTemplate(const std::string& key) {
    auto* db = ReadField<uint8_t*>(g_designLib.templates);
    if (!db)
        return nullptr;
    int n = 0;
    auto** sets = DesignArray<uint8_t*>(db + sdl::kTemplatesSets, 8, 0x14, n, 100000);
    for (int i = 0; i < n; ++i) {
        auto* set = ReadField<uint8_t*>(sets, size_t(i) * 8);
        if (!set)
            continue;
        if (auto* found = FindTemplateIn(set + sdl::kSetComponents - 8, sdl::kComponentKey, key))
            return found;
    }
    for (size_t array : sdl::kTemplatesComponentArrays)
        if (auto* found = FindTemplateIn(db + array, sdl::kComponentKey, key))
            return found;
    return nullptr;
}

// --- Loading -------------------------------------------------------------------------------------
// The hull picker's new design: an empty CShipDesign of `size` in the designer's graphical culture,
// with the saved name and role, loaded as a new design. LoadDesign then shows the role picker; a
// role chosen there generates a whole design for it, so it is hidden again.
bool StartNewDesign(uint8_t* designer, uint8_t* size, const sdl::Design& d, std::string& error) {
    alignas(16) uint8_t design[0x240] = {};
    reinterpret_cast<void* (*)(void*)>(g_designLib.designCtor)(design);
    *reinterpret_cast<uint8_t**>(design + sdl::kDesignGfxCulture) =
        ReadField<uint8_t*>(designer, sdl::kDesignerGfxCulture);
    StarbaseSetGameString(reinterpret_cast<game::String*>(design + sdl::kDesignNameKey), d.name);
    design[sdl::kDesignNameLiteral] = 1;
    StarbaseSetGameString(reinterpret_cast<game::String*>(design + sdl::kDesignRole), d.role);
    reinterpret_cast<void (*)(void*, const void*)>(g_designLib.createGrowthStages)(design, size);
    const int stages = *reinterpret_cast<int*>(design + sdl::kDesignStageCount);
    const bool ok = stages == 1 && *reinterpret_cast<int32_t*>(design + sdl::kDesignId) == -1;
    if (ok) {
        auto* idler = *g_designLib.idler;
        const int owner = idler && *reinterpret_cast<int*>(idler + sdl::kIdlerAllComponents) != 0
                              ? 3
                              : ReadField<int>(designer, sdl::kDesignerOwner);
        reinterpret_cast<void (*)(void*, const void*, int, bool)>(g_designLib.loadDesign)(designer, design, owner,
                                                                                          false);
        for (size_t at = sdl::kDesignerViewsBegin; at < sdl::kDesignerViewsEnd; at += 8)
            if (auto* view = ReadField<uint8_t*>(designer, at))
                uinavigation::Method<void (*)(uint8_t*)>(view, sdl::kViewHideSlot)(view);
    } else {
        error = "\"" + d.size + "\" hulls have growth stages; such designs can't be loaded";
    }
    reinterpret_cast<void (*)(void*)>(g_designLib.designDtor)(design);
    if (ok)
        reinterpret_cast<void (*)(void*, bool)>(g_designLib.rolePicker)(designer, false);
    return ok;
}

// The ship size's section slot whose tag is `slot`: its SSectionSlotID (tag hash at +0x30).
uint8_t* FindSectionSlot(uint8_t* size, const std::string& slot) {
    int n = 0;
    auto* slots = DesignArray<uint8_t>(size, sdl::kShipSizeSlots, sdl::kShipSizeSlotCount, n, 256);
    for (int i = 0; i < n; ++i) {
        auto* id = slots + size_t(i) * sdl::kShipSizeSlotStride;
        if (DesignKeyIs(id, 0, slot))
            return id;
    }
    return nullptr;
}
// The stage's section on the slot with this tag hash.
uint8_t* FindStageSection(uint8_t* stage, uint32_t hash) {
    int n = 0;
    auto** sections = DesignArray<uint8_t*>(stage, sdl::kStageSections, sdl::kStageSectionCount, n, sdl::kMaxSections);
    for (int i = 0; i < n; ++i) {
        auto* section = ReadField<uint8_t*>(sections, size_t(i) * 8);
        if (section && ReadField<uint32_t>(section, sdl::kSectionSlotHash) == hash)
            return section;
    }
    return nullptr;
}
// CSectionTemplate::FitsOnSlot and CompatibleWithSize, as the designer's section list checks them.
bool SectionFits(uint8_t* type, uint8_t* slotId, uint8_t* size) {
    const uint32_t hash = ReadField<uint32_t>(slotId, sdl::kSlotIdHash);
    int fits = 0, sizes = 0;
    auto* slots = DesignArray<uint8_t>(type, sdl::kSectionFits, sdl::kSectionFitCount, fits, 1024);
    bool slotOk = false;
    for (int i = 0; i < fits && !slotOk; ++i)
        slotOk = ReadField<uint32_t>(slots, size_t(i) * sdl::kSlotIdSize + sdl::kSlotIdHash) == hash;
    if (!slotOk)
        return false;
    auto** compatible = DesignArray<uint8_t*>(type, sdl::kSectionSizes, sdl::kSectionSizeCount, sizes, 4096);
    for (int i = 0; i < sizes; ++i)
        if (ReadField<uint8_t*>(compatible, size_t(i) * 8) == size)
            return true;
    return ReadField<uint8_t>(type, sdl::kSectionAllSizes) != 0;
}
uint8_t* FindComponentSlot(uint8_t* sectionType, const std::string& key) {
    int n = 0;
    auto* slots = DesignArray<uint8_t>(sectionType, sdl::kSectionSlots, sdl::kSectionSlotCount, n, 1024);
    for (int i = 0; i < n; ++i) {
        auto* slot = slots + size_t(i) * sdl::kComponentSlotSize;
        if (DesignKeyIs(slot, sdl::kComponentSlotKey, key))
            return slot;
    }
    return nullptr;
}

// Loads `d` into the designer. Returns the problems for the player (empty: all placed).
sdl::Report LoadIntoDesigner(uint8_t* designer, const sdl::Design& d, std::string& fatal) {
    auto* size = FindShipSize(d.size);
    if (!size) {
        fatal =
            "The hull size \"" + d.size + "\" no longer exists (removed by a game update or mod?); nothing was loaded.";
        return {};
    }
    if (!StartNewDesign(designer, size, d, fatal))
        return {};
    auto* stage = DesignerStage(designer);
    if (!stage || ReadField<uint8_t*>(stage, sdl::kStageShipSize) != size) {
        fatal = "The designer did not start the new design.";
        return {};
    }
    const auto& selection = g_starbaseTemplateReader.selection;
    auto* state = ReadField<uint8_t*>(selection.gameState);
    auto* country = state ? starbasetemplates::CallOwner(selection.observedCountry, state) : nullptr;
    const int owner = ReadField<int>(designer, sdl::kDesignerOwner);
    // The game skips its availability checks while this idler flag is set (ComponentIsAllowedOnSlot).
    auto* idler = ReadField<uint8_t*>(g_designLib.idler);
    const bool everything = idler && ReadField<int>(idler, sdl::kIdlerAllComponents) != 0;
    using sdl::Placed;
    auto reasoned = [](bool ok, game::String& reason) {
        auto text = TakeGameString(reason);
        return ok ? sdl::Result{} : sdl::Result{text.empty() ? Placed::Unfit : Placed::Locked, text};
    };
    sdl::Placer placer;
    placer.section = [&](const sdl::Section& s) -> sdl::Result {
        auto* slot = FindSectionSlot(size, s.slot);
        if (!slot)
            return {Placed::NoSlot, ""};
        auto* type = FindSectionTemplate(s.section);
        if (!type)
            return {Placed::Missing, ""};
        if (!SectionFits(type, slot, size))
            return {Placed::Unfit, ""};
        if (country && !everything &&
            !reinterpret_cast<bool (*)(void*, void*)>(g_designLib.sectionPrereqs)(type, country))
            return {Placed::Locked, "your empire lacks the technology for it"};
        reinterpret_cast<void (*)(void*, void*, void*, void*)>(g_designLib.setSection)(designer, stage, type, slot);
        return {};
    };
    placer.component = [&](const sdl::Section& s, const sdl::Component& c) -> sdl::Result {
        auto* slotId = FindSectionSlot(size, s.slot);
        auto* section = slotId ? FindStageSection(stage, ReadField<uint32_t>(slotId, sdl::kSlotIdHash)) : nullptr;
        auto* type = section ? ReadField<uint8_t*>(section, sdl::kSectionTemplate) : nullptr;
        if (!type)
            return {Placed::NoSlot, ""};
        auto* slot = FindComponentSlot(type, c.slot);
        if (!slot)
            return {Placed::NoSlot, ""};
        auto* component = FindComponentTemplate(c.component);
        if (!component)
            return {Placed::Missing, ""};
        game::String reason{};
        reason.cap = 0xF;
        const bool ok = reinterpret_cast<bool (*)(void*, void*, void*, game::String*)>(g_designLib.allowed)(
            designer, component, slot, &reason);
        auto result = reasoned(ok, reason);
        if (ok)
            reinterpret_cast<void (*)(void*, void*, void*)>(g_designLib.sectionSetComponent)(section, component, slot);
        return result;
    };
    placer.required = [&](const sdl::Required& r) -> sdl::Result {
        int sets = 0, chosen = 0;
        auto** setList = DesignArray<uint8_t*>(size, sdl::kShipSizeRequiredSets, sdl::kShipSizeRequiredSetCount, sets,
                                               sdl::kMaxRequired);
        auto** current =
            DesignArray<uint8_t*>(stage, sdl::kStageRequired, sdl::kStageRequiredCount, chosen, sdl::kMaxRequired);
        for (int i = 0; i < sets && i < chosen; ++i) {
            auto* set = ReadField<uint8_t*>(setList, size_t(i) * 8);
            if (!set || !DesignKeyIs(set, sdl::kSetKey, r.set))
                continue;
            auto* component = FindTemplateIn(set + sdl::kSetComponents - 8, sdl::kComponentKey, r.component);
            if (!component)
                return {Placed::Missing, ""};
            game::String reason{};
            reason.cap = 0xF;
            const bool ok = reinterpret_cast<bool (*)(void*, int, void*, void*, game::String*)>(g_designLib.canBuild)(
                component, owner, stage, nullptr, &reason);
            auto result = reasoned(ok, reason);
            if (ok)
                *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(current) + size_t(i) * 8) = component;
            return result;
        }
        return {Placed::NoSlot, ""};
    };
    auto report = sdl::Apply(d, placer);
    // What the designer's own setters do after a change; outdated segments: weapons, components and
    // required components (2|4|8; sections set 6 themselves).
    reinterpret_cast<void (*)(void*)>(g_designLib.updateResources)(stage);
    reinterpret_cast<void (*)(void*)>(g_designLib.recalc)(designer + sdl::kDesignerData);
    designer[sdl::kDesignerOutdated] |= 0x0e;
    return report;
}

void DesignPopup(const std::string& title, const std::string& text) {
    auto* memory = reinterpret_cast<void* (*)(size_t)>(g_starbaseTemplateReader.construction.allocate)(sdl::kPopupSize);
    if (!memory)
        return;
    memset(memory, 0, sdl::kPopupSize);
    auto t = uinavigation::Text(title), body = uinavigation::Text(text);
    auto* popup = reinterpret_cast<void* (*)(void*, const game::String*, const game::String*)>(g_designLib.popupOk)(
        memory, &t, &body);
    reinterpret_cast<int (*)(void*, void*)>(g_starbaseDialogUi.addUpdateable)(*g_starbaseDialogUi.idler, popup);
}
