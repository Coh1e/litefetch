#pragma once
#include "kad_bootstrap.h"
#include "kad_lookup.h"
#include "kad_sources.h"
#include <QHostAddress>
namespace downloader::kad {
// One bounded, unobfuscated query to an explicitly supplied seed. Returned
// contacts are candidates, not authenticated routing entries or download sources.
Bootstrap bootstrap(const QHostAddress& seed,quint16 port,int timeoutMs=3000,const CancelCheck& cancelled={});
QVector<Contact> lookup(const Contact& receiver,const QByteArray& target,int timeoutMs=3000,const CancelCheck& cancelled={});
// Receives one bounded source-result datagram, not the complete multi-page result stream.
SourcePage querySourcesPage(const Contact& receiver,const QByteArray& hash,quint64 size,
    quint16 start=0,int timeoutMs=3000,const CancelCheck& cancelled={});
struct SourceCollection {
    QVector<FileSource> sources;
    int pages=0;
    bool quietEnd=false,limitReached=false;
};
// UDP has no final-page acknowledgement: quietEnd is an idle heuristic, not
// proof that every remote result arrived. Duplicates never reset the idle timer.
SourceCollection collectSources(const Contact& receiver,const QByteArray& hash,quint64 size,
    int timeoutMs=2000,int quietMs=100,int maxPages=6,const CancelCheck& cancelled={});
struct DiscoveryResult {QVector<FileSource> sources;int nodeQueries=0,sourceQueries=0,failedQueries=0;bool limitReached=false;};
DiscoveryResult discoverSources(const QByteArray& hash,quint64 size,const QVector<Contact>& seeds,
    int totalTimeoutMs=10000,int maxSourceNodes=4,int queryTimeoutMs=1000,const CancelCheck& cancelled={});
}
