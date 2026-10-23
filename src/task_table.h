#pragma once
#include <QTableWidget>
#include <QHeaderView>
#include <QSignalBlocker>
#include <functional>
namespace downloader {
inline constexpr int taskSortRole=Qt::UserRole+2;
class TaskTableItem final : public QTableWidgetItem {
public:
    bool operator<(const QTableWidgetItem& other) const override{
        const auto left=data(taskSortRole),right=other.data(taskSortRole);
        if(left.isValid()&&right.isValid())return left.toDouble()<right.toDouble();
        return QString::localeAwareCompare(text().toCaseFolded(),other.text().toCaseFolded())<0;
    }
};
class TaskTable final : public QTableWidget {
public:
    explicit TaskTable(QWidget* parent=nullptr):QTableWidget(0,5,parent){
        connect(model(),&QAbstractItemModel::layoutChanged,this,[this]{applyFilter();});
    }
    QString selectedTaskId() const{
        const auto* item=currentRow()<0?nullptr:this->item(currentRow(),0);
        return item?item->data(Qt::UserRole).toString():QString{};
    }
    void setTaskFilter(const QString& text){query_=text.trimmed();applyFilter();}
    // Populate every cell while sorting is suspended, then restore by identity.
    // Empty populate removes a record and never selects an unrelated neighbor.
    void updateTask(const QString& id,const std::function<void(int)>& populate){
        const auto selected=selectedTaskId();const bool sorting=isSortingEnabled();
        {
            QSignalBlocker blocked(this);setSortingEnabled(false);int row=findRow(id);
            if(!populate){if(row>=0)removeRow(row);}
            else{
                if(row<0){row=rowCount();insertRow(row);for(int column=0;column<columnCount();++column)setItem(row,column,new TaskTableItem);
                    item(row,0)->setData(Qt::UserRole,id);}
                populate(row);
            }
            setSortingEnabled(sorting);applyFilter();
            const auto selectedRow=findRow(selected);
            if(selectedRow>=0&&!isRowHidden(selectedRow))setCurrentCell(selectedRow,0);
            else{clearSelection();setCurrentCell(-1,-1);}
        }
    }
private:
    QString query_;
    int findRow(const QString& id) const{
        if(id.isEmpty())return -1;
        for(int row=0;row<rowCount();++row)if(item(row,0)&&item(row,0)->data(Qt::UserRole).toString()==id)return row;
        return -1;
    }
    void applyFilter(){
        for(int row=0;row<rowCount();++row){const auto* name=item(row,0);
            const bool matches=name&&(name->text().contains(query_,Qt::CaseInsensitive)||name->data(Qt::UserRole+1).toString().contains(query_,Qt::CaseInsensitive));
            setRowHidden(row,!matches);
        }
        if(currentRow()>=0&&isRowHidden(currentRow())){clearSelection();setCurrentCell(-1,-1);}
    }
};
}
