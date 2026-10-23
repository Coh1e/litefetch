#pragma once
#include "kad_bootstrap.h"
namespace downloader::kad {
QByteArray lookupRequest(const QByteArray& target,const QByteArray& receiverId,quint8 count=20);
using ContactsResult=std::variant<QVector<Contact>,InvalidDatagram>;
ContactsResult parseLookup(QByteArrayView datagram,const QByteArray& target,quint8 maxContacts=20);
QByteArray xorDistance(const QByteArray& left,const QByteArray& right);
struct LookupResult {QVector<Contact> responsive;int queries=0;bool budgetExhausted=false;};
// Bounded sequential shortlist walk; returned nodes replied, but are not
// cryptographically authenticated. No persistent routing table is implied.
LookupResult findClosest(const QByteArray& target,const QVector<Contact>& seeds,
    int totalTimeoutMs=10000,int maxQueries=32,int queryTimeoutMs=1000,const CancelCheck& cancelled={});
}
