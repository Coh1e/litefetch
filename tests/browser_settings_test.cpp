#include "browser_settings.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QJsonDocument>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
using namespace downloader;
int main(int argc,char** argv)
{
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
    QTemporaryDir directory(app.applicationDirPath()+"/browser-settings-check-XXXXXX");
    if (!directory.isValid()) return 10;
    int failures=0;
    const auto check=[&](bool ok,const char* name) { if (!ok) { ++failures; QTextStream(stderr)<<"FAIL "<<name<<'\n'; } };
    const auto modal=[]() -> QDialog* {
        for (auto* widget : QApplication::topLevelWidgets())
            if (widget->objectName()=="browserPreferencesDialog") return qobject_cast<QDialog*>(widget);
        return nullptr;
    };
    const auto downloads=directory.filePath("下载"), file=directory.filePath("browser-bridge.json");
    QTimer::singleShot(100,&app,[&] {
        auto* dialog=modal();
        if (!dialog) { check(false,"native browser settings dialog exists"); app.exit(2); return; }
        auto* enabled=dialog->findChild<QCheckBox*>("browserCaptureEnabled");
        auto* path=dialog->findChild<QLineEdit*>("browserSaveDirectory");
        auto* buttons=dialog->findChild<QDialogButtonBox*>("browserPreferencesButtons");
        if (!enabled || !path || !buttons) { check(false,"native settings controls exist"); dialog->reject(); return; }
        check(!enabled->isChecked(),"capture defaults off");
        enabled->setChecked(true); path->setText("relative/path");
        check(!buttons->button(QDialogButtonBox::Save)->isEnabled(),"relative save folder rejected");
        path->setText(downloads);
        check(buttons->button(QDialogButtonBox::Save)->isEnabled(),"new folder with existing parent allowed");
        if (app.arguments().contains("--image")) {
            QTimer::singleShot(180,dialog,[&,dialog,buttons] {
                check(dialog->grab().save(app.arguments().value(app.arguments().indexOf("--image")+1)),"native settings screenshot saved");
                buttons->button(QDialogButtonBox::Save)->click();
            });
        } else buttons->button(QDialogButtonBox::Save)->click();
    });
    const auto choice=chooseBrowserPreferences(nullptr,{false,downloads});
    check(choice && choice->enabled && choice->directory==QDir::cleanPath(downloads) && !choice->prepareConnection,"save returns preferences without implicit installation");
    if (!choice) return 1;
    const auto config=makeBrowserSetup(directory.path(),*choice);
    check(config.allowedOrigins==QStringList{"chrome-extension://namdpafnjehflfkcgdbnmoccndohmjoj/"},"compiled identity agrees with extension public key");
    check(QFileInfo(downloads).isDir(),"accepted configuration creates selected folder");
    saveBrowserSetup(file,config);
    const auto restored=loadBrowserBridgeConfig(file);
    check(restored.enabled && restored.ipcName==config.ipcName && restored.downloadDirectory==config.downloadDirectory,"native settings persisted and reload exactly");
    auto off=config; off.enabled=false; saveBrowserSetup(file,off);
    const auto disabled=loadBrowserBridgeConfig(file);
    check(!disabled.enabled && disabled.ipcName==config.ipcName,"disabled config retains recovery endpoint");
    const auto cancelledFolder=directory.filePath("cancelled");
    QTimer::singleShot(100,&app,[&] {
        auto* dialog=modal(); if (dialog) dialog->reject(); else { check(false,"cancel dialog exists"); app.exit(2); }
    });
    check(!chooseBrowserPreferences(nullptr,{true,cancelledFolder}),"cancel does not accept settings");
    check(!QFileInfo::exists(cancelledFolder),"cancel creates no directory");
    const auto disabledNewFolder=directory.filePath("disabled-new");
    const auto disabledNew=makeBrowserSetup(directory.path(),{false,disabledNewFolder});
    check(disabledNew.ipcName.isEmpty() && !QFileInfo::exists(disabledNewFolder),"disabled initial setup creates no download directory or endpoint");
    bool failed=false;
    try { saveBrowserSetup(directory.path(),config); } catch (...) { failed=true; }
    check(failed && !loadBrowserBridgeConfig(file).enabled,"failed write preserves prior stored settings");
    QTimer::singleShot(100,&app,[&] {
        auto* dialog=modal();
        auto* prepare=dialog?dialog->findChild<QPushButton*>("browserPrepareConnection"):nullptr;
        if (prepare) prepare->click(); else { check(false,"prepare button exists"); if (dialog) dialog->reject(); else app.exit(2); }
    });
    const auto prepareChoice=chooseBrowserPreferences(nullptr,{true,downloads});
    check(prepareChoice && prepareChoice->prepareConnection,"explicit prepare action is distinct from save");
    const auto hostSource=app.arguments().value(app.arguments().indexOf("--host")+1);
    check(QFile::copy(hostSource,directory.filePath("native-downloader-host.exe")),"prepare fixture uses real host executable");
    const auto manifest=prepareBrowserConnection(directory.path(),config);
    QFile hostManifest(manifest); check(hostManifest.open(QIODevice::ReadOnly),"native host manifest written");
    const auto manifestBytes=hostManifest.readAll(); hostManifest.close();
    QFile hostConfig(directory.filePath("native-host.json")); check(hostConfig.open(QIODevice::ReadOnly),"native runtime configuration written");
    const auto runtime=QJsonDocument::fromJson(hostConfig.readAll()).object(); hostConfig.close();
    const auto description=QJsonDocument::fromJson(manifestBytes).object();
    check(runtime.value("ipcName").toString()==config.ipcName && description.value("allowed_origins")==runtime.value("allowedOrigins"),
        "exported host files agree with exact GUI endpoint and origin");
    check(prepareBrowserConnection(directory.path(),config)==manifest,"preparation repeat is stable");
    auto conflict=config; conflict.ipcName="AnotherTaskProfile"; failed=false;
    try { prepareBrowserConnection(directory.path(),conflict); } catch (...) { failed=true; }
    check(failed,"different task profile cannot overwrite connection files");
    check(hostManifest.open(QIODevice::ReadOnly) && hostManifest.readAll()==manifestBytes,"conflict retains original manifest"); hostManifest.close();
    QTextStream(stdout)<<"Browser native settings checks: "<<(failures?"FAILED":"PASS")<<'\n';
    return failures?1:0;
}
