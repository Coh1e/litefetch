// DPAPI-backed credential memory: the file never contains the password, and a
// blob from another account (or a tampered one) simply does not decrypt.
#include "credentials.h"
#include "test_support.h"

#include <fstream>
#include <string>

using namespace litefetch;
using namespace litefetch::test;

namespace {
void roundTrip(const std::filesystem::path& root)
{
    const auto file = root / "credentials.dat";
    CredentialStore store(file);
    check(!store.has("example.org"), "nothing is stored at first");

    check(store.save("example.org", "alice", "s3cret"), "the credential is stored");
    check(store.has("example.org"), "the host is remembered");
    const auto loaded = store.load("example.org");
    check(loaded.has_value(), "the credential loads");
    checkEqual(loaded->username, std::string("alice"), "the user name survives");
    checkEqual(loaded->password, std::string("s3cret"), "the password survives");

    // The file must not contain the password in clear.
    std::ifstream input(file, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    check(raw.find("s3cret") == std::string::npos, "the password is not written in clear");

    store.remove("example.org");
    check(!store.has("example.org"), "the host is forgotten");
    check(!store.load("example.org").has_value(), "a removed credential does not load");
}

void tamperedBlobIsRejected(const std::filesystem::path& root)
{
    const auto file = root / "tampered.dat";
    {
        CredentialStore store(file);
        check(store.save("example.org", "alice", "s3cret"), "the credential is stored");
    }
    // Replace the blob with valid base64 of garbage: decryption must fail.
    {
        std::ofstream output(file, std::ios::trunc);
        output << "example.org\t" << "AAAAAAAAdGhpcyBpcyBub3QgZHBBUEk=" << '\n';
    }
    CredentialStore store(file);
    check(!store.load("example.org").has_value(), "a tampered blob is refused");
    // The DPAPI round trip itself still works for this account.
    const auto protectedBytes = protectForCurrentUser("hello");
    check(!protectedBytes.empty(), "DPAPI protects a value");
    const auto plain = unprotectForCurrentUser(protectedBytes);
    check(plain.has_value() && *plain == "hello", "DPAPI restores the value");
    check(!unprotectForCurrentUser("garbage").has_value(), "unprotect refuses garbage");
}
} // namespace

int main()
{
    return run("credentials", [] {
        TempDir root;
        roundTrip(root.path());
        tamperedBlobIsRejected(root.path());
        return 0;
    });
}
