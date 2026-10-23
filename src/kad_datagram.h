#pragma once
#include <QByteArray>
#include <QByteArrayView>
namespace downloader::kad {
// Returns canonical E4 framing. Both wire input and inflated output are bounded.
// Throws on malformed framing, incomplete/trailing streams or expansion overflow.
QByteArray unpackDatagram(QByteArrayView bytes,qsizetype limit);
}
