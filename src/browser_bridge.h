#pragma once
#include "task_manager.h"
#include <QLocalServer>
#include <QJsonObject>
#include <QSet>
#include <QSharedMemory>
namespace downloader {
struct BrowserBridgeConfig {
    bool enabled = false;
    QString ipcName, downloadDirectory;
    QStringList allowedOrigins;
};
BrowserBridgeConfig loadBrowserBridgeConfig(const QString& path);
class BrowserBridge : public QObject {
public:
    BrowserBridge(TaskManager& manager, BrowserBridgeConfig config, QObject* parent = nullptr);
    ~BrowserBridge() override;
    void updatePreferences(bool enabled, const QString& downloadDirectory);
private:
    TaskManager& manager_;
    BrowserBridgeConfig config_;
    QSharedMemory owner_;
    QLocalServer server_;
    QSet<QLocalSocket*> clients_;
    QJsonObject handle(const QJsonObject& message);
};
}
