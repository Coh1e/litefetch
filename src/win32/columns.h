#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litefetch::ui {

// Every column the complete product design allows in the main list. The first
// six are the default Figma columns; the rest are optional and only show real
// engine data (unknown values render as the em dash).
enum class ColumnId {
    Name,
    Size,
    Progress,
    DownloadSpeed,
    RemainingTime,
    Status,
    Protocol,
    UploadSpeed,
    Connections,
    Sources,
    Downloaded,
    Uploaded,
    AddedTime,
    CompletedTime,
    SavePath,
    Address,
    Count,
};

std::string_view columnKey(ColumnId id);
std::optional<ColumnId> parseColumnKey(std::string_view key);
std::string_view columnTitle(ColumnId id); // UTF-8
int defaultColumnWidth(ColumnId id);       // device-independent pixels (96 DPI)
bool defaultColumnVisible(ColumnId id);
bool requiredColumn(ColumnId id);

struct ColumnState {
    ColumnId id = ColumnId::Name;
    bool visible = true;
    int width = 100; // DIP
};

struct ColumnSettings {
    std::vector<ColumnState> columns;
    // ColumnId::Count means "no explicit sort": keep the store order
    // (created_at, id), which is what an untouched list shows.
    ColumnId sortColumn = ColumnId::Count;
    bool sortAscending = true;
};

ColumnSettings defaultColumnSettings();

// Removes unknown/duplicate ids, clamps widths, guarantees at least the name
// column is visible, and keeps every known column addressable. Safe to call on
// settings loaded from disk.
ColumnSettings sanitizeColumnSettings(ColumnSettings settings);

std::vector<ColumnId> visibleColumns(const ColumnSettings& settings);

// Widths in device pixels for the visible columns in display order. The last
// visible column absorbs spare width (the Figma status-column flex behavior).
// Below the preferred total every column shrinks proportionally but keeps a
// readable minimum. The returned size equals visibleColumns().size().
std::vector<int> columnWidths(const ColumnSettings& settings, int clientWidth, int dpi);

// UI state (column order/visibility/width and sort) lives beside the task
// database so --db acceptance runs keep their state project-local.
std::filesystem::path uiStatePathFor(const std::filesystem::path& databasePath);
bool loadColumnSettings(const std::filesystem::path& path, ColumnSettings& settings, std::string& error);
bool saveColumnSettings(const std::filesystem::path& path, const ColumnSettings& settings, std::string& error);

} // namespace litefetch::ui
