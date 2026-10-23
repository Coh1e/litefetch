#include "kad_download.h"
#include <QDeadlineTimer>
#include <stdexcept>
namespace downloader::kad {
ed2k::DownloadResult downloadFile(const ed2k::ServerDownload& request,const DownloadOptions& options,std::stop_token stop){
    if(options.bootstrapSeeds.isEmpty()||options.bootstrapSeeds.size()>8||options.discoveryTimeoutMs<3||options.discoveryTimeoutMs>60000||options.maxSourceNodes<1||options.maxSourceNodes>20)
        throw std::invalid_argument("Invalid Kad download discovery options");
    auto run=request;run.server={};
    run.sourceProvider=[&request,options,stop]{
        const auto cancelled=[&]{return stop.stop_requested()||(request.cancelled&&request.cancelled());};
        const auto checkStop=[&]{if(cancelled())throw std::runtime_error("Kad discovery cancelled");};
        QDeadlineTimer deadline(options.discoveryTimeoutMs);QVector<Contact> nodes;
        for(const auto& seed:options.bootstrapSeeds){
            checkStop();if(deadline.hasExpired())break;
            try{
                const auto timeout=static_cast<int>(qMin<qint64>(1000,qMax<qint64>(1,deadline.remainingTime()/3)));
                const auto result=bootstrap(seed.first,seed.second,timeout,cancelled);
                if(nodes.size()<256)nodes.append({result.id,seed.first.toIPv4Address(),seed.second,result.tcpPort,result.version});
                for(const auto& contact:result.contacts)if(nodes.size()<256)nodes.append(contact);
            }catch(const std::runtime_error&){checkStop();/* Other explicitly configured seeds may still respond. */}
        }
        checkStop();const auto remaining=deadline.remainingTime();if(nodes.isEmpty()||remaining<2)throw std::runtime_error("Kad bootstrap produced no query budget");
        const auto found=discoverSources(request.fileHash,request.size,nodes,static_cast<int>(remaining),options.maxSourceNodes,qMin(1000,static_cast<int>(remaining)),cancelled);
        checkStop();QVector<ed2k::PeerEndpoint> peers;
        for(const auto& source:found.sources)if(source.directlyUsable())peers.append({QHostAddress(source.ipv4),source.tcpPort,source.userHash});
        return peers;
    };
    return ed2k::download(run,stop);
}
}
