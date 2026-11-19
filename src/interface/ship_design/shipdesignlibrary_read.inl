// Part of shipdesignlibrary_runtime.inl: reading the design open in the ship designer into an
// sdl::Design (hull, sections, components and required components, by key). Every read is checked;
// a bad one throws, and the caller reports it.

// A CString field's text; empty for an empty string. Throws on a bad read or an implausible size.
std::string DesignKey(const uint8_t* object, size_t offset) {
    const auto s = ReadField<game::String>(object, offset);
    if (!s.size)
        return {};
    if (s.size > sdl::kMaxText || s.cap < s.size)
        throw std::runtime_error("invalid key string");
    std::string value(s.size, '\0');
    if (!CopyMemoryChecked(value.data(), s.data(), s.size))
        throw std::runtime_error("unreadable key string");
    return value;
}
// Whether a CString field is `key`. No allocation, and an oversized or unreadable string is no match.
bool DesignKeyIs(const uint8_t* object, size_t offset, const std::string& key) {
    const auto s = ReadField<game::String>(object, offset);
    char buffer[sdl::kMaxText];
    return s.size == key.size() && s.cap >= s.size && s.size <= sizeof(buffer) &&
           CopyMemoryChecked(buffer, s.data(), s.size) && memcmp(buffer, key.data(), s.size) == 0;
}
template <class T> T* DesignArray(const uint8_t* object, size_t data, size_t count, int& n, int limit) {
    n = ReadField<int>(object, count);
    if (n < 0 || n > limit)
        throw std::runtime_error("invalid array size");
    return ReadField<T*>(object, data);
}
std::string TakeGameString(game::String& s) {
    std::string text;
    if (s.size && s.size <= 4096) {
        text.assign(s.size, '\0');
        if (!CopyMemoryChecked(text.data(), s.data(), s.size))
            text.clear();
    }
    FreeGameString(s);
    return text;
}

// The design being edited and its shown growth stage.
uint8_t* DesignerDesign(uint8_t* designer) {
    auto* data = designer + sdl::kDesignerData;
    return uinavigation::Method<uint8_t* (*)(uint8_t*)>(data, 0x10)(data);
}
uint8_t* DesignerStage(uint8_t* designer) {
    const int index = ReadField<int>(designer, sdl::kDesignerStageIndex);
    return reinterpret_cast<uint8_t* (*)(uint8_t*, int)>(g_designLib.growthStage)(designer + sdl::kDesignerData, index);
}

// The open design as library data, or an error for the player.
bool ReadOpenDesign(uint8_t* designer, sdl::Design& d, std::string& error) {
    auto* design = DesignerDesign(designer);
    if (!design) {
        error = "No design open";
        return false;
    }
    const int stages = ReadField<int>(design, sdl::kDesignStageCount);
    if (stages != 1) {
        error = stages < 1 ? "No design open" : "Designs with growth stages can't be saved";
        return false;
    }
    auto* stage = DesignerStage(designer);
    auto* size = stage ? ReadField<uint8_t*>(stage, sdl::kStageShipSize) : nullptr;
    if (!size) {
        error = "No hull selected";
        return false;
    }
    d = {};
    d.size = DesignKey(size, sdl::kShipSizeKey);
    d.role = DesignKey(design, sdl::kDesignRole);
    game::String name{};
    name.cap = 0xF;
    reinterpret_cast<BuildStringFn>(g_str.buildString)(design + sdl::kDesignName, &name);
    d.name = sdl::Ellipsize(TakeGameString(name), sdl::kMaxName);
    int count = 0;
    auto** sections =
        DesignArray<uint8_t*>(stage, sdl::kStageSections, sdl::kStageSectionCount, count, sdl::kMaxSections);
    for (int i = 0; i < count; ++i) {
        auto* section = ReadField<uint8_t*>(sections, size_t(i) * 8);
        auto* type = section ? ReadField<uint8_t*>(section, sdl::kSectionTemplate) : nullptr;
        if (!type)
            continue;
        sdl::Section s;
        s.slot = DesignKey(section, sdl::kSectionSlotKey);
        s.section = DesignKey(type, sdl::kSectionKey);
        if (s.slot.empty() || s.section.empty())
            continue;
        int n = 0;
        auto* entries =
            DesignArray<uint8_t>(section, sdl::kSectionComponents, sdl::kSectionComponentCount, n, sdl::kMaxComponents);
        for (int c = 0; c < n; ++c) {
            auto* entry = entries + size_t(c) * sdl::kComponentEntrySize;
            auto* slot = ReadField<uint8_t*>(entry, sdl::kEntrySlot);
            auto* component = ReadField<uint8_t*>(entry, sdl::kEntryTemplate);
            if (!slot || !component)
                continue;
            sdl::Component part{DesignKey(slot, sdl::kComponentSlotKey), DesignKey(component, sdl::kComponentKey)};
            if (!part.slot.empty() && !part.component.empty())
                s.components.push_back(std::move(part));
        }
        d.sections.push_back(std::move(s));
    }
    int sets = 0, required = 0;
    auto** setList = DesignArray<uint8_t*>(size, sdl::kShipSizeRequiredSets, sdl::kShipSizeRequiredSetCount, sets,
                                           sdl::kMaxRequired);
    auto** chosen =
        DesignArray<uint8_t*>(stage, sdl::kStageRequired, sdl::kStageRequiredCount, required, sdl::kMaxRequired);
    for (int i = 0; i < sets && i < required; ++i) {
        auto* set = ReadField<uint8_t*>(setList, size_t(i) * 8);
        auto* component = ReadField<uint8_t*>(chosen, size_t(i) * 8);
        if (!set || !component)
            continue;
        sdl::Required r{DesignKey(set, sdl::kSetKey), DesignKey(component, sdl::kComponentKey)};
        if (!r.set.empty() && !r.component.empty())
            d.required.push_back(std::move(r)); // the null template has no key
    }
    if (!sdl::ValidName(d.name))
        d.name = "Design";
    return true;
}
