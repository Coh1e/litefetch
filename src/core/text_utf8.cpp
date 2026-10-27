#include "text_utf8.h"

#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <climits>
#endif

namespace litefetch {

#ifdef _WIN32
std::wstring utf16FromUtf8(std::string_view text)
{
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("text is too long to convert");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) throw std::runtime_error("text is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), result.data(), length) != length)
        throw std::runtime_error("text is not valid UTF-8");
    return result;
}

std::string utf8FromUtf16(std::wstring_view text)
{
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("text is too long to convert");
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::runtime_error("text is not valid UTF-16");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), result.data(), length, nullptr, nullptr) != length)
        throw std::runtime_error("text is not valid UTF-16");
    return result;
}

std::filesystem::path pathFromUtf8(std::string_view text)
{
    return std::filesystem::path(utf16FromUtf8(text));
}

std::string pathToUtf8(const std::filesystem::path& path)
{
    return utf8FromUtf16(path.native());
}
#else
std::wstring utf16FromUtf8(std::string_view text)
{
    return std::wstring(text.begin(), text.end());
}

std::string utf8FromUtf16(std::wstring_view text)
{
    return std::string(text.begin(), text.end());
}

std::filesystem::path pathFromUtf8(std::string_view text)
{
    return std::filesystem::path(std::string(text));
}

std::string pathToUtf8(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
#endif

} // namespace litefetch
