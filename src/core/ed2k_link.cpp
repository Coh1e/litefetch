#include "ed2k_link.h"

#include <charconv>
#include <cstdio>
#include <vector>

namespace litefetch {
namespace {
std::vector<std::string_view> splitFields(std::string_view link)
{
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= link.size()) {
        const auto end = link.find('|', start);
        fields.push_back(link.substr(start,
            end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return fields;
}

bool isHexHash(std::string_view text)
{
    if (text.size() != 32) return false;
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}
} // namespace

Ed2kSource decodeEd2kSource(std::uint32_t id)
{
    Ed2kSource source;
    source.id = id;
    const auto first = static_cast<unsigned>((id >> 24) & 0xff);
    const auto second = static_cast<unsigned>((id >> 16) & 0xff);
    const auto third = static_cast<unsigned>((id >> 8) & 0xff);
    const auto portHigh = static_cast<unsigned>(id & 0xff);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.0", first, second, third);
    source.address = buffer;
    source.port = static_cast<std::uint16_t>(portHigh << 8);
    return source;
}

Ed2kLinkResult parseEd2kLink(std::string_view link)
{
    Ed2kLinkResult result;
    if (link.rfind("ed2k://", 0) != 0) {
        result.error = "not an ed2k link";
        return result;
    }
    auto fields = splitFields(link.substr(7));
    // The separator right after "ed2k://" leaves an empty first field.
    if (!fields.empty() && fields.front().empty()) fields.erase(fields.begin());
    // File links: |file|name|size|hash|/
    if (fields.size() < 5 || fields[0] != "file") {
        if (!fields.empty() && fields[0] == "server") {
            result.error = "server links do not name a file";
            return result;
        }
        if (!fields.empty() && fields[0] == "nodes") {
            result.error = "nodes links belong to Kad, not to a file";
            return result;
        }
        result.error = "ed2k file link needs name, size and hash";
        return result;
    }
    result.link.name = std::string(fields[1]);
    if (result.link.name.empty()) {
        result.error = "ed2k link has no file name";
        return result;
    }
    const auto sizeText = fields[2];
    std::int64_t size = 0;
    const auto sizeResult = std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size);
    if (sizeResult.ec != std::errc{} || size <= 0) {
        result.error = "ed2k link has an invalid size";
        return result;
    }
    result.link.size = size;
    if (!isHexHash(fields[3])) {
        result.error = "ed2k link hash must be 32 hexadecimal characters";
        return result;
    }
    result.link.hash = std::string(fields[3]);
    // "sources,ip:port,..." may follow the file fields.
    for (std::size_t index = 4; index < fields.size(); ++index) {
        const auto field = fields[index];
        if (field.rfind("sources,", 0) != 0) continue;
        std::size_t start = 8;
        while (start < field.size()) {
            const auto end = field.find(',', start);
            const auto item = field.substr(start,
                end == std::string_view::npos ? std::string_view::npos : end - start);
            const auto colon = item.rfind(':');
            if (colon != std::string_view::npos) {
                const auto portText = item.substr(colon + 1);
                int port = 0;
                const auto parsed = std::from_chars(portText.data(),
                    portText.data() + portText.size(), port);
                if (parsed.ec == std::errc{} && port > 0 && port <= 65535) {
                    Ed2kSource source;
                    source.address = std::string(item.substr(0, colon));
                    source.port = static_cast<std::uint16_t>(port);
                    result.sources.push_back(std::move(source));
                }
            }
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
    }
    result.ok = true;
    return result;
}

} // namespace litefetch
