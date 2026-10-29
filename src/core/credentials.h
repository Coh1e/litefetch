#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace litefetch {

struct StoredCredential {
    std::string host;
    std::string username;
    std::string password;
};

// Credentials are memory-only by default; when the user asks to remember one it
// is protected with DPAPI for the current Windows account and written to a file
// beside the database. Another account cannot decrypt it, so it simply asks
// again instead of pretending the stored secret works.
class CredentialStore {
public:
    explicit CredentialStore(std::filesystem::path file);

    bool save(const std::string& host, const std::string& username, const std::string& password);
    std::optional<StoredCredential> load(const std::string& host) const;
    void remove(const std::string& host);
    bool has(const std::string& host) const;

private:
    std::filesystem::path file_;
};

// Exposed for tests: DPAPI round trip for one account.
std::string protectForCurrentUser(const std::string& plain);
std::optional<std::string> unprotectForCurrentUser(const std::string& protectedBytes);

} // namespace litefetch
