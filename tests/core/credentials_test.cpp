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

void forgettingRemovesEverything(const std::filesystem::path& root)
{
    const auto file = root / "forget.dat";
    CredentialStore store(file);
    check(store.save("one.example", "alice", "a"), "the first host is stored");
    check(store.save("two.example", "bob", "b"), "the second host is stored");
    check(store.has("one.example") && store.has("two.example"), "both hosts are remembered");
    // "Forget all" is the file going away; the next load starts empty.
    std::error_code error;
    std::filesystem::remove(file, error);
    check(!store.has("one.example"), "forgetting clears the first host");
    check(!store.has("two.example"), "forgetting clears the second host");
    check(!store.load("one.example").has_value(), "a forgotten host does not load");
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
void unwritableFileIsReported(const std::filesystem::path& root)
{
    // A directory that does not exist cannot be opened for writing. The caller
    // must hear about it instead of promising a memory that was never saved.
    const auto file = root / "missing-dir" / "credentials.dat";
    CredentialStore store(file);
    check(!store.save("example.org", "alice", "s3cret"), "a failed write is reported");
    check(!store.has("example.org"), "a failed write leaves nothing to load");
    check(!std::filesystem::exists(file), "a failed write creates no file");
}
} // namespace

int main()
{
    return run("credentials", [] {
        TempDir root;
        roundTrip(root.path());
        tamperedBlobIsRejected(root.path());
        forgettingRemovesEverything(root.path());
        unwritableFileIsReported(root.path());
        return 0;
    });
}
