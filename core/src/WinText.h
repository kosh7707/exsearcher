#pragma once

// Internal Win32 text helpers shared by CrawlIndexer and SearchEngine.
// Not part of the public include/ surface.

#include <string>

namespace exsearcher::wintext {

// Convert UTF-16 to UTF-8.
std::string toUtf8(const wchar_t* s, int len);
inline std::string toUtf8(const std::wstring& s) {
    return toUtf8(s.c_str(), static_cast<int>(s.size()));
}

// Convert UTF-8 to UTF-16.
std::wstring toUtf16(const char* s, int len);
inline std::wstring toUtf16(const std::string& s) {
    return toUtf16(s.c_str(), static_cast<int>(s.size()));
}

// Case-fold UTF-16 using LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE).
std::wstring caseFold(const std::wstring& s);

} // namespace exsearcher::wintext
