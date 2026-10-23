#pragma once
#include "torrent_engine.h"
#include <optional>
class QWidget;
namespace downloader {
std::optional<QVector<int>> chooseTorrentFiles(QWidget* parent, const TorrentMetadata& metadata);
}
