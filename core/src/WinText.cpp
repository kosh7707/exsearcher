#include "WinText.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace exsearcher::wintext {

std::string toUtf8(const wchar_t* s, int len) {
    if (len == 0)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring toUtf16(const char* s, int len) {
    if (len == 0)
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, out.data(), n);
    return out;
}

std::wstring caseFold(const std::wstring& s) {
    if (s.empty())
        return std::wstring();
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                          s.c_str(), static_cast<int>(s.size()),
                          nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0)
        return s;
    std::wstring out(static_cast<size_t>(n), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                  s.c_str(), static_cast<int>(s.size()),
                  out.data(), n, nullptr, nullptr, 0);
    return out;
}

} // namespace exsearcher::wintext
