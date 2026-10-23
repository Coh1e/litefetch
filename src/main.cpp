#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QRegularExpression>
#include <QDir>
#include <QLockFile>
#include <QDesktopServices>
#include <QMenu>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibraryInfo>
#include <QMainWindow>
#include <QPixmap>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSet>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QMenuBar>
#include <QHeaderView>
#include <QShortcut>
#include "task_manager.h"
#include "torrent_dialog.h"
#include "browser_bridge.h"
#include "browser_settings.h"
#include "ed2k_settings.h"
#include "download_batch.h"
#include "desktop_tray.h"
#include "bandwidth_settings.h"
#include "task_table.h"
#include "table_layout.h"
#include "task_details.h"
#include "seeding_settings.h"
#include "sha256_dialog.h"
#include "sha256_verify.h"
#include <memory>

#include "download_window.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("NativeDownloader");
    QApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"smoke-report", "Write GUI startup evidence and exit.", "path"});
    parser.addOption({"data-dir", "Portable task database directory.", "path", QCoreApplication::applicationDirPath() + "/data"});
    parser.addOption({"browser-bridge-config", "Explicit browser bridge development configuration.", "path"});
    parser.process(app);

    const auto dataDir = QFileInfo(parser.value("data-dir")).absoluteFilePath();
    if (!QDir().mkpath(dataDir)) {
        QMessageBox::critical(nullptr, QStringLiteral("无法启动"), QStringLiteral("无法创建任务数据目录。"));
        return 4;
    }
    QLockFile lock(QDir(dataDir).filePath("session.lock"));
    if (!lock.tryLock(0)) {
        QMessageBox::information(nullptr, QStringLiteral("下载器"), QStringLiteral("该任务目录已由另一个下载器窗口使用。"));
        return 5;
    }
    std::unique_ptr<downloader::TaskManager> manager;
    const auto bandwidthConfigPath=QDir(dataDir).filePath("bandwidth.json");
    downloader::BandwidthPreferences bandwidthPreferences;
    QString bandwidthStartupError;
    const auto seedingConfigPath=QDir(dataDir).filePath("bt-seeding.json");
    downloader::SeedingPolicy seedingPreferences;QString seedingStartupError;
    try{seedingPreferences=downloader::loadSeedingPolicy(seedingConfigPath);}
    catch(...){seedingStartupError=QStringLiteral("做种设置无法读取，暂用完成即停止；原设置文件已保留。");}
    try{bandwidthPreferences=downloader::loadBandwidthPreferences(bandwidthConfigPath);}
    catch(...){bandwidthStartupError=QStringLiteral("限速设置无法读取，暂用默认值；原设置文件已保留。");}
    const auto ed2kConfigPath=QDir(dataDir).filePath("ed2k.json");
    auto ed2kPreferences=downloader::defaultEd2kPreferences();
    QString ed2kStartupError;
    try{ed2kPreferences=downloader::loadEd2kPreferences(ed2kConfigPath);}catch(...){ed2kStartupError=QStringLiteral("ed2k 设置无法读取，请重新配置。");}
    try { 
        const auto effective=downloader::effectiveBandwidthPreferences(bandwidthPreferences);
        downloader::TorrentRuntimeOptions torrentOptions;
        torrentOptions.engine.downloadBytesPerSecond=effective.btDownload;
        torrentOptions.engine.uploadBytesPerSecond=effective.btUpload;
        torrentOptions.seeding=seedingPreferences;
        manager = std::make_unique<downloader::TaskManager>(QDir(dataDir).filePath("tasks.sqlite"),3,downloader::TransferRunner{},nullptr,
            torrentOptions,downloader::ed2kRuntimeOptions(ed2kPreferences));
        manager->setHttpDownloadLimit(effective.directDownload);
    }
    catch (const std::exception& error) {
        QMessageBox::critical(nullptr, QStringLiteral("任务数据无法打开"), QString::fromUtf8(error.what()));
        return 6;
    }

    const auto applyBandwidth=[&](const downloader::BandwidthPreferences& preferences){
        const auto effective=downloader::effectiveBandwidthPreferences(preferences);
        manager->setHttpDownloadLimit(effective.directDownload);
        manager->setTorrentRateLimits({effective.btDownload,effective.btUpload});
    };
    std::unique_ptr<downloader::BrowserBridge> browserBridge;
    downloader::BrowserBridgeConfig bridgeConfig;
    const auto bridgeConfigPath=parser.isSet("browser-bridge-config")?QFileInfo(parser.value("browser-bridge-config")).absoluteFilePath()
        :QDir(dataDir).filePath("browser-bridge.json");
    QString bridgeStartupError;
    if (parser.isSet("browser-bridge-config") || QFileInfo::exists(bridgeConfigPath)) {
        try {
            bridgeConfig=downloader::loadBrowserBridgeConfig(bridgeConfigPath);
            if (!bridgeConfig.ipcName.isEmpty()) browserBridge=std::make_unique<downloader::BrowserBridge>(*manager,bridgeConfig);
        } catch (...) {
            bridgeConfig={}; bridgeStartupError=QStringLiteral("浏览器接管未启用，请检查接管设置。");
            if (parser.isSet("browser-bridge-config")) {
                QMessageBox::critical(nullptr,QStringLiteral("浏览器接管未启用"),bridgeStartupError); return 7;
            }
        }
    }
    DownloadWindow window;
    window.setWindowTitle(QStringLiteral("下载器 · 开发版"));
    window.resize(960, 600);
    downloader::DesktopTray desktopTray(window);
    auto* toolbar = window.addToolBar(QStringLiteral("任务"));
    toolbar->setMovable(false);
    auto* add = toolbar->addAction(QStringLiteral("添加下载"));
    add->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    auto* addTorrent = toolbar->addAction(QStringLiteral("打开种子"));
    addTorrent->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
    auto* cancel = toolbar->addAction(QStringLiteral("取消下载"));
    auto* pause = toolbar->addAction(QStringLiteral("暂停"));
    auto* resume = toolbar->addAction(QStringLiteral("继续"));
    auto* retry = toolbar->addAction(QStringLiteral("从头重试"));
    auto* openFolder = toolbar->addAction(QStringLiteral("打开目录"));
    auto* browserSettings=toolbar->addAction(QStringLiteral("浏览器接管"));
    auto* ed2kSettings=toolbar->addAction(QStringLiteral("ed2k 设置"));
    auto* speedLimit=toolbar->addAction(QStringLiteral("HTTP/电驴限速"));
    speedLimit->setToolTip(QStringLiteral("HTTP(S)/FTP和ed2k任务共用下载限额；BT单独计额。设置自动保存。"));
    QObject::connect(speedLimit,&QAction::triggered,&window,[&]{
        bool accepted=false;
        const auto kib=QInputDialog::getInt(&window,QStringLiteral("HTTP/电驴下载限速"),
            QStringLiteral("HTTP(S)/FTP、ed2k合计下载速度（KiB/s，0不限速）\n设置自动保存；BT单独计额。")+
                (bandwidthPreferences.lowUsage?QStringLiteral("\n低占用已开启：当前实际额度至多128 KiB/s。"):QString{}),
            bandwidthPreferences.directDownload/1024,0,1048576,64,&accepted);
        if(accepted){
            auto next=bandwidthPreferences;next.directDownload=kib*1024;
            try{downloader::saveBandwidthPreferences(bandwidthConfigPath,next);applyBandwidth(next);bandwidthPreferences=next;
                window.statusBar()->showMessage(QStringLiteral("HTTP/电驴限速设置已保存。"));}
            catch(...){QMessageBox::warning(&window,QStringLiteral("设置未保存"),QStringLiteral("限速设置无法写入，请检查数据目录。"));}
        }
    });
    auto* toTray=toolbar->addAction(QStringLiteral("收起到托盘"));
    auto* btLimits=toolbar->addAction(QStringLiteral("BT 限速"));
    btLimits->setToolTip(QStringLiteral("所有BT任务共用的上传/下载限额，独立于HTTP/电驴；设置自动保存。"));
    QObject::connect(btLimits,&QAction::triggered,&window,[&]{
        QDialog dialog(&window);dialog.setWindowTitle(QStringLiteral("BT 会话限速"));QFormLayout form(&dialog);
        const downloader::TorrentRateLimits limits{bandwidthPreferences.btDownload,bandwidthPreferences.btUpload};
        QSpinBox download,upload;download.setRange(0,1048576);upload.setRange(0,1048576);
        download.setValue(limits.download/1024);upload.setValue(limits.upload/1024);
        download.setSuffix(QStringLiteral(" KiB/s"));upload.setSuffix(QStringLiteral(" KiB/s"));
        download.setSpecialValueText(QStringLiteral("不限速"));upload.setSpecialValueText(QStringLiteral("不限速"));
        form.addRow(QStringLiteral("合计下载"),&download);form.addRow(QStringLiteral("合计上传"),&upload);
        QLabel explanation(QStringLiteral("仅BT任务共用；与HTTP/电驴分别计额。\n设置自动保存。")+
            (bandwidthPreferences.lowUsage?QStringLiteral("\n低占用已开启：实际下载至多128、上传至多32 KiB/s。"):QString{}));form.addRow(&explanation);
        QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
        QObject::connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
        QObject::connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);form.addRow(&buttons);
        if(dialog.exec()==QDialog::Accepted){
            auto next=bandwidthPreferences;next.btDownload=download.value()*1024;next.btUpload=upload.value()*1024;
            try{downloader::saveBandwidthPreferences(bandwidthConfigPath,next);applyBandwidth(next);bandwidthPreferences=next;
                window.statusBar()->showMessage(QStringLiteral("BT上传/下载限速设置已保存。"));}
            catch(...){QMessageBox::warning(&window,QStringLiteral("设置未保存"),QStringLiteral("限速设置无法写入，请检查数据目录。"));}
        }
    });
    toTray->setEnabled(desktopTray.available());
    toTray->setToolTip(QStringLiteral("下载继续运行；点击托盘图标恢复窗口。关闭窗口会退出下载器。"));
    QObject::connect(toTray,&QAction::triggered,&window,[&]{desktopTray.hideWindow();});
    auto* fileMenu=window.menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addAction(add);fileMenu->addAction(addTorrent);fileMenu->addSeparator();
    auto* quit=fileMenu->addAction(QStringLiteral("退出(&X)"));
    QObject::connect(quit,&QAction::triggered,&app,&QApplication::quit);
    auto* taskMenu=window.menuBar()->addMenu(QStringLiteral("任务(&T)"));
    for(auto* action:{pause,resume,cancel,retry,openFolder})taskMenu->addAction(action);
    taskMenu->addSeparator();
    auto* verifyFile=taskMenu->addAction(QStringLiteral("SHA-256 校验…"));verifyFile->setEnabled(false);
    auto* taskDetails=taskMenu->addAction(QStringLiteral("任务详情…"));taskDetails->setEnabled(false);
    taskDetails->setShortcut(QKeySequence(QStringLiteral("Alt+Return")));
    auto* optionsMenu=window.menuBar()->addMenu(QStringLiteral("选项(&O)"));
    auto* seedingSettings=optionsMenu->addAction(QStringLiteral("BT 做种…"));
    QObject::connect(seedingSettings,&QAction::triggered,&window,[&]{
        QDialog dialog(&window);dialog.setWindowTitle(QStringLiteral("BT 做种停止条件"));QFormLayout form(&dialog);
        QSpinBox seconds,ratio;seconds.setRange(0,604800);ratio.setRange(0,100000);
        seconds.setValue(seedingPreferences.seconds);ratio.setValue(seedingPreferences.ratioPercent);
        seconds.setSuffix(QStringLiteral(" 秒"));ratio.setSuffix(QStringLiteral(" %"));
        seconds.setSpecialValueText(QStringLiteral("不按时间停止"));ratio.setSpecialValueText(QStringLiteral("不按比例停止"));
        form.addRow(QStringLiteral("完成后做种时长"),&seconds);form.addRow(QStringLiteral("累计上传 / 所选大小"),&ratio);
        QLabel explanation(QStringLiteral("两项均为0：完成即停止。启用多项时先达到任一项即停止。\n时间按本次连续做种计算；比例可包含之前已保存的累计上传。\n只影响之后启动或继续的任务；运行中任务保留原策略。"));explanation.setWordWrap(true);form.addRow(&explanation);
        QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
        QObject::connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
        QObject::connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);form.addRow(&buttons);
        if(dialog.exec()!=QDialog::Accepted)return;
        const downloader::SeedingPolicy next{seconds.value(),ratio.value()};
        try{downloader::saveSeedingPolicy(seedingConfigPath,next);manager->setSeedingPolicy(next);seedingPreferences=next;
            window.statusBar()->showMessage(QStringLiteral("做种策略已保存，对之后启动或继续的BT任务生效。"));}
        catch(...){QMessageBox::warning(&window,QStringLiteral("设置未保存"),QStringLiteral("做种策略无法保存，请检查任务数据目录。"));}
    });
    for(auto* action:{speedLimit,btLimits,browserSettings,ed2kSettings})optionsMenu->addAction(action);
    optionsMenu->addSeparator();
    auto* lowUsage=optionsMenu->addAction(QStringLiteral("低占用模式"));lowUsage->setCheckable(true);lowUsage->setChecked(bandwidthPreferences.lowUsage);
    lowUsage->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    lowUsage->setToolTip(QStringLiteral("HTTP/电驴与BT下载各至多128 KiB/s，BT上传至多32 KiB/s；更低的自定义限额保留。关闭后恢复原设置。"));
    QObject::connect(lowUsage,&QAction::triggered,&window,[&](bool enabled){
        auto next=bandwidthPreferences;next.lowUsage=enabled;
        try{downloader::saveBandwidthPreferences(bandwidthConfigPath,next);applyBandwidth(next);bandwidthPreferences=next;
            window.statusBar()->showMessage(enabled?QStringLiteral("低占用：两组下载各至多128 KiB/s，BT上传至多32 KiB/s。"):
                QStringLiteral("已关闭低占用，恢复自定义限额。"));}
        catch(...){lowUsage->setChecked(bandwidthPreferences.lowUsage);QMessageBox::warning(&window,QStringLiteral("设置未保存"),QStringLiteral("低占用设置未能保存，请检查数据目录。"));}
    });
    auto* viewMenu=window.menuBar()->addMenu(QStringLiteral("查看(&V)"));viewMenu->addAction(toTray);
    for(auto* action:{retry,openFolder,browserSettings,ed2kSettings,speedLimit,btLimits,toTray})toolbar->removeAction(action);
    pause->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
    resume->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    auto* content = new QWidget(&window);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(2,2,2,0);layout->setSpacing(2);
    auto* filter=new QLineEdit(content);filter->setClearButtonEnabled(true);
    filter->setPlaceholderText(QStringLiteral("筛选任务名称或链接"));
    filter->setToolTip(QStringLiteral("Ctrl+F定位；Esc清空。添加链接用Ctrl+N，打开种子用Ctrl+O。"));
    layout->addWidget(filter);
    auto* focusFilter=new QShortcut(QKeySequence::Find,&window);
    QObject::connect(focusFilter,&QShortcut::activated,&window,[filter]{filter->setFocus();filter->selectAll();});
    auto* clearFilter=new QShortcut(QKeySequence(Qt::Key_Escape),filter);clearFilter->setContext(Qt::WidgetShortcut);
    QObject::connect(clearFilter,&QShortcut::activated,filter,&QLineEdit::clear);
    auto* tasks = new downloader::TaskTable(content);
    tasks->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("类型"),
        QStringLiteral("进度"), QStringLiteral("速度"), QStringLiteral("状态")});
    tasks->setSelectionBehavior(QAbstractItemView::SelectRows);
    tasks->setSelectionMode(QAbstractItemView::SingleSelection);
    tasks->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tasks->setShowGrid(false);tasks->setWordWrap(false);
    tasks->verticalHeader()->hide();tasks->verticalHeader()->setDefaultSectionSize(22);
    tasks->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft|Qt::AlignVCenter);
    tasks->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    tasks->setColumnWidth(1,76);tasks->setColumnWidth(2,90);tasks->setColumnWidth(3,100);tasks->setColumnWidth(4,140);
    tasks->setSortingEnabled(true);tasks->sortItems(0,Qt::AscendingOrder);
    const auto tableLayoutPath=QDir(dataDir).filePath("task-table.json");
    const bool tableLayoutValid=downloader::loadTableLayout(tableLayoutPath,*tasks->horizontalHeader());
    tasks->horizontalHeader()->setSectionsMovable(true);
    QObject::connect(tasks->horizontalHeader(),&QHeaderView::sectionResized,tasks,[tasks](int logical,int,int){if(logical!=0)tasks->setProperty("layoutDirty",true);});
    QObject::connect(tasks->horizontalHeader(),&QHeaderView::sectionMoved,tasks,[tasks]{tasks->setProperty("layoutDirty",true);});
    QObject::connect(tasks->horizontalHeader(),&QHeaderView::sortIndicatorChanged,tasks,[tasks]{tasks->setProperty("layoutDirty",true);});
    QObject::connect(&app,&QApplication::aboutToQuit,tasks,[tasks,tableLayoutPath]{
        if(tasks->property("layoutDirty").toBool()&&!downloader::saveTableLayout(tableLayoutPath,*tasks->horizontalHeader()))
            qWarning("Task table layout could not be saved");
    });
    QObject::connect(filter,&QLineEdit::textChanged,tasks,&downloader::TaskTable::setTaskFilter);
    layout->addWidget(tasks);
    window.setCentralWidget(content);
    window.statusBar()->showMessage(bridgeStartupError.isEmpty()?QStringLiteral("就绪"):bridgeStartupError);
    if(!tableLayoutValid)window.statusBar()->showMessage(QStringLiteral("列表布局无法读取，暂用默认布局；原设置文件已保留。"));
    if(!ed2kStartupError.isEmpty())window.statusBar()->showMessage(ed2kStartupError);
    if(!bandwidthStartupError.isEmpty())window.statusBar()->showMessage(bandwidthStartupError);
    else if(bandwidthPreferences.lowUsage&&ed2kStartupError.isEmpty()&&bridgeStartupError.isEmpty())window.statusBar()->showMessage(QStringLiteral("就绪 · 低占用模式"));
    if(!seedingStartupError.isEmpty())window.statusBar()->showMessage(seedingStartupError);
    QObject::connect(ed2kSettings,&QAction::triggered,&window,[&]{
        const auto selected=downloader::chooseEd2kPreferences(&window,ed2kPreferences);if(!selected)return;
        if(!manager->configureEd2k(downloader::ed2kRuntimeOptions(*selected))){
            QMessageBox::information(&window,QStringLiteral("ed2k 设置"),QStringLiteral("请先暂停正在运行的 ed2k 任务，再修改找源设置。"));return;
        }
        try{downloader::saveEd2kPreferences(ed2kConfigPath,*selected);ed2kPreferences=*selected;window.statusBar()->showMessage(QStringLiteral("ed2k 设置已保存。"));}
        catch(...){manager->configureEd2k(downloader::ed2kRuntimeOptions(ed2kPreferences));QMessageBox::warning(&window,QStringLiteral("设置未保存"),QStringLiteral("无法保存 ed2k 设置，已恢复原设置。"));}
    });
    QObject::connect(browserSettings,&QAction::triggered,&window,[&] {
        const auto preferences=downloader::chooseBrowserPreferences(&window,{bridgeConfig.enabled,
            bridgeConfig.downloadDirectory.isEmpty()?QDir(dataDir).filePath("downloads"):bridgeConfig.downloadDirectory});
        if (!preferences) return;
        try {
            auto next=downloader::makeBrowserSetup(dataDir,*preferences);
            if (!bridgeConfig.ipcName.isEmpty()) {
                next.ipcName=bridgeConfig.ipcName; next.allowedOrigins=bridgeConfig.allowedOrigins;
                if (next.downloadDirectory.isEmpty()) next.downloadDirectory=bridgeConfig.downloadDirectory;
            }
            std::unique_ptr<downloader::BrowserBridge> candidate;
            if (!browserBridge && !next.ipcName.isEmpty()) {
                auto inactive=next; inactive.enabled=false;
                candidate=std::make_unique<downloader::BrowserBridge>(*manager,std::move(inactive));
            }
            downloader::saveBrowserSetup(bridgeConfigPath,next);
            if (browserBridge) browserBridge->updatePreferences(next.enabled,next.downloadDirectory);
            if (candidate) { candidate->updatePreferences(next.enabled,next.downloadDirectory); browserBridge=std::move(candidate); }
            bridgeConfig=std::move(next);
            if (preferences->prepareConnection) {
                try {
                    downloader::prepareBrowserConnection(QCoreApplication::applicationDirPath(),bridgeConfig);
                    QMessageBox::information(&window,QStringLiteral("连接文件已准备"),
                        QStringLiteral("本机设置和连接文件已保存。浏览器连接尚未注册，安装步骤仍需单独完成。"));
                } catch (...) {
                    QMessageBox::warning(&window,QStringLiteral("连接文件未能准备"),
                        QStringLiteral("本机设置已保存，但连接文件不可写或属于另一套配置。请检查下载器所在目录。"));
                }
            }
            window.statusBar()->showMessage(preferences->enabled
                ?QStringLiteral("本机接管设置已保存；浏览器连接组件仍需注册。")
                :QStringLiteral("已停止接收新的浏览器任务。"));
        } catch (...) {
            QMessageBox::warning(&window,QStringLiteral("设置无法应用"),QStringLiteral("请检查保存目录和设置文件是否可用；已添加的任务不受影响。"));
        }
    });
    QElapsedTimer clock; clock.start();
    QHash<QString, QPair<qint64, qint64>> measurements;
    auto selectedId = [&] {
        return tasks->selectedTaskId();
    };
    auto selectedTask = [&]() -> const downloader::Task* {
        const auto id = selectedId();
        for (const auto& task : manager->tasks()) if (task.id == id) return &task;
        return nullptr;
    };
    auto updateActions = [&] {
        const auto* task = selectedTask();
        const bool active = task && (task->state == downloader::TaskState::Running || task->state == downloader::TaskState::Queued);
        const bool resumable = task && (task->state == downloader::TaskState::Paused || task->state == downloader::TaskState::Interrupted
            || task->state == downloader::TaskState::Failed || task->state == downloader::TaskState::Cancelled);
        pause->setEnabled(active && (task->request.protocol == downloader::Protocol::Http || task->request.protocol == downloader::Protocol::BitTorrent||task->request.protocol==downloader::Protocol::Ed2k));
        const bool completedTorrent=task&&task->state==downloader::TaskState::Completed&&task->request.protocol==downloader::Protocol::BitTorrent;
        resume->setEnabled(resumable||(completedTorrent&&manager->canReseed(task->id))); retry->setEnabled(resumable);
        resume->setText(completedTorrent?QStringLiteral("重新校验并做种"):QStringLiteral("继续"));
        resume->setToolTip(completedTorrent?QStringLiteral("先在选项中启用做种条件。重新校验已有文件；缺失或损坏的数据可能重新下载。"):
            QStringLiteral("继续使用已保存的恢复数据下载。"));
        retry->setText(task && task->request.protocol == downloader::Protocol::BitTorrent ? QStringLiteral("重试并校验") : QStringLiteral("从头重试"));
        cancel->setEnabled(task && (active || task->state == downloader::TaskState::Paused || task->state == downloader::TaskState::Interrupted
            || task->state == downloader::TaskState::Failed));
        openFolder->setEnabled(task != nullptr);
        taskDetails->setEnabled(task != nullptr);
        verifyFile->setEnabled(task&&task->state==downloader::TaskState::Completed&&
            (task->request.protocol==downloader::Protocol::BitTorrent?QFileInfo(task->destination).isDir():QFileInfo(task->destination).isFile()));
    };
    auto refresh = [&](const QString& id) {
        const downloader::Task* current = nullptr;
        for (const auto& task : manager->tasks()) if (task.id == id) current = &task;
        if (!current) {tasks->updateTask(id,{}); measurements.remove(id); updateActions(); return; }
        tasks->updateTask(id,[&](int row){
        tasks->item(row, 0)->setText(QFileInfo(current->destination).fileName());
        tasks->item(row,0)->setData(Qt::UserRole+1,current->request.source);
        tasks->item(row, 1)->setText(current->request.protocol == downloader::Protocol::BitTorrent ? "BT"
            : current->request.protocol == downloader::Protocol::Ftp ? "FTP" : current->request.protocol==downloader::Protocol::Ed2k?"ed2k":"HTTP(S)");
        tasks->item(row, 2)->setText(current->state == downloader::TaskState::Completed ? "100 %"
            : current->total > 0 ? QStringLiteral("%1 %").arg(100.0 * current->received / current->total, 0, 'f', 1)
            : QStringLiteral("%1 MiB").arg(current->received / 1048576.0, 0, 'f', 1));
        tasks->item(row,2)->setData(downloader::taskSortRole,current->state==downloader::TaskState::Completed?100.0:
            current->total>0?100.0*current->received/current->total:-1.0);
        QString speed;
        double bytesPerSecond=-1;
        const auto now = clock.elapsed();
        const auto previous = measurements.value(id, {now, current->received});
        if (current->state == downloader::TaskState::Running && now > previous.first && current->received >= previous.second){
            bytesPerSecond=(current->received-previous.second)*1000.0/(now-previous.first);
            speed = QStringLiteral("%1 KiB/s").arg(bytesPerSecond/1024.0, 0, 'f', 1);
        }
        measurements[id] = {now, current->received};
        if(current->state==downloader::TaskState::Running&&current->phase==downloader::TransferPhase::Seeding){
            bytesPerSecond=qMax<qint64>(0,current->uploadBytesPerSecond);
            speed=QStringLiteral("↑ %1 KiB/s").arg(bytesPerSecond/1024.0,0,'f',1);
        }
        tasks->item(row, 3)->setText(speed);
        tasks->item(row,3)->setData(downloader::taskSortRole,bytesPerSecond);
        auto stateText = downloader::taskStateText(current->state);
        if (current->state == downloader::TaskState::Running) {
            if (current->phase == downloader::TransferPhase::FetchingMetadata) stateText = QStringLiteral("获取元数据");
            if (current->phase == downloader::TransferPhase::ChoosingFiles) stateText = QStringLiteral("等待选择文件");
            if (current->phase == downloader::TransferPhase::CheckingFiles) stateText = QStringLiteral("校验文件");
            if (current->phase == downloader::TransferPhase::Seeding) stateText = QStringLiteral("做种中");
        }
        tasks->item(row, 4)->setText(stateText);
        tasks->item(row, 4)->setToolTip(current->error);
        });
        updateActions();
    };
    QObject::connect(manager.get(), &downloader::TaskManager::changed, &window, refresh);
    QObject::connect(manager.get(), &downloader::TaskManager::storageError, &window, [&](const QString& error) {
        window.statusBar()->showMessage(QStringLiteral("任务存储失败，已停止接收新任务。"));
        QMessageBox::critical(&window, QStringLiteral("任务无法保存"), error);
    });
    for (const auto& task : manager->tasks()) refresh(task.id);
    QHash<QString,downloader::TaskState> notificationStates;
    for(const auto& task:manager->tasks())notificationStates.insert(task.id,task.state);
    QObject::connect(manager.get(),&downloader::TaskManager::changed,&window,[&](const QString& id){
        const downloader::Task* current=nullptr;
        for(const auto& task:manager->tasks())if(task.id==id){current=&task;break;}
        if(!current){notificationStates.remove(id);return;}
        const bool newlyCompleted=current->state==downloader::TaskState::Completed&&
            notificationStates.value(id,downloader::TaskState::Queued)!=downloader::TaskState::Completed;
        notificationStates.insert(id,current->state);
        if(newlyCompleted){
            const auto name=QFileInfo(current->destination).fileName();
            window.statusBar()->showMessage(QStringLiteral("下载完成：%1").arg(name));
            desktopTray.completed(name);
        }
    });
    QObject::connect(tasks, &QTableWidget::itemSelectionChanged, &window, updateActions);
    updateActions();
    QObject::connect(cancel, &QAction::triggered, &window, [&] {
        manager->cancel(selectedId());
    });
    QObject::connect(retry, &QAction::triggered, &window, [&] { manager->retry(selectedId()); });
    QObject::connect(taskDetails,&QAction::triggered,&window,[&]{
        if(const auto* task=selectedTask())downloader::showTaskDetails(&window,*task);
    });
    QObject::connect(tasks,&QTableWidget::cellDoubleClicked,&window,[&]{taskDetails->trigger();});
    QObject::connect(pause, &QAction::triggered, &window, [&] { manager->pause(selectedId()); });
    QObject::connect(resume, &QAction::triggered, &window, [&] { manager->resume(selectedId()); });
    QObject::connect(openFolder, &QAction::triggered, &window, [&] {
        if (const auto* task = selectedTask()) QDesktopServices::openUrl(QUrl::fromLocalFile(
            task->request.protocol == downloader::Protocol::BitTorrent ? task->destination : QFileInfo(task->destination).absolutePath()));
    });
    QObject::connect(verifyFile,&QAction::triggered,&window,[&]{
        const auto* task=selectedTask();
        if(!task||task->state!=downloader::TaskState::Completed)return;
        const auto destination=task->destination;const bool torrent=task->request.protocol==downloader::Protocol::BitTorrent;
        auto path=destination;
        if(torrent){
            const auto selected=QFileDialog::getOpenFileName(&window,QStringLiteral("选择任务内需要校验的文件"),destination);
            if(selected.isEmpty())return;
            path=downloader::verificationFileInDirectory(destination,selected);
            if(path.isEmpty()){QMessageBox::information(&window,QStringLiteral("检查文件"),QStringLiteral("请选择该BT任务目录内的普通文件。"));return;}
        }
        downloader::showSha256Verification(&window,path);
    });
    tasks->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(tasks, &QWidget::customContextMenuRequested, &window, [&](const QPoint& point) {
        const auto row=tasks->indexAt(point).row();if(row<0)return;tasks->setCurrentCell(row,0);
        if (!selectedTask()) return;
        QMenu menu(&window);
        menu.addAction(taskDetails);menu.addSeparator();
        if(manager->canReseed(selectedId()))menu.addAction(resume);
        menu.addAction(verifyFile);
        auto* open = menu.addAction(selectedTask()->request.protocol == downloader::Protocol::BitTorrent ? QStringLiteral("打开任务目录") : QStringLiteral("打开文件"));
        open->setEnabled(selectedTask()->state == downloader::TaskState::Completed && QFileInfo::exists(selectedTask()->destination));
        QObject::connect(open, &QAction::triggered, &menu, [&] { if (const auto* task = selectedTask()) QDesktopServices::openUrl(QUrl::fromLocalFile(task->destination)); });
        auto* high = menu.addAction(QStringLiteral("优先下载"));
        QObject::connect(high, &QAction::triggered, &menu, [&] { manager->setPriority(selectedId(), 10); });
        auto* normal = menu.addAction(QStringLiteral("普通优先级"));
        QObject::connect(normal, &QAction::triggered, &menu, [&] { manager->setPriority(selectedId(), 0); });
        auto* remove = menu.addAction(QStringLiteral("移除记录（保留文件）"));
        const auto state = selectedTask()->state;
        remove->setEnabled(state != downloader::TaskState::Running && state != downloader::TaskState::Cancelling
            && state != downloader::TaskState::Pausing);
        QObject::connect(remove, &QAction::triggered, &menu, [&] { manager->remove(selectedId()); });
        menu.exec(tasks->viewport()->mapToGlobal(point));
    });
    QObject::connect(manager.get(), &downloader::TaskManager::torrentFilesReady, &window,
        [&](const QString& id, const downloader::TorrentMetadata& metadata) {
            const auto priorities = downloader::chooseTorrentFiles(&window, metadata);
            if (!priorities) manager->pause(id);
            else if (!manager->selectTorrentFiles(id, *priorities))
                window.statusBar()->showMessage(QStringLiteral("任务已停止或文件选择未能保存。"));
        });
    auto addInput = [&](const QString& input,const QString& batchDirectory=QString{}) {
        const auto parsed = downloader::parseRequest(input);
        if (const auto* invalid = std::get_if<downloader::InvalidRequest>(&parsed)) {
            QMessageBox::information(&window, QStringLiteral("检查链接"), invalid->message);
            return;
        }
        auto request = std::get<downloader::DownloadRequest>(parsed);
        if (request.protocol == downloader::Protocol::Ed2k&&!ed2kPreferences.enabled) {
            QMessageBox::information(&window, QStringLiteral("ed2k 下载"), QStringLiteral("请先在“选项 → ed2k 设置”中配置并启用找源方式。"));
            return;
        }
        QVector<int> priorities;
        if (request.protocol == downloader::Protocol::BitTorrent && !request.source.startsWith("magnet:", Qt::CaseInsensitive)) {
            const auto inspected = downloader::inspectTorrent(request.source);
            const auto* metadata = std::get_if<downloader::TorrentMetadata>(&inspected);
            if (!metadata) { QMessageBox::information(&window, QStringLiteral("检查种子"), std::get<downloader::InvalidRequest>(inspected).message); return; }
            const auto selected = downloader::chooseTorrentFiles(&window, *metadata);
            if (!selected) return;
            priorities = *selected; request.identities = metadata->identities; request.suggestedName = metadata->name;
        }
        // A remote name is only a suggestion, never an automatically trusted path.
        auto name = request.suggestedName;
        name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), "_");
        if (name.isEmpty() || name == "." || name == "..") name = request.protocol == downloader::Protocol::BitTorrent ? "BT-download" : "download";
        QString destination;
        if (request.protocol == downloader::Protocol::BitTorrent) {
            const auto parent = batchDirectory.isEmpty()?QFileDialog::getExistingDirectory(&window, QStringLiteral("选择保存位置")):batchDirectory;
            if (parent.isEmpty()) return;
            bool accepted = false;
            const auto folder = QInputDialog::getText(&window, QStringLiteral("下载目录"), QStringLiteral("新任务目录名"), QLineEdit::Normal, name, &accepted).trimmed();
            if (!accepted) return;
            if (folder.isEmpty() || folder == "." || folder == ".." || folder.contains(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")))
                || folder.endsWith('.')) {
                QMessageBox::information(&window, QStringLiteral("检查目录名"), QStringLiteral("请输入不含路径或特殊字符的新目录名。")); return;
            }
            destination = QDir(parent).filePath(folder);
        } else destination = batchDirectory.isEmpty()?QFileDialog::getSaveFileName(&window, QStringLiteral("保存到"), name):QDir(batchDirectory).filePath(name);
        if (destination.isEmpty()) return;
        if (QFileInfo::exists(destination)) {
            QMessageBox::information(&window, QStringLiteral("文件已存在"), QStringLiteral("请选择其他文件名，已有文件不会被覆盖。"));
            return;
        }
        const auto result = manager->add(request, destination, 0, priorities);
        if (!result.error.isEmpty()) QMessageBox::information(&window, QStringLiteral("添加任务"), result.error);
        for (int row = 0; row < tasks->rowCount(); ++row)
            if (tasks->item(row, 0)->data(Qt::UserRole).toString() == result.id) tasks->setCurrentCell(row, 0);
    };
    auto addInputs = [&](const QStringList& lines) {
        const auto prepared=downloader::prepareBatch(lines);
        if(const auto* error=std::get_if<downloader::InvalidRequest>(&prepared)){
            QMessageBox::information(&window,QStringLiteral("检查输入"),error->message);return;
        }
        const auto& inputs=std::get<QStringList>(prepared);
        if(inputs.isEmpty())return;
        QString directory;
        if(inputs.size()>1){directory=QFileDialog::getExistingDirectory(&window,QStringLiteral("选择这%1项任务的保存目录").arg(inputs.size()));if(directory.isEmpty())return;}
        for(const auto& input:inputs)addInput(input,directory);
    };
    window.dropped=addInputs;
    QObject::connect(add, &QAction::triggered, &window, [&] {
        bool accepted = false;
        const auto input = QInputDialog::getMultiLineText(&window, QStringLiteral("添加下载"),
            QStringLiteral("下载链接、磁力链或种子路径，每行一项（最多100项）"), {}, &accepted);
        if (accepted) {
            if(input.size()>65536){QMessageBox::information(&window,QStringLiteral("添加下载"),QStringLiteral("输入内容过长，请分批添加。"));return;}
            addInputs(input.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),Qt::SkipEmptyParts));
        }
    });
    QObject::connect(addTorrent, &QAction::triggered, &window, [&] {
        const auto paths = QFileDialog::getOpenFileNames(&window, QStringLiteral("打开种子"), {}, QStringLiteral("BT 种子 (*.torrent)"));
        if (!paths.isEmpty()) addInputs(paths);
    });
    window.show();

    if (parser.isSet("smoke-report")) {
        QTimer::singleShot(300, &app, [&] {
            QFile report(parser.value("smoke-report"));
            if (!report.open(QIODevice::WriteOnly)) {
                app.exit(2);
                return;
            }
            QJsonObject result{{"qt_version", qVersion()},
                {"platform", QApplication::platformName()},
                {"window_visible", window.isVisible()},
                {"direct_download_limit",qint64(manager->httpDownloadLimit())},
                {"bt_download_limit",manager->torrentRateLimits().download},
                {"bt_upload_limit",manager->torrentRateLimits().upload},
                {"seeding_seconds",manager->seedingPolicy().seconds},
                {"seeding_ratio_percent",manager->seedingPolicy().ratioPercent},
                {"seeding_configuration_valid",seedingStartupError.isEmpty()},
                {"bandwidth_configuration_valid",bandwidthStartupError.isEmpty()},
                {"low_usage_mode",bandwidthPreferences.lowUsage},
                {"system_tray_available",desktopTray.available()},
                {"tray_action_enabled",toTray->isEnabled()},
                {"task_columns", tasks->columnCount()},
                {"table_layout_valid",tableLayoutValid},
                {"table_sort_column",tasks->horizontalHeader()->sortIndicatorSection()},
                {"table_sort_order",int(tasks->horizontalHeader()->sortIndicatorOrder())},
                {"task_rows", tasks->rowCount()},
                {"download_engine_connected", true},
                {"ed2k_enabled",ed2kPreferences.enabled},
                {"ed2k_source_mode",ed2kPreferences.options.useKad?"kad":"server"},
                {"ed2k_configuration_valid",downloader::validEd2kOptions(downloader::ed2kRuntimeOptions(ed2kPreferences))},
                {"ed2k_settings_available",ed2kSettings->isEnabled()},
                {"qt_plugins_path", QLibraryInfo::path(QLibraryInfo::PluginsPath)}};
            QJsonArray tableOrder,tableWidths;
            for(int i=0;i<tasks->columnCount();++i){tableOrder.append(tasks->horizontalHeader()->logicalIndex(i));tableWidths.append(tasks->columnWidth(i));}
            result.insert("table_column_order",tableOrder);result.insert("table_column_widths",tableWidths);
            result.insert("window_capture_saved", window.grab().save(parser.value("smoke-report") + ".png"));
            const auto bytes = QJsonDocument(result).toJson();
            const bool written = report.write(bytes) == bytes.size();
            report.close();
            app.exit(written && window.isVisible() ? 0 : 3);
        });
    }
    const int exitCode = app.exec();
    manager.reset();
    return exitCode;
}
