#pragma once
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QWidget>
namespace downloader {
// Window outlives this controller. Connections use the owned tray as context.
class DesktopTray final {
public:
    explicit DesktopTray(QWidget& window):window_(window){
        tray_.setIcon(QApplication::style()->standardIcon(QStyle::SP_ArrowDown));
        tray_.setToolTip(QStringLiteral("下载器"));
        auto* show=menu_.addAction(QStringLiteral("显示主窗口"));
        auto* quit=menu_.addAction(QStringLiteral("退出下载器"));
        QObject::connect(show,&QAction::triggered,&tray_,[this]{restore();});
        QObject::connect(quit,&QAction::triggered,qApp,&QApplication::quit);
        QObject::connect(&tray_,&QSystemTrayIcon::activated,&tray_,[this](QSystemTrayIcon::ActivationReason reason){
            if(reason==QSystemTrayIcon::Trigger||reason==QSystemTrayIcon::DoubleClick)restore();
        });
        QObject::connect(&tray_,&QSystemTrayIcon::messageClicked,&tray_,[this]{restore();});
        tray_.setContextMenu(&menu_);
        if(QSystemTrayIcon::isSystemTrayAvailable())tray_.show();
    }
    DesktopTray(const DesktopTray&)=delete;
    DesktopTray& operator=(const DesktopTray&)=delete;
    bool available() const{return QSystemTrayIcon::isSystemTrayAvailable()&&tray_.isVisible();}
    bool hideWindow(){if(!available())return false;window_.hide();return true;}
    void restore(){window_.showNormal();window_.raise();window_.activateWindow();}
    bool completed(const QString& name){
        if(!available()||!QSystemTrayIcon::supportsMessages())return false;
        tray_.showMessage(QStringLiteral("下载完成"),name,QSystemTrayIcon::Information,5000);return true;
    }
private:
    QWidget& window_;
    QMenu menu_;
    QSystemTrayIcon tray_;
};
}
