// Ship design library: designs saved by key (hull size, sections, components, required
// components) in the global sefdata store, and loaded back into the ship designer. Pure code; the
// DLL reads and writes the designer through native calls (shipdesignlibrary_runtime.inl).
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace shipdesignlibrary {
inline constexpr char kStoreKey[] = "sef.shipdesigns.v1";
inline constexpr char kHeader[] = "SEF_SHIPDESIGNS_1";
inline constexpr size_t kMaxDesigns = 4096, kMaxSections = 64, kMaxComponents = 128, kMaxRequired = 64;
inline constexpr size_t kMaxText = 256, kMaxName = 128;

struct Component {
    std::string slot, component;
};
struct Section {
    std::string slot, section;
    std::vector<Component> components;
};
// A required component (reactor, FTL, sensors, ...) by the ship size's component set.
struct Required {
    std::string set, component;
};
struct Design {
    std::string name; // as displayed; also the library key
    std::string size; // ship size key
    std::string role; // may be empty
    std::vector<Section> sections;
    std::vector<Required> required;
};
using Library = std::map<std::string, Design>;

// A design name typed by the player: 1-128 bytes, no control characters, not only spaces.
inline bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > kMaxName || name.find_first_not_of(' ') == std::string::npos)
        return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f)
            return false;
    return true;
}

inline bool Valid(const Design& d) {
    auto text = [](const std::string& s, bool empty) { return (empty || !s.empty()) && s.size() <= kMaxText; };
    if (!ValidName(d.name) || !text(d.size, false) || !text(d.role, true))
        return false;
    if (d.sections.size() > kMaxSections || d.required.size() > kMaxRequired)
        return false;
    for (const auto& s : d.sections) {
        if (!text(s.slot, false) || !text(s.section, false) || s.components.size() > kMaxComponents)
            return false;
        for (const auto& c : s.components)
            if (!text(c.slot, false) || !text(c.component, false))
                return false;
    }
    for (const auto& r : d.required)
        if (!text(r.set, false) || !text(r.component, false))
            return false;
    return true;
}

// Text tokens: bytes outside [A-Za-z0-9_.:-] become %XX; an empty string is "%".
inline std::string Escape(const std::string& s) {
    if (s.empty())
        return "%";
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                           c == '.' || c == ':' || c == '-';
        if (plain)
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
inline bool Unescape(const std::string& token, std::string& out) {
    out.clear();
    if (token == "%")
        return true;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '%') {
            out += token[i];
            continue;
        }
        if (i + 2 >= token.size())
            return false;
        const int hi = digit(token[i + 1]), lo = digit(token[i + 2]);
        if (hi < 0 || lo < 0)
            return false;
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    return out.size() <= kMaxText;
}

// SEF_SHIPDESIGNS_1 <designs>
// design <name> <size> <role> <sections> <required>, then per section
// section <slot> <section> <components>, then its components: component <slot> <component>,
// then the required components: required <set> <component>.
inline std::string Serialize(const Library& library) {
    std::ostringstream o;
    o << kHeader << ' ' << library.size() << '\n';
    for (const auto& [name, d] : library) {
        o << "design " << Escape(d.name) << ' ' << Escape(d.size) << ' ' << Escape(d.role) << ' ' << d.sections.size()
          << ' ' << d.required.size() << '\n';
        for (const auto& s : d.sections) {
            o << "section " << Escape(s.slot) << ' ' << Escape(s.section) << ' ' << s.components.size() << '\n';
            for (const auto& c : s.components)
                o << "component " << Escape(c.slot) << ' ' << Escape(c.component) << '\n';
        }
        for (const auto& r : d.required)
            o << "required " << Escape(r.set) << ' ' << Escape(r.component) << '\n';
    }
    return o.str();
}

// All or nothing: `out` is replaced only when the whole text is valid.
inline bool Deserialize(const std::string& text, Library& out) {
    std::istringstream in(text);
    std::string word;
    size_t count = 0;
    if (!(in >> word) || word != kHeader || !(in >> count) || count > kMaxDesigns)
        return false;
    auto token = [&](std::string& value) {
        std::string t;
        return in >> t && Unescape(t, value);
    };
    auto keyword = [&](const char* expected) {
        std::string t;
        return in >> t && t == expected;
    };
    auto number = [&](size_t& n, size_t limit) {
        long long v = -1;
        if (!(in >> v) || v < 0 || static_cast<size_t>(v) > limit)
            return false;
        n = static_cast<size_t>(v);
        return true;
    };
    Library library;
    for (size_t i = 0; i < count; ++i) {
        Design d;
        size_t sections = 0, required = 0;
        if (!keyword("design") || !token(d.name) || !token(d.size) || !token(d.role) ||
            !number(sections, kMaxSections) || !number(required, kMaxRequired))
            return false;
        for (size_t s = 0; s < sections; ++s) {
            Section section;
            size_t components = 0;
            if (!keyword("section") || !token(section.slot) || !token(section.section) ||
                !number(components, kMaxComponents))
                return false;
            for (size_t c = 0; c < components; ++c) {
                Component component;
                if (!keyword("component") || !token(component.slot) || !token(component.component))
                    return false;
                section.components.push_back(std::move(component));
            }
            d.sections.push_back(std::move(section));
        }
        for (size_t r = 0; r < required; ++r) {
            Required entry;
            if (!keyword("required") || !token(entry.set) || !token(entry.component))
                return false;
            d.required.push_back(std::move(entry));
        }
        if (!Valid(d) || library.count(d.name))
            return false;
        library.emplace(d.name, std::move(d));
    }
    if (in >> word)
        return false; // trailing data
    out.swap(library);
    return true;
}

// --- Loading ------------------------------------------------------------------------------------
// The DLL places each part through the designer, checking it first the way the designer does.
enum class Placed { Ok, Missing, NoSlot, Unfit, Locked };
struct Result {
    Placed placed = Placed::Ok;
    std::string reason; // the game's own explanation, when it gives one
};
struct Placer {
    std::function<Result(const Section&)> section; // sets the section on its slot
    std::function<Result(const Section&, const Component&)> component;
    std::function<Result(const Required&)> required;
};
struct Report {
    int placed = 0, skipped = 0;
    std::vector<std::string> problems;
};

inline std::string Describe(const Result& r, const std::string& what, const std::string& slot) {
    switch (r.placed) {
    case Placed::Missing:
        return what + " no longer exists (removed by a game update or mod?)";
    case Placed::NoSlot:
        return what + ": this hull has no slot " + slot + " any more";
    case Placed::Unfit:
        return what + " does not fit slot " + slot;
    case Placed::Locked:
        return what + ": " + (r.reason.empty() ? std::string("not available to your empire") : r.reason);
    default:
        return what;
    }
}

// Places every part it can; anything refused is skipped and reported. A section that can't be set
// takes its components with it (their slots belong to the section).
inline Report Apply(const Design& d, const Placer& placer) {
    Report report;
    for (const auto& s : d.sections) {
        const auto r = placer.section(s);
        if (r.placed != Placed::Ok) {
            report.problems.push_back(
                "Section " + Describe(r, s.section, s.slot) +
                (s.components.empty() ? std::string()
                                      : "; its " + std::to_string(s.components.size()) + " components were skipped"));
            report.skipped += 1 + static_cast<int>(s.components.size());
            continue;
        }
        ++report.placed;
        for (const auto& c : s.components) {
            const auto rc = placer.component(s, c);
            if (rc.placed == Placed::Ok) {
                ++report.placed;
                continue;
            }
            report.problems.push_back(Describe(rc, c.component, s.slot + "/" + c.slot));
            ++report.skipped;
        }
    }
    for (const auto& r : d.required) {
        const auto rr = placer.required(r);
        if (rr.placed == Placed::Ok) {
            ++report.placed;
            continue;
        }
        report.problems.push_back(Describe(rr, r.component, r.set));
        ++report.skipped;
    }
    return report;
}

// The message for the player: what was skipped, at most `maxLines` lines.
inline std::string ReportText(const std::string& name, const Report& report, size_t maxLines = 14) {
    std::string text = "\"" + name + "\" was loaded, but " + std::to_string(report.skipped) +
                       (report.skipped == 1 ? " part" : " parts") + " could not be added and were left empty:\n";
    for (size_t i = 0; i < report.problems.size() && i < maxLines; ++i)
        text += "\n- " + report.problems[i];
    if (report.problems.size() > maxLines)
        text += "\n- ... and " + std::to_string(report.problems.size() - maxLines) +
                " more (see stellaris-engine-fixes.log)";
    return text;
}

// --- Panel ---------------------------------------------------------------------------------------
struct Row {
    std::string name, label;
    bool selected = false;
};
struct Page {
    std::vector<Row> rows;
    int page = 0, pages = 1;
    std::string selected;
    std::string pageLabel;
};
// Shortens UTF-8 text to at most `limit` bytes without splitting a code point.
inline std::string Ellipsize(const std::string& text, size_t limit) {
    if (text.size() <= limit)
        return text;
    if (limit < 4)
        return std::string();
    size_t cut = limit - 3;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr(0, cut) + "...";
}
// Rows sorted by name, labelled with the hull size. `follow` turns to the selection's page.
inline Page BuildPage(const Library& library, const std::string& selected, int page, bool follow, int perPage,
                      size_t labelBytes) {
    Page view;
    perPage = (std::max)(1, perPage);
    const int count = static_cast<int>(library.size());
    view.pages = (std::max)(1, (count + perPage - 1) / perPage);
    if (library.count(selected))
        view.selected = selected;
    else if (!library.empty())
        view.selected = library.begin()->first;
    int index = 0;
    for (const auto& entry : library) {
        if (entry.first == view.selected)
            break;
        ++index;
    }
    view.page = follow && !view.selected.empty() ? index / perPage : std::clamp(page, 0, view.pages - 1);
    auto it = library.begin();
    std::advance(it, (std::min)(count, view.page * perPage));
    for (int i = 0; i < perPage && it != library.end(); ++i, ++it) {
        Row row;
        row.name = it->first;
        row.selected = it->first == view.selected;
        const std::string prefix = std::string(row.selected ? "> " : "") + "[" + it->second.size + "] ";
        row.label = prefix + Ellipsize(it->first, labelBytes > prefix.size() + 4 ? labelBytes - prefix.size() : 4);
        view.rows.push_back(std::move(row));
    }
    view.pageLabel = count == 0 ? std::string("No designs saved")
                                : "Page " + std::to_string(view.page + 1) + "/" + std::to_string(view.pages) + " - " +
                                      std::to_string(count) + (count == 1 ? " design" : " designs");
    return view;
}

// Controls of the library panel, in button order. Rows follow kRow0.
enum Control { kToggle, kSave, kLoad, kPrev, kPage, kNext, kDelete, kClose, kStatus, kRow0 };
inline constexpr int kMaxRows = 16, kControls = kRow0 + kMaxRows;
struct Rect {
    int x, y, w, h;
};
struct Geometry {
    int x = 445, y = 0; // the toggle's corner, inside the designer window
    int width = 320, rows = 12;
    size_t rowText = 38, statusText = 40;
};
inline constexpr int kPitch = 27, kButton = 24;
// Rows fill the designer's height below the toggle, between 4 and kMaxRows.
inline Geometry MakeGeometry(int x, int y, int windowHeight) {
    Geometry g;
    g.x = x;
    g.y = y;
    const int top = y + 30, bottom = (windowHeight >= 300 && windowHeight <= 4000 ? windowHeight : 642) - 8;
    const int fixed = 4 * kPitch; // save/load, pager, delete/close, status
    g.rows = std::clamp((bottom - top - fixed) / kPitch, 4, kMaxRows);
    g.rowText = static_cast<size_t>((g.width - 20) / 8);
    g.statusText = g.rowText;
    return g;
}
inline Rect Layout(int control, const Geometry& g) {
    const int x = g.x, w = g.width, half = (w - 4) / 2, h = kButton;
    const int top = g.y + 30, rows = top + 2 * kPitch, after = rows + g.rows * kPitch;
    switch (control) {
    case kToggle:
        return {x, g.y, 180, h};
    case kSave:
        return {x, top, half, h};
    case kLoad:
        return {x + w - half, top, half, h};
    case kPrev:
        return {x, top + kPitch, 50, h};
    case kPage:
        return {x + 54, top + kPitch, w - 108, h};
    case kNext:
        return {x + w - 50, top + kPitch, 50, h};
    case kDelete:
        return {x, after, half, h};
    case kClose:
        return {x + w - half, after, half, h};
    case kStatus:
        return {x, after + kPitch, w, h};
    default:
        return {x, rows + kPitch * (control - kRow0), w, h};
    }
}
} // namespace shipdesignlibrary
