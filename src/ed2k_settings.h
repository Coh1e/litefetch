#pragma once
#include "ed2k_task.h"
#include <optional>
class QWidget;
namespace downloader {
struct Ed2kPreferences {bool enabled=false;Ed2kRuntimeOptions options;};
Ed2kPreferences defaultEd2kPreferences();
Ed2kPreferences loadEd2kPreferences(const QString& path);
void saveEd2kPreferences(const QString& path,const Ed2kPreferences& preferences);
Ed2kRuntimeOptions ed2kRuntimeOptions(const Ed2kPreferences& preferences);
std::optional<Ed2kPreferences> chooseEd2kPreferences(QWidget* parent,const Ed2kPreferences& current);
}
