#pragma once
#include <QVector>
#include <QPair>
#include <algorithm>
#include <stdexcept>

namespace downloader {
// Selected file intervals in the torrent's linear address space. Include the
// whole intersecting piece, since boundary bytes may be stored in a partfile.
inline qint64 torrentSpaceUpperBound(QVector<QPair<qint64,qint64>> intervals,
    qint64 pieceLength,qint64 total)
{
    if(pieceLength<=0||total<0)throw std::invalid_argument("Invalid torrent space layout");
    for(auto& interval:intervals){
        if(interval.first<0||interval.second<interval.first||interval.second>total)
            throw std::invalid_argument("Invalid torrent space interval");
        if(interval.first==interval.second)continue;
        interval.first-=interval.first%pieceLength;
        const auto extra=(pieceLength-interval.second%pieceLength)%pieceLength;
        interval.second+=qMin(extra,total-interval.second);
    }
    std::sort(intervals.begin(),intervals.end());
    qint64 result=0,end=0;
    for(const auto& interval:intervals){
        if(interval.second<=end)continue;
        result+=interval.second-qMax(end,interval.first);
        end=interval.second;
    }
    return result;
}
}
