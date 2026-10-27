#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace litefetch {

// The core boundary carries text as UTF-8. Windows APIs and
// std::filesystem::path's narrow constructor use the ANSI code page, so every
// conversion below is explicit and strict: invalid UTF-8 throws instead of
// silently retargeting a different file name. Chinese destinations, sources and
// database paths must survive the round trip unchanged.
std::wstring utf16FromUtf8(std::string_view text);
std::string utf8FromUtf16(std::wstring_view text);
std::filesystem::path pathFromUtf8(std::string_view text);
std::string pathToUtf8(const std::filesystem::path& path);

} // namespace litefetch
