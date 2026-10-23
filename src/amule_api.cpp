#include "amule_api.h"
#include "curl_runtime.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <memory>
#include <algorithm>

namespace downloader {
namespace {
constexpr qsizetype maxReply = 16 * 1024 * 1024;
size_t append(char* data, size_t size, size_t count, void* user) noexcept
{
    try {
        auto& bytes = *static_cast<QByteArray*>(user);
        if (size && count > static_cast<size_t>(maxReply) / size) return 0;
        const auto length = size * count;
        if (length > static_cast<size_t>(maxReply - bytes.size())) return 0;
        bytes.append(data, static_cast<qsizetype>(length)); return length;
    } catch (...) { return 0; }
}
qint64 number(const QJsonObject& object, const char* key)
{
    const auto value = object.value(key);
    const auto integer = value.toInteger(-1);
    if (!value.isDouble() || integer < 0) throw AmuleApiError("Invalid aMule numeric field");
    return integer;
}
}
AmuleApi::AmuleApi(quint16 port, const QByteArray& password) : port_(port)
{
    if (!port || password.isEmpty() || password.size() > 1024)
        throw AmuleApiError("Invalid aMule API configuration");
    const QJsonObject body{{"password", QString::fromUtf8(password)}};
    const auto reply = call("POST", "/auth/login?include_token=true", &body, false);
    const auto token = reply.value("token").toString().toUtf8();
    if (token.isEmpty() || token.size() > 8192
        || std::any_of(token.begin(), token.end(), [](char character) { return character < 0x21 || character > 0x7e; }))
        throw AmuleApiError("Invalid aMule authentication response");
    token_ = token;
}
QJsonObject AmuleApi::call(const QByteArray& method, const QByteArray& path,
    const QJsonObject* body, bool authenticated)
{
    if (detail::initializeCurl() != CURLE_OK) throw AmuleApiError("HTTP runtime unavailable");
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw AmuleApiError("HTTP client unavailable");
    const QByteArray url = "http://127.0.0.1:" + QByteArray::number(port_) + "/api/v1" + path;
    const QByteArray payload = body ? QJsonDocument(*body).toJson(QJsonDocument::Compact) : QByteArray{};
    curl_slist* rawHeaders = nullptr;
    auto header = [&](const QByteArray& value) {
        auto* next = curl_slist_append(rawHeaders, value.constData());
        if (!next) { curl_slist_free_all(rawHeaders); rawHeaders = nullptr; throw AmuleApiError("HTTP headers unavailable"); }
        rawHeaders = next;
    };
    if (authenticated) header("Authorization: Bearer " + token_);
    if (body) header("Content-Type: application/json");
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(rawHeaders, curl_slist_free_all);
    QByteArray response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.constData());
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.constData());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    if (body) {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.constData());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    }
    if (curl_easy_perform(curl.get()) != CURLE_OK) throw AmuleApiError("aMule API connection failed");
    long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw AmuleApiError("aMule API request rejected", status);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(response, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) throw AmuleApiError("Invalid aMule API response");
    return document.object();
}
bool AmuleApi::connected() { return call("GET", "/health").value("ec_connected").toBool(); }
QByteArray AmuleApi::taskPath(const QString& hash)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{32}$"));
    if (!pattern.match(hash).hasMatch()) throw AmuleApiError("Invalid ed2k identity");
    return "/downloads/" + hash.toLower().toLatin1();
}
Ed2kSnapshot AmuleApi::parseSnapshot(const QJsonObject& object)
{
    Ed2kSnapshot snapshot{object.value("hash").toString().toLower(), object.value("name").toString(),
        object.value("status").toString(), object.value("priority").toString(),
        number(object,"size_bytes"), number(object,"completed_bytes"), number(object,"speed_bytes_per_second")};
    taskPath(snapshot.hash);
    if (snapshot.name.isEmpty() || snapshot.status.isEmpty() || snapshot.completed > snapshot.size)
        throw AmuleApiError("Invalid aMule task snapshot");
    return snapshot;
}
Ed2kSnapshot AmuleApi::task(const QString& hash)
{
    auto result = parseSnapshot(call("GET", taskPath(hash)));
    if (result.hash != hash.toLower()) throw AmuleApiError("aMule task identity mismatch");
    return result;
}
void AmuleApi::add(const DownloadRequest& request)
{
    const auto parsed = parseRequest(request.source);
    const auto* valid = std::get_if<DownloadRequest>(&parsed);
    if (request.protocol != Protocol::Ed2k || !valid || valid->protocol != Protocol::Ed2k) throw AmuleApiError("Invalid ed2k request");
    const QJsonObject body{{"links", QJsonArray{valid->source}}};
    const auto results = call("POST", "/downloads", &body).value("results").toArray();
    if (results.size() != 1 || !results.first().toObject().value("ok").toBool())
        throw AmuleApiError("aMule rejected ed2k task");
}
void AmuleApi::action(const QString& hash, const QString& action)
{
    const QJsonObject body{{"action", action}}; call("PATCH", taskPath(hash), &body);
}
void AmuleApi::pause(const QString& hash) { action(hash, "pause"); }
void AmuleApi::resume(const QString& hash) { action(hash, "resume"); }
void AmuleApi::priority(const QString& hash, const QString& value)
{
    if (value != "low" && value != "normal" && value != "high") throw AmuleApiError("Invalid ed2k priority");
    const QJsonObject body{{"priority", value}}; call("PATCH", taskPath(hash), &body);
}
}
