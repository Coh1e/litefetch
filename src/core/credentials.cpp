#include "credentials.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>

#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace litefetch {
namespace {
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& bytes)
{
    std::string out;
    std::size_t index = 0;
    while (index + 2 < bytes.size()) {
        const auto chunk = (static_cast<unsigned char>(bytes[index]) << 16)
            | (static_cast<unsigned char>(bytes[index + 1]) << 8)
            | static_cast<unsigned char>(bytes[index + 2]);
        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out.push_back(alphabet[(chunk >> 6) & 0x3f]);
        out.push_back(alphabet[chunk & 0x3f]);
        index += 3;
    }
    const auto remaining = bytes.size() - index;
    if (remaining == 1) {
        const auto chunk = static_cast<unsigned char>(bytes[index]) << 16;
        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const auto chunk = (static_cast<unsigned char>(bytes[index]) << 16)
            | (static_cast<unsigned char>(bytes[index + 1]) << 8);
        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out.push_back(alphabet[(chunk >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64Decode(const std::string& text)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') break;
        const int digit = value(c);
        if (digit < 0) return std::nullopt;
        buffer = (buffer << 6) | digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

std::map<std::string, std::string> readFile(const std::filesystem::path& file)
{
    std::map<std::string, std::string> records;
    std::ifstream input(file);
    std::string line;
    while (std::getline(input, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        records[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return records;
}

bool writeFile(const std::filesystem::path& file, const std::map<std::string, std::string>& records)
{
    std::ofstream output(file, std::ios::trunc);
    if (!output) return false;
    for (const auto& [host, blob] : records) output << host << '\t' << blob << '\n';
    output.flush();
    return static_cast<bool>(output);
}
} // namespace

std::string protectForCurrentUser(const std::string& plain)
{
    if (plain.empty()) return {};
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    input.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB output{};
    // User scope: another Windows account cannot decrypt this, which is exactly
    // the behaviour the ticket asks for.
    if (!CryptProtectData(&input, L"LiteFetch", nullptr, nullptr, nullptr, 0, &output))
        return {};
    std::string result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::optional<std::string> unprotectForCurrentUser(const std::string& protectedBytes)
{
    if (protectedBytes.empty()) return std::nullopt;
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(protectedBytes.data()));
    input.cbData = static_cast<DWORD>(protectedBytes.size());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
        return std::nullopt;
    std::string result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

CredentialStore::CredentialStore(std::filesystem::path file) : file_(std::move(file)) {}

bool CredentialStore::save(const std::string& host, const std::string& username,
    const std::string& password)
{
    if (host.empty()) return false;
    const auto plain = username + '\n' + password;
    const auto protectedBytes = protectForCurrentUser(plain);
    if (protectedBytes.empty()) return false;
    auto records = readFile(file_);
    records[host] = base64Encode(protectedBytes);
    // A failed write must not be reported as remembered: the next start would
    // quietly ask again while the UI had promised otherwise.
    return writeFile(file_, records);
}

std::optional<StoredCredential> CredentialStore::load(const std::string& host) const
{
    const auto records = readFile(file_);
    const auto found = records.find(host);
    if (found == records.end()) return std::nullopt;
    const auto blob = base64Decode(found->second);
    if (!blob) return std::nullopt;
    const auto plain = unprotectForCurrentUser(*blob);
    if (!plain) return std::nullopt; // another account or tampered data
    const auto newline = plain->find('\n');
    if (newline == std::string::npos) return std::nullopt;
    StoredCredential credential;
    credential.host = host;
    credential.username = plain->substr(0, newline);
    credential.password = plain->substr(newline + 1);
    return credential;
}

void CredentialStore::remove(const std::string& host)
{
    auto records = readFile(file_);
    if (records.erase(host) > 0) writeFile(file_, records);
}

bool CredentialStore::has(const std::string& host) const
{
    const auto records = readFile(file_);
    return records.contains(host);
}

} // namespace litefetch
