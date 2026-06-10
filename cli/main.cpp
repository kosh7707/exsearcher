#include "exsearcher/Index.h"
#include "exsearcher/CrawlIndexer.h"
#include "exsearcher/SearchEngine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <string>
#include <vector>

using namespace exsearcher;

namespace {

std::wstring utf8ToUtf16(const std::string& s) {
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

std::string utf16ToUtf8(const std::wstring& s) {
    if (s.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

bool g_stdoutIsConsole = false;
HANDLE g_hOut = nullptr;

// Print a UTF-8 string. To console: convert to UTF-16 + WriteConsoleW.
// When redirected: write raw UTF-8 bytes.
void printU8(const std::string& u8) {
    if (g_stdoutIsConsole) {
        std::wstring w = utf8ToUtf16(u8);
        DWORD written = 0;
        WriteConsoleW(g_hOut, w.c_str(), static_cast<DWORD>(w.size()), &written,
                      nullptr);
    } else {
        DWORD written = 0;
        WriteFile(g_hOut, u8.data(), static_cast<DWORD>(u8.size()), &written,
                  nullptr);
    }
}

void printLine(const std::string& u8) {
    printU8(u8);
    printU8("\n");
}

// Read one line of input as UTF-16. From console: ReadConsoleW.
// When redirected: read raw bytes (assumed UTF-8) and convert.
bool readLine(std::wstring& out) {
    out.clear();
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool isConsole = GetConsoleMode(hIn, &mode) != 0;

    if (isConsole) {
        std::wstring buf;
        buf.resize(4096);
        DWORD read = 0;
        if (!ReadConsoleW(hIn, buf.data(), static_cast<DWORD>(buf.size()), &read,
                          nullptr) ||
            read == 0) {
            return false;  // Ctrl+Z / EOF
        }
        buf.resize(read);
        // Strip trailing CR/LF.
        while (!buf.empty() && (buf.back() == L'\r' || buf.back() == L'\n'))
            buf.pop_back();
        out = std::move(buf);
        return true;
    }

    // Redirected stdin: read bytes until newline.
    std::string bytes;
    char ch;
    DWORD read = 0;
    bool eof = false;
    for (;;) {
        if (!ReadFile(hIn, &ch, 1, &read, nullptr) || read == 0) {
            eof = true;
            break;
        }
        if (ch == '\n')
            break;
        if (ch != '\r')
            bytes.push_back(ch);
    }
    // Strip a UTF-8 BOM at stream start (text files and PowerShell pipes
    // often prepend one; it must not become part of the first query).
    static bool firstLine = true;
    if (firstLine) {
        firstLine = false;
        if (bytes.rfind("\xEF\xBB\xBF", 0) == 0)
            bytes.erase(0, 3);
    }
    if (eof && bytes.empty())
        return false;
    out = utf8ToUtf16(bytes);
    return true;
}

std::string formatBytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return std::string(buf);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD outMode = 0;
    g_stdoutIsConsole = GetConsoleMode(g_hOut, &outMode) != 0;

    if (argc < 2) {
        printLine("Usage: exsearcher-cli <root> [<root>...]");
        return 1;
    }

    std::vector<std::wstring> roots;
    for (int i = 1; i < argc; ++i)
        roots.emplace_back(argv[i]);

    Index index;
    CrawlIndexer crawler(index);
    crawler.setProgressCallback([](uint64_t n) {
        char buf[128];
        snprintf(buf, sizeof(buf), "  ...indexed %llu entries",
                 static_cast<unsigned long long>(n));
        printLine(buf);
    });

    printLine("Crawling...");
    CrawlStats stats = crawler.crawl(roots);

    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Indexed %llu files, %llu dirs (%llu skipped) in %llu ms",
                 static_cast<unsigned long long>(stats.totalFiles),
                 static_cast<unsigned long long>(stats.totalDirs),
                 static_cast<unsigned long long>(stats.skippedDirs),
                 static_cast<unsigned long long>(stats.elapsedMs));
        printLine(buf);
        printLine("Index memory: " + formatBytes(index.memoryBytes()) +
                  " (" + std::to_string(index.size()) + " entries)");
    }

    SearchEngine engine(index);

    for (;;) {
        printU8("> ");
        std::wstring line;
        if (!readLine(line))
            break;  // EOF / Ctrl+Z
        if (line.empty())
            break;  // empty line exits

        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        SearchResult res = engine.search(line, 50);
        const auto t1 = clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();

        for (uint32_t idx : res.indices)
            printLine(index.fullPath(idx));

        char buf[128];
        snprintf(buf, sizeof(buf), "%llu matches in %.2f ms",
                 static_cast<unsigned long long>(res.totalMatches), ms);
        printLine(buf);
    }

    return 0;
}
