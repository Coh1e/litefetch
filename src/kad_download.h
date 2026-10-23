#pragma once
#include "ed2k_download.h"
#include "kad_client.h"
namespace downloader::kad {
struct DownloadOptions {
    QVector<QPair<QHostAddress,quint16>> bootstrapSeeds;
    int discoveryTimeoutMs=5000;
    int maxSourceNodes=4;
};
// In-process worker API. No server login or resource-name search.
ed2k::DownloadResult downloadFile(const ed2k::ServerDownload& request,
    const DownloadOptions& options,std::stop_token stop={});
}
