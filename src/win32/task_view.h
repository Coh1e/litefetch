#pragma once

#include "columns.h"
#include "task_list_model.h"

#include <string>
#include <string_view>
#include <vector>

namespace litefetch::ui {

enum class TaskFilter { All, Active, Completed, Failed };
enum class SortDirection { Ascending, Descending };

// URL shown in the list, infotips, clipboard and reports. Credentials and
// token-like query values are replaced; the transfer itself always uses the
// original source unchanged.
std::string redactAddress(std::string_view address);
std::string protocolLabel(std::string_view source);

bool matchesSearch(const TaskRow& row, std::string_view query);
bool matchesFilter(const TaskRow& row, TaskFilter filter);
bool rowMatches(const TaskRow& row, TaskFilter filter, std::string_view query);

// Display text for one real column. Unknown values use the em dash; no value
// is invented for upload, connection or source counts this engine does not
// report yet.
std::string columnText(const TaskRow& row, ColumnId column);

// Sorts by the real numeric/time value behind a column (not its formatted
// string). Unknown values stay after known values in both directions and ties
// fall back to the stable TaskId order.
void sortRows(std::vector<TaskRow>& rows, ColumnId column, SortDirection direction);

} // namespace litefetch::ui
