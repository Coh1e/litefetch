#include "task_identity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <stdexcept>

namespace litefetch {
namespace {
class Algorithm {
public:
    Algorithm()
    {
        if (BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("cannot open SHA-256 provider");
    }
    ~Algorithm() { if (handle_) BCryptCloseAlgorithmProvider(handle_, 0); }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class Hash {
public:
    explicit Hash(BCRYPT_ALG_HANDLE algorithm)
    {
        if (BCryptCreateHash(algorithm, &handle_, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("cannot create SHA-256 hash");
    }
    ~Hash() { if (handle_) BCryptDestroyHash(handle_); }
    Hash(const Hash&) = delete;
    Hash& operator=(const Hash&) = delete;
    void update(const void* bytes, std::size_t size)
    {
        if (size > 0xffffffffull) throw std::runtime_error("identity input is too large");
        if (BCryptHashData(handle_, static_cast<PUCHAR>(const_cast<void*>(bytes)),
                static_cast<ULONG>(size), 0) < 0)
            throw std::runtime_error("cannot hash task source");
    }
    std::array<unsigned char, 32> finish()
    {
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(handle_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("cannot finish SHA-256 hash");
        return digest;
    }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};
} // namespace

std::string taskIdentity(std::string_view canonicalSource)
{
    static const Algorithm algorithm;
    Hash hash(algorithm.get());
    hash.update(canonicalSource.data(), canonicalSource.size());
    const auto digest = hash.finish();
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const unsigned char byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

} // namespace litefetch
