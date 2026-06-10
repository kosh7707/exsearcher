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

// Hot-path helper for the crawler: from a UTF-16 name produce BOTH the UTF-8
// original (`outName`) and its case-folded lowercase UTF-8 mirror (`outLower`).
//
// The mirror is guaranteed to have the SAME byte length as outName (the Index
// search path relies on shared offsets). When a non-ASCII fold would change the
// byte length, outLower falls back to a copy of outName (matching the prior
// `foldedU8.size() == name.size() ? foldedU8 : name` semantics).
//
// Fast path: if every UTF-16 unit is < 0x80 (pure ASCII, the vast majority of
// filenames) both strings are produced in a single inline loop with ZERO Win32
// calls. Otherwise it falls back to LCMapStringEx + WideCharToMultiByte.
//
// `foldScratch` is a caller-owned std::wstring reused across calls to avoid a
// per-name allocation on the non-ASCII path; it is cleared internally.
void toUtf8AndLower(const wchar_t* name, int len, std::string& outName,
                    std::string& outLower, std::wstring& foldScratch);

} // namespace exsearcher::wintext
