#include "disk_space.h"
#include "torrent_space.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <limits>
#include <iostream>
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    QTemporaryDir dir(QCoreApplication::applicationDirPath()+"/disk-space-XXXXXX");
    if(!dir.isValid())return 1;
    const auto nested=dir.filePath("missing/nested/file.bin");
    if(!downloader::insufficientDiskSpace(nested,0).isEmpty())return 2;
    if(!downloader::insufficientDiskSpace(nested,1).isEmpty())return 3;
    const auto huge=(std::numeric_limits<qint64>::max)();
    if(downloader::insufficientDiskSpace(nested,huge).isEmpty())return 4;
    try{downloader::requireDiskSpace(nested,huge);return 5;}
    catch(const downloader::DiskSpaceError& error){if(error.message().isEmpty())return 6;}
    if(QFile::exists(nested)||QDir(dir.filePath("missing")).exists())return 7;
    using downloader::torrentSpaceUpperBound;
    if(torrentSpaceUpperBound({{1,3},{5,7}},4,12)!=8)return 8;
    if(torrentSpaceUpperBound({{1,3},{3,6}},4,12)!=8)return 9;
    if(torrentSpaceUpperBound({{9,10}},4,10)!=2)return 10;
    if(torrentSpaceUpperBound({},4,10)!=0)return 11;
    if(torrentSpaceUpperBound({{7,7}},4,10)!=0)return 12;
    if(torrentSpaceUpperBound({{9,10},{0,1}},4,10)!=6)return 13;
    try{torrentSpaceUpperBound({{0,11}},4,10);return 14;}
    catch(const std::invalid_argument&){}
    std::cout<<"Disk preflight reports shortage without creating output paths\n";
}
