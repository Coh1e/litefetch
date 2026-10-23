#pragma once
#include "browser_bridge.h"
#include <optional>
class QWidget;
namespace downloader {
struct BrowserPreferences { bool enabled=false; QString directory; bool prepareConnection=false; };
std::optional<BrowserPreferences> chooseBrowserPreferences(QWidget* parent,const BrowserPreferences& current);
BrowserBridgeConfig makeBrowserSetup(const QString& dataDirectory,const BrowserPreferences& preferences);
void saveBrowserSetup(const QString& file,const BrowserBridgeConfig& config);
QString prepareBrowserConnection(const QString& applicationDirectory,const BrowserBridgeConfig& config);
}
