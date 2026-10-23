#pragma once
#include "seeding_policy.h"
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <stdexcept>
namespace downloader {
inline SeedingPolicy loadSeedingPolicy(const QString& path)
{
    QFile input(path);if(!input.exists())return {};
    if(!input.open(QIODevice::ReadOnly)||input.size()>4096)throw std::runtime_error("Cannot read seeding settings");
    const auto object=QJsonDocument::fromJson(input.readAll()).object();
    const auto integer=[](QJsonValue value,int maximum){return value.isDouble()&&value.toDouble()>=0
        &&value.toDouble()<=maximum&&value.toDouble()==value.toInt(-1);};
    if(object.value("version")!=QJsonValue(1)||!integer(object.value("seconds"),604800)
        ||!integer(object.value("ratioPercent"),100000))throw std::runtime_error("Invalid seeding settings");
    return {object.value("seconds").toInt(),object.value("ratioPercent").toInt()};
}
inline void saveSeedingPolicy(const QString& path,SeedingPolicy policy)
{
    if(!validSeedingPolicy(policy))throw std::invalid_argument("Invalid seeding policy");
    const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"seconds",policy.seconds},{"ratioPercent",policy.ratioPercent}}).toJson(QJsonDocument::Compact);
    QSaveFile output(path);output.setDirectWriteFallback(false);
    if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit())
        throw std::runtime_error("Cannot save seeding settings");
}
}
