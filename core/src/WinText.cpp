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

void toUtf8AndLower(const wchar_t* name, int len, std::string& outName,
                    std::string& outLower, std::wstring& foldScratch) {
    outName.clear();
    outLower.clear();
    if (len <= 0)
        return;

    // ASCII fast path: scan once. If every unit is < 0x80, both the UTF-8
    // original and the lowercase mirror are pure-ASCII byte-for-byte copies, so
    // we build them inline with no Win32 round-trips.
    bool ascii = true;
    for (int i = 0; i < len; ++i) {
        if (name[i] >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii) {
        outName.resize(static_cast<size_t>(len));
        outLower.resize(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            const char b = static_cast<char>(name[i]);
            outName[static_cast<size_t>(i)] = b;
            outLower[static_cast<size_t>(i)] =
                (b >= 'A' && b <= 'Z') ? static_cast<char>(b | 0x20) : b;
        }
        return;
    }

    // Non-ASCII fallback: identical to the prior crawler logic. Korean and other
    // caseless scripts fold to themselves; Latin/Cyrillic/etc. fold correctly.
    outName = toUtf8(name, len);

    int fn = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, name, len,
                           nullptr, 0, nullptr, nullptr, 0);
    if (fn <= 0) {
        outLower = outName;
        return;
    }
    foldScratch.clear();
    foldScratch.resize(static_cast<size_t>(fn));
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, name, len,
                  foldScratch.data(), fn, nullptr, nullptr, 0);

    std::string foldedU8 = toUtf8(foldScratch);
    // The Index requires the mirror to match outName's byte length; if folding
    // changed the encoded length, fall back to the original bytes.
    outLower = (foldedU8.size() == outName.size()) ? std::move(foldedU8) : outName;
}

} // namespace exsearcher::wintext
