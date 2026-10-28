#include "native_bridge.h"

#include <nlohmann/json.hpp>

#include <istream>
#include <ostream>

namespace litefetch {
namespace {
bool isSupportedScheme(const std::string& url)
{
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0
        || url.rfind("ftp://", 0) == 0 || url.rfind("ftps://", 0) == 0
        || url.rfind("magnet:", 0) == 0 || url.rfind("ed2k://", 0) == 0;
}
} // namespace

bool readNativeMessage(std::istream& input, std::string& payload, std::string& error)
{
    unsigned char header[4]{};
    input.read(reinterpret_cast<char*>(header), 4);
    if (input.gcount() != 4) {
        error = "the browser closed the channel";
        return false;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(header[0])
        | (static_cast<std::uint32_t>(header[1]) << 8)
        | (static_cast<std::uint32_t>(header[2]) << 16)
        | (static_cast<std::uint32_t>(header[3]) << 24);
    if (length == 0) {
        error = "empty native message";
        return false;
    }
    if (length > maxNativeMessageBytes) {
        error = "native message is larger than the browser allows";
        return false;
    }
    payload.assign(length, '\0');
    input.read(payload.data(), static_cast<std::streamsize>(length));
    if (static_cast<std::uint32_t>(input.gcount()) != length) {
        error = "native message was cut short";
        return false;
    }
    return true;
}

bool writeNativeMessage(std::ostream& output, std::string_view payload)
{
    if (payload.size() > maxNativeMessageBytes) return false;
    const auto length = static_cast<std::uint32_t>(payload.size());
    const unsigned char header[4] = {
        static_cast<unsigned char>(length & 0xff),
        static_cast<unsigned char>((length >> 8) & 0xff),
        static_cast<unsigned char>((length >> 16) & 0xff),
        static_cast<unsigned char>((length >> 24) & 0xff),
    };
    output.write(reinterpret_cast<const char*>(header), 4);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.flush();
    return output.good();
}

HandoffParse parseHandoff(std::string_view json)
{
    HandoffParse result;
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(json);
    } catch (const std::exception& error) {
        result.error = std::string("handoff is not valid JSON: ") + error.what();
        return result;
    }
    if (!document.is_object()) {
        result.error = "handoff must be a JSON object";
        return result;
    }
    const auto stringField = [&](const char* key) {
        const auto found = document.find(key);
        if (found == document.end() || !found->is_string()) return std::string{};
        return found->get<std::string>();
    };
    result.handoff.url = stringField("url");
    result.handoff.referrer = stringField("referrer");
    result.handoff.cookies = stringField("cookies");
    result.handoff.userAgent = stringField("userAgent");
    result.handoff.downloadId = stringField("downloadId");
    if (result.handoff.url.empty()) {
        result.error = "handoff has no url";
        return result;
    }
    if (!isSupportedScheme(result.handoff.url)) {
        result.error = "scheme is not supported by the core";
        return result;
    }
    result.ok = true;
    return result;
}

std::string handoffResponse(bool accepted, std::string_view detail)
{
    nlohmann::json answer;
    answer["accepted"] = accepted;
    answer["detail"] = std::string(detail);
    return answer.dump();
}

} // namespace litefetch
