// UI boundary tests for column configuration, persistence and geometry. No
// window is needed: these are the values the native header/list receive.
#include "columns.h"
#include "list_layout.h"
#include "test_support.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using namespace litefetch;
using namespace litefetch::test;
using namespace litefetch::ui;

namespace {
void defaultsMatchDesignPitches()
{
    const auto settings = defaultColumnSettings();
    const auto visible = visibleColumns(settings);
    check(visible.size() == 6, "six default columns");
    check(visible.front() == ColumnId::Name && visible.back() == ColumnId::Status,
        "default column order is the Figma order");

    for (const int width : {640, 761, 762, 763, 900, 997, 998, 1000, 1300}) {
        for (const int dpi : {96, 120, 144, 168}) {
            const auto configured = columnWidths(settings, width, dpi);
            const auto design = listColumnWidths(width, dpi);
            check(configured.size() == design.size(), "same column count as the design layout");
            for (std::size_t index = 0; index < design.size(); ++index) {
                if (configured[index] != design[index])
                    throw std::runtime_error("default geometry diverged at width "
                        + std::to_string(width) + " dpi " + std::to_string(dpi)
                        + " column " + std::to_string(index) + ": "
                        + std::to_string(configured[index]) + " vs "
                        + std::to_string(design[index]));
            }
            const int total = std::accumulate(configured.begin(), configured.end(), 0);
            check(total == width, "columns cover the client width");
            check(*std::min_element(configured.begin(), configured.end()) >= 40,
                "no visible column collapses");
        }
    }
}

void sanitizeAndValidate()
{
    ColumnSettings broken;
    broken.columns = {
        {ColumnId::Name, false, 10},
        {ColumnId::Name, true, 200}, // duplicate wins only once
        {ColumnId::Size, true, 5},
        {ColumnId::Status, false, 9999},
        {ColumnId::Size, false, 100},
    };
    broken.sortColumn = ColumnId::Status;
    const auto fixed = sanitizeColumnSettings(broken);
    const auto visible = visibleColumns(fixed);
    check(std::count(visible.begin(), visible.end(), ColumnId::Name) == 1, "name exactly once");
    check(fixed.columns.front().id == ColumnId::Name && fixed.columns.front().visible,
        "required name column is visible and kept first");
    for (const auto& state : fixed.columns) {
        check(state.width >= 40 && state.width <= 2000, "width clamped");
    }
    check(fixed.sortColumn == ColumnId::Count, "hidden sort column falls back to store order");
    check(fixed.columns.size() == static_cast<std::size_t>(ColumnId::Count),
        "all known columns remain addressable in the chooser");

    ColumnSettings noName;
    noName.columns = {{ColumnId::Size, true, 120}};
    const auto withName = sanitizeColumnSettings(noName);
    check(visibleColumns(withName).front() == ColumnId::Name, "missing name is inserted");
}

void persistenceRoundTrip()
{
    TempDir directory;
    const auto path = uiStatePathFor(directory.file("tasks.db"));
    check(path.filename() == L"ui-state.ini", "state lives beside the database");

    ColumnSettings settings = defaultColumnSettings();
    // Reorder: name, status, protocol, path; hide remaining time; custom width.
    settings.columns = {
        {ColumnId::Name, true, 411},
        {ColumnId::Status, true, 222},
        {ColumnId::Protocol, true, 91},
        {ColumnId::SavePath, true, 333},
        {ColumnId::Size, false, 140},
        {ColumnId::Progress, true, 75},
    };
    settings.sortColumn = ColumnId::SavePath;
    settings.sortAscending = false;
    std::string error;
    check(saveColumnSettings(path, settings, error), "save succeeds");

    ColumnSettings loaded;
    check(loadColumnSettings(path, loaded, error), "load succeeds");
    const auto visible = visibleColumns(loaded);
    check(visible.size() == 5, "visibility persisted");
    check(visible[0] == ColumnId::Name && visible[1] == ColumnId::Status
            && visible[2] == ColumnId::Protocol && visible[3] == ColumnId::SavePath
            && visible[4] == ColumnId::Progress,
        "order persisted");
    check(loaded.sortColumn == ColumnId::SavePath && !loaded.sortAscending,
        "sort persisted");
    check(loaded.columns.front().width == 411, "width persisted in DIP");

    std::ofstream invalid(path, std::ios::binary | std::ios::trunc);
    invalid << "version=1\ncolumns=name:1:not-a-number\n";
    invalid.close();
    ColumnSettings fallback;
    check(!loadColumnSettings(path, fallback, error), "malformed file is rejected");
    check(!loadColumnSettings(directory.file("missing.ini"), fallback, error),
        "missing file is reported, caller uses defaults");
}

void dpiRestoreKeepsColumns()
{
    ColumnSettings settings = defaultColumnSettings();
    settings.columns.front().width = 40; // minimum stored DIP width
    for (const int dpi : {96, 120, 144, 168}) {
        const auto widths = columnWidths(settings, 640, dpi);
        const int total = std::accumulate(widths.begin(), widths.end(), 0);
        check(total == 640, "restored widths fill the client at every DPI");
        check(widths.front() >= MulDiv(24, dpi, 96), "name stays visible at every DPI");
        for (const int width : widths) check(width > 0, "no column disappears after DPI change");
    }
}
} // namespace

int main()
{
    return run("ui-columns", [] {
        defaultsMatchDesignPitches();
        sanitizeAndValidate();
        persistenceRoundTrip();
        dpiRestoreKeepsColumns();
        return 0;
    });
}
