#include "torrent_dialog.h"
#include <QAbstractTableModel>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>

namespace downloader {
namespace {
const QStringList priorityNames{QStringLiteral("跳过"), QStringLiteral("最低"), QStringLiteral("低"), QStringLiteral("较低"),
    QStringLiteral("普通"), QStringLiteral("较高"), QStringLiteral("高"), QStringLiteral("最高")};
class FileModel : public QAbstractTableModel {
public:
    TorrentMetadata metadata;
    QVector<int> priorities;
    QVector<qsizetype> rows;
    explicit FileModel(TorrentMetadata value, QObject* parent) : QAbstractTableModel(parent), metadata(std::move(value)) {
        for (qsizetype i = 0; i < metadata.files.size(); ++i) {
            priorities.append(metadata.files[i].padding ? 0 : 4);
            if (!metadata.files[i].padding) rows.append(i);
        }
    }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(rows.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
            return QStringList{QStringLiteral("文件"), QStringLiteral("大小"), QStringLiteral("优先级")}.value(section);
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= rows.size()) return {};
        const auto position = rows[index.row()]; const auto& file = metadata.files[position];
        if (role == Qt::CheckStateRole && index.column() == 0) return priorities[position] ? Qt::Checked : Qt::Unchecked;
        if (role == Qt::EditRole && index.column() == 2) return priorities[position];
        if (role == Qt::DisplayRole) {
            if (index.column() == 0) return file.path;
            if (index.column() == 1) return QStringLiteral("%1 MiB").arg(file.size / 1048576.0, 0, 'f', 2);
            if (index.column() == 2) return priorityNames.value(priorities[position]);
        }
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        auto result = QAbstractTableModel::flags(index);
        if (index.column() == 0) result |= Qt::ItemIsUserCheckable;
        if (index.column() == 2) result |= Qt::ItemIsEditable;
        return result;
    }
    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (!index.isValid() || index.row() >= rows.size()) return false;
        int priority;
        if (index.column() == 0 && role == Qt::CheckStateRole) priority = value.toInt() == Qt::Checked ? 4 : 0;
        else if (index.column() == 2 && role == Qt::EditRole) priority = value.toInt();
        else return false;
        if (priority < 0 || priority > 7) return false;
        priorities[rows[index.row()]] = priority;
        emit dataChanged(this->index(index.row(), 0), this->index(index.row(), 2)); return true;
    }
    void setAll(int value) {
        for (const auto position : rows) priorities[position] = value;
        if (!rows.isEmpty()) emit dataChanged(index(0, 0), index(static_cast<int>(rows.size() - 1), 2));
    }
};
class PriorityEditor : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const override {
        auto* editor = new QComboBox(parent);
        for (int i = 0; i <= 7; ++i) editor->addItem(priorityNames[i], i);
        return editor;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        static_cast<QComboBox*>(editor)->setCurrentIndex(index.data(Qt::EditRole).toInt());
    }
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
        model->setData(index, static_cast<QComboBox*>(editor)->currentData(), Qt::EditRole);
    }
};
}
std::optional<QVector<int>> chooseTorrentFiles(QWidget* parent, const TorrentMetadata& metadata)
{
    QDialog dialog(parent); dialog.setWindowTitle(QStringLiteral("选择下载文件")); dialog.resize(720, 440);
    auto* layout = new QVBoxLayout(&dialog);
    auto* name = new QLabel(metadata.name, &dialog); name->setTextFormat(Qt::PlainText); layout->addWidget(name);
    FileModel model(metadata, &dialog);
    auto* table = new QTableView(&dialog); table->setObjectName("torrentFiles"); table->setModel(&model);
    table->setItemDelegateForColumn(2, new PriorityEditor(table));
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(table);
    auto* controls = new QHBoxLayout;
    auto* all = new QPushButton(QStringLiteral("全选"), &dialog);
    auto* none = new QPushButton(QStringLiteral("全部取消"), &dialog);
    auto* summary = new QLabel(&dialog); summary->setObjectName("torrentSelectionSummary");
    controls->addWidget(all); controls->addWidget(none); controls->addWidget(summary); controls->addStretch(); layout->addLayout(controls);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName("torrentFileButtons");
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("开始下载"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("暂不下载"));
    auto refresh = [&] {
        qint64 size = 0; int count = 0;
        for (qsizetype i = 0; i < model.priorities.size(); ++i) if (model.priorities[i] && !metadata.files[i].padding) {
            size += metadata.files[i].size; ++count;
        }
        summary->setText(QStringLiteral("已选 %1 个文件，%2 MiB").arg(count).arg(size / 1048576.0, 0, 'f', 2));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(validateTorrentSelection(metadata, model.priorities).isEmpty());
    };
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &dialog, refresh);
    QObject::connect(all, &QPushButton::clicked, &dialog, [&] { model.setAll(4); });
    QObject::connect(none, &QPushButton::clicked, &dialog, [&] { model.setAll(0); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons); refresh();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return model.priorities;
}
}
