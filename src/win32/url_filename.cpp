#include "url_filename.h"

#include <cctype>
#include <string>
#include <vector>

namespace litefetch::ui {
namespace {
bool startsWithNoCase(std::string_view text, std::string_view prefix)
{
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto left = static_cast<unsigned char>(text[i]);
        const auto right = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

std::vector<std::string> splitAddresses(std::string_view text)
{
    std::vector<std::string> result;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
        const std::size_t start = index;
        while (index < text.size() && !std::isspace(static_cast<unsigned char>(text[index]))) ++index;
        if (index > start) result.emplace_back(text.substr(start, index - start));
    }
    return result;
}

int hexValue(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

std::string percentDecode(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hexValue(text[i + 1]);
            const int low = hexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(text[i]);
    }
    return result;
}

std::string sanitizeFileName(std::string name)
{
    std::string result;
    result.reserve(name.size());
    for (const char character : name) {
        const auto value = static_cast<unsigned char>(character);
        const bool reserved = value < 0x20 || character == '<' || character == '>' || character == ':'
            || character == '"' || character == '/' || character == '\\' || character == '|'
            || character == '?' || character == '*';
        result.push_back(reserved ? '_' : character);
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) result.pop_back();
    // Keep the name usable on Windows without splitting a UTF-8 sequence.
    constexpr std::size_t maxBytes = 180;
    if (result.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xC0) == 0x80) --cut;
        result.resize(cut);
    }
    if (result.empty() || result == "." || result == "..") result = "download.bin";
    return result;
}
} // namespace

std::string suggestedFileName(std::string_view url)
{
    const auto fragment = url.find('#');
    if (fragment != std::string_view::npos) url = url.substr(0, fragment);
    const auto query = url.find('?');
    if (query != std::string_view::npos) url = url.substr(0, query);
    const auto slash = url.find_last_of('/');
    const std::string_view candidate = slash == std::string_view::npos ? url : url.substr(slash + 1);
    if (candidate.empty()) return "download.bin";
    return sanitizeFileName(percentDecode(candidate));
}

SourceInput analyzeSourceInput(std::string_view text)
{
    SourceInput result;
    const auto addresses = splitAddresses(text);
    if (addresses.empty()) {
        result.kind = SourceKind::Empty;
        result.hint = "请输入下载地址。";
        return result;
    }
    result.source = addresses.front();
    result.suggestedFileName = suggestedFileName(result.source);
    if (addresses.size() > 1) {
        result.kind = SourceKind::Multiple;
        result.hint = "已识别: 多个地址。批量添加将在后续版本提供。";
        return result;
    }
    if (startsWithNoCase(result.source, "https://")) {
        result.kind = SourceKind::Https;
        result.hint = "已识别: HTTPS。";
    } else if (startsWithNoCase(result.source, "http://")) {
        result.kind = SourceKind::Http;
        result.hint = "已识别: HTTP。";
    } else if (startsWithNoCase(result.source, "ftp://")
        || startsWithNoCase(result.source, "ftps://")) {
        result.kind = SourceKind::Ftp;
        result.hint = "已识别: FTP。需要登录时会在开始时询问账号。";
    } else if (startsWithNoCase(result.source, "magnet:")) {
        result.kind = SourceKind::Magnet;
        result.hint = "已识别: 磁力链接。取到元数据后选择文件。";
    } else if (startsWithNoCase(result.source, "ed2k://")) {
        result.kind = SourceKind::Ed2k;
        result.hint = "已识别: ed2k 链接。需要已配置 ed2k 服务器或链接自带来源。";
        // The name travels with the link: ed2k://|file|<name>|<size>|<hash>|/
        const auto firstBar = result.source.find('|');
        const auto secondBar = firstBar == std::string::npos
            ? std::string::npos : result.source.find('|', firstBar + 1);
        const auto thirdBar = secondBar == std::string::npos
            ? std::string::npos : result.source.find('|', secondBar + 1);
        if (thirdBar != std::string::npos && thirdBar > secondBar + 1) {
            const auto name = result.source.substr(secondBar + 1, thirdBar - secondBar - 1);
            if (!name.empty()) result.suggestedFileName = sanitizeFileName(percentDecode(name));
        }
    } else {
        result.kind = SourceKind::Unsupported;
        result.hint = "不支持的地址类型。支持 HTTP(S)、FTP、磁力链接与 ed2k 文件链接。";
    }
    return result;
}

} // namespace litefetch::ui
