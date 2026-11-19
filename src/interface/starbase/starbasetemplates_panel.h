// Pure layout and text for the template management panel. The DLL draws it inside the
// native starbase side view; nothing here touches game memory.
#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "starbasetemplates.h"

namespace starbasetemplates {
// Button indices in the side panel. Rows follow kPanelRow0; how many show depends on the
// side view's height. The editor and its picker (starbasetemplates_editor.h) reuse the same
// buttons with other labels.
enum PanelControl {
    kPanelSave,
    kPanelSaveGlobal,
    kPanelPrev,
    kPanelPage,
    kPanelNext,
    kPanelApply,
    kPanelDelete,
    kPanelStop,
    kPanelStatus,
    kPanelNew,
    kPanelEdit,
    kPanelCopyCode,  // the selected template, or the editor's draft, as a share code
    kPanelPasteCode, // a share code into the editor
    kPanelRow0
};
inline constexpr int kPanelMaxRows = 24;
inline constexpr int kPanelControls = kPanelRow0 + kPanelMaxRows;

struct PanelRect {
    int x, y, w, h;
};

// The side view's size decides the layout: 360x559 or 360x753 in vanilla, 450x871 with UI
// Overhaul Dynamic. Sizes outside a sane range fall back to the vanilla short layout.
struct PanelGeometry {
    int width = 360, height = 559;
    int rows = 11;                        // template list rows
    int slotRows = 9;                     // the editor's and picker's taller rows
    size_t rowText = 40, statusText = 46; // bytes that fit a row / the status line
};
inline constexpr int kPanelButtonHeight = 24, kPanelMargin = 10;
// The same row buttons at two sizes: the template list's rows are as high as the other buttons;
// the editor's and picker's are taller, so their icons (28 px) can be made out.
inline constexpr int kPanelRowHeight = kPanelButtonHeight, kPanelRowPitch = 27;
inline constexpr int kPanelSlotRowHeight = 30, kPanelSlotRowPitch = 33;
inline constexpr int kPanelTop = 52; // below the title text box and its header line
inline constexpr int kPanelSecondRow = kPanelTop + 30, kPanelThirdRow = kPanelTop + 60;
inline constexpr int kPanelFirstRow = kPanelTop + 92;
inline PanelGeometry MakeGeometry(int width, int height) {
    PanelGeometry g;
    if (width >= 240 && width <= 1600 && height >= 400 && height <= 3000) {
        g.width = width;
        g.height = height;
    }
    const int full = g.width - 2 * kPanelMargin;
    // Bottom block: pager, Apply/Delete, Stop, status, each 34 apart, 12 above the edge.
    const int pager = g.height - 12 - kPanelButtonHeight - 3 * 34;
    const int first = kPanelFirstRow;
    g.rows = std::clamp((pager - 8 - first + kPanelRowPitch - kPanelRowHeight) / kPanelRowPitch, 1, kPanelMaxRows);
    g.slotRows = std::clamp((pager - 8 - first + kPanelSlotRowPitch - kPanelSlotRowHeight) / kPanelSlotRowPitch, 1,
                            kPanelMaxRows);
    g.rowText = static_cast<size_t>((std::max)(12, (full - 24) / 8));
    g.statusText = g.rowText + 6;
    return g;
}
// `slotRows`: the rows at the editor's and picker's size.
inline PanelRect PanelLayout(int control, const PanelGeometry& g, bool slotRows = false) {
    const int left = kPanelMargin, full = g.width - 2 * kPanelMargin, half = (full - 4) / 2;
    const int right = left + full - half, h = kPanelButtonHeight;
    const int status = g.height - 12 - h, stop = status - 34, apply = stop - 34, pager = apply - 34;
    switch (control) {
    case kPanelSave:
        return {left, kPanelTop, half, h};
    case kPanelSaveGlobal:
        return {right, kPanelTop, half, h};
    case kPanelPrev:
        return {left, pager, 60, h};
    case kPanelPage:
        return {left + 64, pager, full - 128, h};
    case kPanelNext:
        return {left + full - 60, pager, 60, h};
    case kPanelApply:
        return {left, apply, half, h};
    case kPanelDelete:
        return {right, apply, half, h};
    case kPanelStop:
        return {left, stop, full, h};
    case kPanelStatus:
        return {left, status, full, h};
    case kPanelNew:
        return {left, kPanelSecondRow, half, h};
    case kPanelEdit:
        return {right, kPanelSecondRow, half, h};
    case kPanelCopyCode:
        return {left, kPanelThirdRow, half, h};
    case kPanelPasteCode:
        return {right, kPanelThirdRow, half, h};
    default:
        return {left, kPanelFirstRow + (slotRows ? kPanelSlotRowPitch : kPanelRowPitch) * (control - kPanelRow0), full,
                slotRows ? kPanelSlotRowHeight : kPanelRowHeight};
    }
}

// Shortens UTF-8 text to at most `limit` bytes without splitting a code point.
inline std::string Ellipsize(const std::string& text, size_t limit) {
    if (text.size() <= limit)
        return text;
    if (limit < 4)
        return text.substr(0, 0);
    size_t cut = limit - 3;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr(0, cut) + "...";
}

struct PanelRow {
    std::string name, label;
    bool selected = false;
};
struct PanelPageView {
    std::vector<PanelRow> rows; // At most `perPage`; fewer on the last page.
    int page = 0, pages = 1;
    std::string selected; // Library entry the selection resolved to; empty if none.
    std::string pageLabel;
};

// `library` is the merged library; `local` is the save-specific one (its names win and are
// marked [S]). `follow` moves the page to show the selection, as after saving or deleting.
inline PanelPageView BuildPage(const std::map<std::string, Layout>& library, const std::map<std::string, Layout>& local,
                               const std::string& selected, int page, bool follow, int perPage = 12,
                               size_t labelBytes = 40) {
    PanelPageView view;
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
        PanelRow row;
        row.name = it->first;
        row.selected = it->first == view.selected;
        row.label = std::string(row.selected ? "> " : "") + (local.count(it->first) ? "[S] " : "[G] ") +
                    Ellipsize(it->first, labelBytes > 6 ? labelBytes - 6 : labelBytes);
        view.rows.push_back(std::move(row));
    }
    view.pageLabel = count == 0 ? std::string("No templates saved")
                                : "Page " + std::to_string(view.page + 1) + "/" + std::to_string(view.pages) + " - " +
                                      std::to_string(count) + (count == 1 ? " template" : " templates");
    return view;
}

// Status line for the selected starbase's plan.
inline std::string PlanStatus(const Job* job, size_t limit = 46) {
    if (!job)
        return "No plan on this starbase";
    if (job->reason.empty())
        return job->paused ? "Plan paused" : "Plan active: queueing orders";
    return Ellipsize(std::string(job->paused ? "Paused: " : "Active: ") + job->reason, limit);
}
} // namespace starbasetemplates
