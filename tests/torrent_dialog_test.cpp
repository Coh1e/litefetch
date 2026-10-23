#include "torrent_dialog.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTableView>
#include <QTextStream>
#include <QTimer>

using namespace downloader;
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const char* text) { if (!ok) { ++failures; QTextStream(stderr) << "FAIL: " << text << '\n'; } };
    const TorrentMetadata metadata{QStringLiteral("文件选择测试"), {}, {},
        {{"a.bin", 1024 * 1024, false}, {"b.bin", 2048 * 1024, false}, {".pad/16", 16, true}}, 3 * 1024 * 1024};
    QTimer::singleShot(100, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { check(false, "native selection dialog is active"); app.exit(2); return; }
        auto* table = dialog->findChild<QTableView*>("torrentFiles");
        auto* buttons = dialog->findChild<QDialogButtonBox*>("torrentFileButtons");
        if (!table || !buttons) { check(false, "dialog controls exist"); dialog->reject(); return; }
        auto* model = table->model();
        check(model->rowCount() == 2, "padding files excluded from user choices");
        model->setData(model->index(0, 0), Qt::Unchecked, Qt::CheckStateRole);
        model->setData(model->index(1, 0), Qt::Unchecked, Qt::CheckStateRole);
        check(!buttons->button(QDialogButtonBox::Ok)->isEnabled(), "cannot start with no selected file");
        model->setData(model->index(0, 2), 7, Qt::EditRole);
        check(buttons->button(QDialogButtonBox::Ok)->isEnabled(), "priority enables selected file");
        if (app.arguments().size() > 1) check(dialog->grab().save(app.arguments()[1]), "save native dialog image");
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    const auto result = chooseTorrentFiles(nullptr, metadata);
    if (!result || *result != QVector<int>({7,0,0})) {
        QTextStream diagnostic(stderr);
        diagnostic << "Selection result: ";
        if (!result) diagnostic << "declined";
        else for (const auto priority : *result) diagnostic << priority << ',';
        diagnostic << '\n';
    }
    check(result && *result == QVector<int>({7, 0, 0}), "dialog returns exact priorities including hidden padding slots");
    QTimer::singleShot(100, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog) dialog->reject(); else { check(false, "cancel dialog is active"); app.exit(2); }
    });
    check(!chooseTorrentFiles(nullptr, metadata).has_value(), "declining choice is distinct from selecting no files");
    QTextStream(stdout) << "Torrent native dialog checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
