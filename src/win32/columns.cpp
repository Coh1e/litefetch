#include "columns.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <sstream>

namespace litefetch::ui {
namespace {
struct Definition {
    ColumnId id;
    std::string_view key;
    std::string_view title;
    int width;
    bool visible;
};

constexpr std::array<Definition, static_cast<std::size_t>(ColumnId::Count)> definitions{{
    {ColumnId::Name, "name", "名称", 342, true},
    {ColumnId::Size, "size", "大小", 104, true},
    {ColumnId::Progress, "progress", "进度", 82, true},
    {ColumnId::DownloadSpeed, "speed", "下载速度", 126, true},
    {ColumnId::RemainingTime, "remaining", "剩余时间", 108, true},
    {ColumnId::Status, "status", "状态", 236, true},
    {ColumnId::Protocol, "protocol", "协议", 80, false},
    {ColumnId::UploadSpeed, "upload", "上传速度", 100, false},
    {ColumnId::Connections, "connections", "连接数", 80, false},
    {ColumnId::Sources, "sources", "来源数", 80, false},
    {ColumnId::Downloaded, "downloaded", "已下载", 110, false},
    {ColumnId::Uploaded, "uploaded", "已上传", 110, false},
    {ColumnId::AddedTime, "added", "添加时间", 150, false},
    {ColumnId::CompletedTime, "completed", "完成时间", 150, false},
    {ColumnId::SavePath, "path", "保存路径", 280, false},
    {ColumnId::Address, "address", "下载地址", 300, false},
}};

const Definition* definition(ColumnId id)
{
    const auto index = static_cast<std::size_t>(id);
    if (index >= definitions.size()) return nullptr;
    return &definitions[index];
}

int scale(int value, int dpi)
{
    if (value <= 0 || dpi <= 0) return 0;
    return static_cast<int>((static_cast<long long>(value) * dpi + 48) / 96);
}

int roundRatio(int value, int numerator, int denominator)
{
    if (denominator <= 0) return 0;
    return static_cast<int>((static_cast<long long>(value) * numerator + denominator / 2) / denominator);
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

bool parseBool(std::string_view text, bool& value)
{
    if (text == "1" || text == "true") {
        value = true;
        return true;
    }
    if (text == "0" || text == "false") {
        value = false;
        return true;
    }
    return false;
}
} // namespace

std::string_view columnKey(ColumnId id)
{
    const auto* found = definition(id);
    return found ? found->key : std::string_view{};
}

std::optional<ColumnId> parseColumnKey(std::string_view key)
{
    for (const auto& entry : definitions) {
        if (entry.key == key) return entry.id;
    }
    return std::nullopt;
}

std::string_view columnTitle(ColumnId id)
{
    const auto* found = definition(id);
    return found ? found->title : std::string_view{};
}

int defaultColumnWidth(ColumnId id)
{
    const auto* found = definition(id);
    return found ? found->width : 100;
}

bool defaultColumnVisible(ColumnId id)
{
    const auto* found = definition(id);
    return found ? found->visible : false;
}

bool requiredColumn(ColumnId id)
{
    return id == ColumnId::Name;
}

ColumnSettings defaultColumnSettings()
{
    ColumnSettings settings;
    settings.columns.reserve(definitions.size());
    for (const auto& entry : definitions)
        settings.columns.push_back(ColumnState{entry.id, entry.visible, entry.width});
    return settings;
}

ColumnSettings sanitizeColumnSettings(ColumnSettings settings)
{
    ColumnSettings result;
    std::array<bool, static_cast<std::size_t>(ColumnId::Count)> seen{};
    for (const auto& state : settings.columns) {
        const auto index = static_cast<std::size_t>(state.id);
        if (index >= seen.size() || seen[index]) continue;
        if (!definition(state.id)) continue;
        seen[index] = true;
        int width = state.width > 0 ? state.width : defaultColumnWidth(state.id);
        width = std::clamp(width, 40, 2000);
        result.columns.push_back(ColumnState{state.id, state.visible, width});
    }

    if (!seen[static_cast<std::size_t>(ColumnId::Name)]) {
        result.columns.insert(result.columns.begin(),
            ColumnState{ColumnId::Name, true, defaultColumnWidth(ColumnId::Name)});
        seen[static_cast<std::size_t>(ColumnId::Name)] = true;
    }
    // Keep every known optional column available in the chooser even when the
    // state file predates it or it was omitted.
    for (const auto& entry : definitions) {
        const auto index = static_cast<std::size_t>(entry.id);
        if (seen[index]) continue;
        seen[index] = true;
        result.columns.push_back(ColumnState{entry.id, false, entry.width});
    }
    for (auto& state : result.columns) {
        if (requiredColumn(state.id)) state.visible = true;
    }
    if (result.columns.empty()) result = defaultColumnSettings();

    const auto visible = visibleColumns(result);
    const bool sortVisible = settings.sortColumn == ColumnId::Count
        || std::find(visible.begin(), visible.end(), settings.sortColumn) != visible.end();
    result.sortColumn = sortVisible ? settings.sortColumn : ColumnId::Count;
    result.sortAscending = settings.sortAscending;
    return result;
}

std::vector<ColumnId> visibleColumns(const ColumnSettings& settings)
{
    std::vector<ColumnId> visible;
    for (const auto& state : settings.columns) {
        if (state.visible) visible.push_back(state.id);
    }
    return visible;
}

std::vector<int> columnWidths(const ColumnSettings& settings, int clientWidth, int dpi)
{
    std::vector<ColumnState> visible;
    for (const auto& state : settings.columns) {
        if (state.visible) visible.push_back(state);
    }
    std::vector<int> widths(visible.size(), 0);
    if (visible.empty() || clientWidth <= 0 || dpi <= 0) return widths;
    if (visible.size() == 1) {
        widths[0] = clientWidth;
        return widths;
    }

    const int minimum = std::max(1, scale(24, dpi));
    std::vector<int> preferred;
    preferred.reserve(visible.size());
    long long total = 0;
    for (const auto& state : visible) {
        const int width = std::max(minimum, scale(state.width > 0 ? state.width
            : defaultColumnWidth(state.id), dpi));
        preferred.push_back(width);
        total += width;
    }

    if (clientWidth >= total) {
        long long fixed = 0;
        for (std::size_t index = 0; index + 1 < preferred.size(); ++index) {
            widths[index] = preferred[index];
            fixed += preferred[index];
        }
        widths.back() = std::max(minimum, clientWidth - static_cast<int>(fixed));
        return widths;
    }

    long long allocated = 0;
    for (std::size_t index = 0; index < preferred.size(); ++index) {
        widths[index] = std::max(minimum, roundRatio(preferred[index], clientWidth,
            static_cast<int>(total)));
        allocated += widths[index];
    }
    long long difference = static_cast<long long>(clientWidth) - allocated;
    if (difference >= 0) {
        widths.back() += static_cast<int>(difference);
        return widths;
    }
    // The last visible column is the flexible design column: absorb the
    // rounding overflow there first so the fixed pitches stay exact.
    if (widths.back() + difference >= minimum) {
        widths.back() += static_cast<int>(difference);
        return widths;
    }

    // Too wide after rounding/minimums: take pixels from the widest columns
    // first, never below the readable minimum.
    while (difference < 0) {
        std::size_t largest = widths.size();
        for (std::size_t index = 0; index < widths.size(); ++index) {
            if (widths[index] <= minimum) continue;
            if (largest == widths.size() || widths[index] > widths[largest]) largest = index;
        }
        if (largest == widths.size()) break;
        --widths[largest];
        ++difference;
    }
    if (difference < 0) {
        // Pathological client narrower than every column's minimum: distribute
        // the remaining pixels so no visible column vanishes entirely.
        std::size_t index = 0;
        while (difference < 0 && index < widths.size()) {
            if (widths[index] > 1) {
                --widths[index];
                ++difference;
            }
            index = (index + 1) % widths.size();
        }
    }
    return widths;
}

std::filesystem::path uiStatePathFor(const std::filesystem::path& databasePath)
{
    auto directory = databasePath.parent_path();
    if (directory.empty()) directory = std::filesystem::path(L".");
    return directory / L"ui-state.ini";
}

bool loadColumnSettings(const std::filesystem::path& path, ColumnSettings& settings, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open UI state file";
        return false;
    }
    std::string line;
    bool versionSeen = false;
    bool columnsSeen = false;
    ColumnSettings parsed;
    while (std::getline(input, line)) {
        const auto view = trim(line);
        if (view.empty() || view.front() == '#') continue;
        const auto separator = view.find('=');
        if (separator == std::string_view::npos) {
            error = "malformed UI state line";
            return false;
        }
        const auto key = trim(view.substr(0, separator));
        const auto value = trim(view.substr(separator + 1));
        if (key == "version") {
            if (value != "1") {
                error = "unsupported UI state version";
                return false;
            }
            versionSeen = true;
        } else if (key == "columns") {
            columnsSeen = true;
            std::size_t start = 0;
            while (start <= value.size()) {
                const auto end = value.find(';', start);
                const auto item = end == std::string_view::npos ? value.substr(start)
                                                                 : value.substr(start, end - start);
                if (!item.empty()) {
                    const auto first = item.find(':');
                    const auto second = first == std::string_view::npos
                        ? std::string_view::npos : item.find(':', first + 1);
                    if (first == std::string_view::npos || second == std::string_view::npos) {
                        error = "malformed column entry";
                        return false;
                    }
                    const auto id = parseColumnKey(item.substr(0, first));
                    bool visible = false;
                    int width = 0;
                    const auto widthText = item.substr(second + 1);
                    const auto result = std::from_chars(widthText.data(),
                        widthText.data() + widthText.size(), width);
                    if (!id || !parseBool(item.substr(first + 1, second - first - 1), visible)
                        || result.ec != std::errc{} || result.ptr != widthText.data() + widthText.size()) {
                        error = "unknown column or malformed width";
                        return false;
                    }
                    parsed.columns.push_back(ColumnState{*id, visible, width});
                }
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
        } else if (key == "sort") {
            const auto separator2 = value.find(':');
            if (separator2 == std::string_view::npos) {
                error = "malformed sort entry";
                return false;
            }
            const auto idText = value.substr(0, separator2);
            const auto direction = value.substr(separator2 + 1);
            if (idText == "none") {
                parsed.sortColumn = ColumnId::Count;
            } else {
                const auto id = parseColumnKey(idText);
                if (!id) {
                    error = "unknown sort column";
                    return false;
                }
                parsed.sortColumn = *id;
            }
            if (direction == "asc") {
                parsed.sortAscending = true;
            } else if (direction == "desc") {
                parsed.sortAscending = false;
            } else {
                error = "unknown sort direction";
                return false;
            }
        } else {
            error = "unknown UI state key";
            return false;
        }
    }
    if (!versionSeen || !columnsSeen) {
        error = "incomplete UI state file";
        return false;
    }
    settings = sanitizeColumnSettings(std::move(parsed));
    return true;
}

bool saveColumnSettings(const std::filesystem::path& path, const ColumnSettings& settings, std::string& error)
{
    const auto sanitized = sanitizeColumnSettings(settings);
    std::error_code filesystemError;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) {
        error = "cannot create UI state directory: " + filesystemError.message();
        return false;
    }

    std::ostringstream text;
    text << "# LiteFetch UI state\nversion=1\ncolumns=";
    bool first = true;
    for (const auto& state : sanitized.columns) {
        if (!first) text << ';';
        first = false;
        text << columnKey(state.id) << ':' << (state.visible ? 1 : 0) << ':' << state.width;
    }
    text << "\nsort="
         << (sanitized.sortColumn == ColumnId::Count ? "none" : std::string(columnKey(sanitized.sortColumn)))
         << ':' << (sanitized.sortAscending ? "asc" : "desc") << '\n';

    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "cannot write UI state file";
            return false;
        }
        output << text.str();
        output.flush();
        if (!output) {
            error = "cannot finish UI state file";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(path, filesystemError);
        filesystemError.clear();
        std::filesystem::rename(temporary, path, filesystemError);
    }
    if (filesystemError) {
        std::filesystem::remove(temporary, filesystemError);
        error = "cannot replace UI state file";
        return false;
    }
    return true;
}

} // namespace litefetch::ui
