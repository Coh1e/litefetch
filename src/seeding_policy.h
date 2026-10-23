#pragma once
#include <QtTypes>

namespace downloader {
struct SeedingPolicy {
    int seconds=0;
    int ratioPercent=0;
};
inline bool validSeedingPolicy(SeedingPolicy policy)
{
    return policy.seconds>=0&&policy.seconds<=7*24*3600&&policy.ratioPercent>=0&&policy.ratioPercent<=100000;
}
inline bool seedingLimitReached(SeedingPolicy policy,qint64 elapsedMs,qint64 uploaded,qint64 wanted)
{
    if(policy.seconds==0&&policy.ratioPercent==0)return true;
    if(wanted==0)return true;
    if(policy.seconds>0&&elapsedMs>=qint64(policy.seconds)*1000)return true;
    if(policy.ratioPercent>0&&wanted>0&&uploaded>=0
        &&static_cast<long double>(uploaded)*100>=static_cast<long double>(wanted)*policy.ratioPercent)return true;
    return false;
}
}
