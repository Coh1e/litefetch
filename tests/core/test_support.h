#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace litefetch::test {

inline void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

inline void checkEqual(const std::string& actual, const std::string& expected, const char* message)
{
    if (actual != expected)
        throw std::runtime_error(std::string(message) + ": expected '" + expected + "', got '" + actual + "'");
}

inline void checkEqual(long long actual, long long expected, const char* message)
{
    if (actual != expected)
        throw std::runtime_error(std::string(message) + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual));
}

inline std::string environmentValue(const char* name)
{
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

inline std::filesystem::path testTempRoot()
{
    const auto configured = environmentValue("LITEFETCH_TEST_TEMP");
    if (!configured.empty()) {
        const std::filesystem::path root(configured);
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error) throw std::runtime_error("cannot create LITEFETCH_TEST_TEMP: " + error.message());
        return root;
    }
    return std::filesystem::temp_directory_path();
}

class TempDir {
public:
    TempDir()
    {
        static std::atomic<unsigned long long> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = testTempRoot()
            / ("litefetch-core-" + std::to_string(GetCurrentProcessId()) + "-"
                + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path file(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

inline std::vector<unsigned char> payload(std::size_t size = 256 * 1024)
{
    std::vector<unsigned char> bytes(size);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<unsigned char>((i * 37 + 11) & 0xff);
    return bytes;
}

inline std::vector<unsigned char> readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read file: " + path.string());
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

inline void writeFile(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write file: " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot finish file: " + path.string());
}

inline int run(const char* name, int (*body)())
{
    try {
        const int result = body();
        std::cout << name << ": PASS\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << name << ": FAIL: " << error.what() << '\n';
        return 1;
    }
}

} // namespace litefetch::test
