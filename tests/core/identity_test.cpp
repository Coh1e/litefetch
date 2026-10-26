#include "task_identity.h"
#include "test_support.h"

#include <string>

using namespace litefetch;
using namespace litefetch::test;

namespace {
int body()
{
    // Published SHA-256 test vectors (FIPS 180-2 examples).
    checkEqual(taskIdentity("abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 abc vector");
    checkEqual(taskIdentity(""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA-256 empty vector");

    const std::string source = "http://127.0.0.1:8080/files/payload.bin";
    const auto first = taskIdentity(source);
    const auto second = taskIdentity(source);
    checkEqual(first, second, "identity is stable for identical source bytes");
    checkEqual(static_cast<long long>(first.size()), 64, "identity length");
    check(first != taskIdentity(source + "?v=2"), "different sources have different identities");
    for (const char c : first)
        check((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "identity is lowercase hex");
    return 0;
}
} // namespace

int main()
{
    return run("identity", &body);
}
