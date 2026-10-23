#pragma once
#include <QHeaderView>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>

namespace downloader {
inline bool loadTableLayout(const QString& path,QHeaderView& header)
{
    if(!QFile::exists(path))return true;
    QFile input(path);
    if(!input.open(QIODevice::ReadOnly)||input.size()>4096)return false;
    const auto object=QJsonDocument::fromJson(input.readAll()).object();
    const auto order=object.value("order").toArray(),widths=object.value("widths").toArray();
    const auto integer=[](const QJsonValue& value,int minimum,int maximum){
        return value.isDouble()&&value.toDouble()>=minimum&&value.toDouble()<=maximum
            &&value.toDouble()==value.toInt(-1);
    };
    const auto count=header.count();
    if(object.value("version").toInt()!=1||order.size()!=count||widths.size()!=count
        ||!integer(object.value("sortColumn"),0,count-1)||!integer(object.value("sortOrder"),0,1))return false;
    QSet<int> seen;
    for(int i=0;i<count;++i){
        if(!integer(order[i],0,count-1)||seen.contains(order[i].toInt())||!integer(widths[i],40,2000))return false;
        seen.insert(order[i].toInt());
    }
    // Validate the entire document before changing any visible layout.
    for(int visual=0;visual<count;++visual)header.moveSection(header.visualIndex(order[visual].toInt()),visual);
    for(int logical=1;logical<count;++logical)header.resizeSection(logical,widths[logical].toInt());
    header.setSortIndicator(object.value("sortColumn").toInt(),Qt::SortOrder(object.value("sortOrder").toInt()));
    return true;
}
inline bool saveTableLayout(const QString& path,const QHeaderView& header)
{
    QJsonArray order,widths;
    for(int i=0;i<header.count();++i){order.append(header.logicalIndex(i));widths.append(qBound(40,header.sectionSize(i),2000));}
    const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"order",order},{"widths",widths},
        {"sortColumn",header.sortIndicatorSection()},{"sortOrder",int(header.sortIndicatorOrder())}}).toJson(QJsonDocument::Compact);
    QSaveFile output(path);output.setDirectWriteFallback(false);
    return output.open(QIODevice::WriteOnly)&&output.write(bytes)==bytes.size()&&output.commit();
}
}
