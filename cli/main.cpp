#include "exsearcher/Index.h"
#include "exsearcher/CrawlIndexer.h"
#include "exsearcher/SearchEngine.h"
#include "exsearcher/Snapshot.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
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

// ---------------------------------------------------------------------------
// --watch <dir> test mode.
//
// Crawls <dir> into an index, then runs ReadDirectoryChangesW on it and applies
// ADDED/REMOVED/MODIFIED/RENAMED events to the index live, printing each batch.
// This exercises the exact RDCW parsing + path-map + tombstone/updateMeta logic
// the app's Watcher/IndexController use, but headless so it can be driven from a
// shell (create/delete/rename files, observe the printed events). Runs until a
// line is read on stdin (or watchSeconds elapse, whichever first).
//
// Mirrors IndexController::applyWatcherBatch translation rules but inline, since
// the controller is Qt-side and the CLI is core-only.
// ---------------------------------------------------------------------------

// Lowercased, separator-trimmed dir-path key (matches IndexController::dirKey).
std::wstring dirKeyW(std::wstring p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();
    // Lowercase via LCMapStringEx invariant fold.
    if (!p.empty()) {
        int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, p.c_str(),
                              static_cast<int>(p.size()), nullptr, 0, nullptr,
                              nullptr, 0);
        if (n > 0) {
            std::wstring out(static_cast<size_t>(n), L'\0');
            LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, p.c_str(),
                          static_cast<int>(p.size()), out.data(), n, nullptr,
                          nullptr, 0);
            return out;
        }
    }
    return p;
}

int runWatch(const std::wstring& dir, int watchSeconds) {
    Index index;
    {
        CrawlIndexer crawler(index);
        printLine("Crawling " + utf16ToUtf8(dir) + " ...");
        CrawlStats stats = crawler.crawl({dir});
        char buf[256];
        snprintf(buf, sizeof(buf), "Indexed %llu files, %llu dirs",
                 static_cast<unsigned long long>(stats.totalFiles),
                 static_cast<unsigned long long>(stats.totalDirs));
        printLine(buf);
    }

    // Build the lowercased-dir-path -> idx map (directories only).
    std::unordered_map<std::wstring, uint32_t> dirMap;
    auto rebuildMap = [&] {
        dirMap.clear();
        const auto& es = index.entries();
        for (size_t i = 0; i < es.size(); ++i) {
            if ((es[i].attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            if (es[i].attr & kAttrTombstone)
                continue;
            std::wstring full = utf8ToUtf16(index.fullPath(static_cast<uint32_t>(i)));
            dirMap[dirKeyW(full)] = static_cast<uint32_t>(i);
        }
    };
    rebuildMap();

    // The watch root path (no trailing sep) for joining relative event paths.
    std::wstring rootPath = dir;
    while (!rootPath.empty() &&
           (rootPath.back() == L'\\' || rootPath.back() == L'/'))
        rootPath.pop_back();

    HANDLE h = CreateFileW(
        (rootPath + L"\\").c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        printLine("Cannot open directory for watching.");
        return 1;
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME |
                         FILE_NOTIFY_CHANGE_DIR_NAME |
                         FILE_NOTIFY_CHANGE_LAST_WRITE |
                         FILE_NOTIFY_CHANGE_SIZE;
    std::vector<char> buffer(64 * 1024);

    printLine("Watching... (run for ~" + std::to_string(watchSeconds) +
              "s; create/delete/rename files now)");

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(watchSeconds);

    while (clock::now() < deadline) {
        ResetEvent(ov.hEvent);
        DWORD br = 0;
        if (!ReadDirectoryChangesW(h, buffer.data(),
                                   static_cast<DWORD>(buffer.size()), TRUE,
                                   filter, &br, &ov, nullptr)) {
            printLine("ReadDirectoryChangesW failed -> would schedule rescan");
            break;
        }
        const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - clock::now())
                                .count();
        DWORD waitMs = remain > 0 ? static_cast<DWORD>(remain) : 0;
        DWORD wr = WaitForSingleObject(ov.hEvent, waitMs);
        if (wr == WAIT_TIMEOUT) {
            CancelIo(h);
            DWORD dummy = 0;
            GetOverlappedResult(h, &ov, &dummy, TRUE);
            break;
        }
        DWORD transferred = 0;
        if (!GetOverlappedResult(h, &ov, &transferred, FALSE) ||
            transferred == 0) {
            printLine("** buffer overflow / zero-length -> schedule full rescan **");
            continue;
        }

        bool structural = false;
        size_t offset = 0;
        for (;;) {
            auto* fni =
                reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
            std::wstring rel(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
            std::wstring full = rootPath + L"\\" + rel;

            // Split rel into parent + leaf.
            size_t slash = rel.find_last_of(L'\\');
            std::wstring parentRel =
                (slash == std::wstring::npos) ? L"" : rel.substr(0, slash);
            std::wstring leaf =
                (slash == std::wstring::npos) ? rel : rel.substr(slash + 1);
            std::wstring parentFull =
                parentRel.empty() ? rootPath : (rootPath + L"\\" + parentRel);

            const char* actName = "?";
            switch (fni->Action) {
                case FILE_ACTION_ADDED: actName = "ADDED"; break;
                case FILE_ACTION_REMOVED: actName = "REMOVED"; break;
                case FILE_ACTION_MODIFIED: actName = "MODIFIED"; break;
                case FILE_ACTION_RENAMED_OLD_NAME: actName = "RENAMED_OLD"; break;
                case FILE_ACTION_RENAMED_NEW_NAME: actName = "RENAMED_NEW"; break;
                default: actName = "OTHER"; break;
            }
            std::string applied = "ignored";

            if (fni->Action == FILE_ACTION_ADDED ||
                fni->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                auto it = dirMap.find(dirKeyW(parentFull));
                if (it != dirMap.end()) {
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard,
                                             &fad)) {
                        PendingEntry pe;
                        pe.name = utf16ToUtf8(leaf);
                        std::string low = utf16ToUtf8(dirKeyW(leaf));
                        pe.nameLower =
                            (low.size() == pe.name.size()) ? low : pe.name;
                        pe.parentIdx = it->second;
                        pe.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) |
                                  fad.nFileSizeLow;
                        pe.mtime = (static_cast<uint64_t>(
                                        fad.ftLastWriteTime.dwHighDateTime)
                                    << 32) |
                                   fad.ftLastWriteTime.dwLowDateTime;
                        pe.attr = fad.dwFileAttributes;
                        uint32_t newIdx = index.appendOne(pe);
                        structural = true;
                        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                            dirMap[dirKeyW(full)] = newIdx;
                        applied = "appended idx " + std::to_string(newIdx);
                    } else {
                        applied = "stat failed (gone)";
                    }
                } else {
                    applied = "parent unknown -> ignore";
                }
            } else if (fni->Action == FILE_ACTION_REMOVED ||
                       fni->Action == FILE_ACTION_RENAMED_OLD_NAME) {
                auto dit = dirMap.find(dirKeyW(full));
                if (dit != dirMap.end()) {
                    uint32_t dirIdx = dit->second;
                    const auto& es = index.entries();
                    std::vector<uint8_t> drop(es.size(), 0);
                    if (dirIdx < es.size())
                        drop[dirIdx] = 1;
                    for (size_t i = dirIdx; i < es.size(); ++i) {
                        uint32_t p = es[i].parentIdx;
                        if (p != Index::kNoParent && p < drop.size() && drop[p])
                            drop[i] = 1;
                    }
                    size_t cnt = 0;
                    for (size_t i = 0; i < drop.size(); ++i)
                        if (drop[i]) { index.markDeleted(static_cast<uint32_t>(i)); ++cnt; }
                    dirMap.erase(dirKeyW(full));
                    structural = true;
                    applied = "tombstoned dir + " + std::to_string(cnt - 1) +
                              " descendants";
                } else {
                    auto pit = dirMap.find(dirKeyW(parentFull));
                    if (pit != dirMap.end()) {
                        std::wstring leafLow = dirKeyW(leaf);
                        uint32_t parentIdx = pit->second;
                        const auto& es = index.entries();
                        bool found = false;
                        for (size_t i = parentIdx + 1; i < es.size(); ++i) {
                            if (es[i].parentIdx != parentIdx)
                                continue;
                            if (index.isDeleted(static_cast<uint32_t>(i)))
                                continue;
                            std::wstring nm =
                                utf8ToUtf16(index.name(static_cast<uint32_t>(i)));
                            if (dirKeyW(nm) == leafLow) {
                                index.markDeleted(static_cast<uint32_t>(i));
                                structural = true;
                                found = true;
                                applied = "tombstoned file idx " + std::to_string(i);
                                break;
                            }
                        }
                        if (!found)
                            applied = "not found under parent";
                    } else {
                        applied = "parent unknown -> ignore";
                    }
                }
            } else if (fni->Action == FILE_ACTION_MODIFIED) {
                auto pit = dirMap.find(dirKeyW(parentFull));
                if (pit != dirMap.end()) {
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard,
                                             &fad)) {
                        std::wstring leafLow = dirKeyW(leaf);
                        uint32_t parentIdx = pit->second;
                        const auto& es = index.entries();
                        for (size_t i = parentIdx + 1; i < es.size(); ++i) {
                            if (es[i].parentIdx != parentIdx)
                                continue;
                            std::wstring nm =
                                utf8ToUtf16(index.name(static_cast<uint32_t>(i)));
                            if (dirKeyW(nm) == leafLow) {
                                uint64_t sz = (static_cast<uint64_t>(
                                                   fad.nFileSizeHigh)
                                               << 32) |
                                              fad.nFileSizeLow;
                                uint64_t mt = (static_cast<uint64_t>(
                                                   fad.ftLastWriteTime.dwHighDateTime)
                                               << 32) |
                                              fad.ftLastWriteTime.dwLowDateTime;
                                index.updateMeta(static_cast<uint32_t>(i), sz, mt);
                                applied = "updateMeta idx " + std::to_string(i) +
                                          " size " + std::to_string(sz);
                                break;
                            }
                        }
                    }
                }
            }

            printLine(std::string("  [") + actName + "] " + utf16ToUtf8(rel) +
                      "  -> " + applied);

            if (fni->NextEntryOffset == 0)
                break;
            offset += fni->NextEntryOffset;
            if (offset >= buffer.size())
                break;
        }

        if (structural)
            printLine("  (batch applied; entries now " +
                      std::to_string(index.size()) + ", tombstones " +
                      std::to_string(index.tombstoneCount()) + ")");
    }

    CloseHandle(ov.hEvent);
    CloseHandle(h);

    printLine("Watch ended. Final entries " + std::to_string(index.size()) +
              ", tombstones " + std::to_string(index.tombstoneCount()) + ".");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD outMode = 0;
    g_stdoutIsConsole = GetConsoleMode(g_hOut, &outMode) != 0;

    if (argc < 2) {
        printLine("Usage: exsearcher-cli [--save <file>] <root> [<root>...]");
        printLine("       exsearcher-cli --load <file>");
        printLine("       exsearcher-cli --watch <dir> [--seconds N]");
        return 1;
    }

    // Parse flags: --save <file> (crawl then save), --load <file> (load instead
    // of crawl), --watch <dir> (crawl + RDCW live-update test). Remaining args
    // are root paths.
    std::wstring saveFile;
    std::wstring loadFile;
    std::wstring watchDir;
    int watchSeconds = 30;
    std::vector<std::wstring> roots;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--save" && i + 1 < argc) {
            saveFile = argv[++i];
        } else if (a == L"--load" && i + 1 < argc) {
            loadFile = argv[++i];
        } else if (a == L"--watch" && i + 1 < argc) {
            watchDir = argv[++i];
        } else if (a == L"--seconds" && i + 1 < argc) {
            watchSeconds = _wtoi(argv[++i]);
        } else {
            roots.emplace_back(a);
        }
    }

    if (!watchDir.empty())
        return runWatch(watchDir, watchSeconds > 0 ? watchSeconds : 30);

    Index index;

    // Load first (if requested), then crawl any positional roots into the SAME
    // index (append-after-load), then save (if requested). This mirrors the app
    // flow where new drives are appended to a snapshot-loaded index.
    if (!loadFile.empty()) {
        std::vector<RootMeta> loadedRoots;
        if (!loadSnapshot(index, loadedRoots, loadFile)) {
            printLine("Load failed (corrupt or missing snapshot).");
            return 1;
        }
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Loaded snapshot: %llu entries, %u roots",
                 static_cast<unsigned long long>(index.size()),
                 static_cast<unsigned>(loadedRoots.size()));
        printLine(buf);
        for (const RootMeta& rm : loadedRoots) {
            printLine("  root[" + std::to_string(rm.rootEntryIdx) + "] = " +
                      rm.rootPathU8);
        }
        printLine("Index memory: " + formatBytes(index.memoryBytes()) +
                  " (" + std::to_string(index.size()) + " entries)");
    }

    if (loadFile.empty() && roots.empty()) {
        printLine("No roots given.");
        return 1;
    }

    if (!roots.empty()) {
        CrawlIndexer crawler(index);
        crawler.setProgressCallback([](const CrawlProgress& p) {
            std::string cur = utf16ToUtf8(p.currentDir);
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "  ...entries %llu  dirs %llu/~%llu  cur: %s",
                     static_cast<unsigned long long>(p.entries),
                     static_cast<unsigned long long>(p.dirsDone),
                     static_cast<unsigned long long>(p.dirsDone + p.dirsPending),
                     cur.c_str());
            printLine(buf);
        });

        printLine("Crawling...");
        CrawlStats stats = crawler.crawl(roots);

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

    if (!saveFile.empty()) {
        // Build RootMeta from the no-parent entries (every crawl/loaded root).
        std::vector<RootMeta> rootMetas;
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const uint64_t nowFt =
            (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const auto& entries = index.entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].parentIdx == Index::kNoParent) {
                RootMeta rm;
                rm.rootEntryIdx = static_cast<uint32_t>(i);
                rm.crawledAtFiletime = nowFt;
                rm.rootPathU8 = index.name(static_cast<uint32_t>(i));
                rootMetas.push_back(std::move(rm));
            }
        }
        if (saveSnapshot(index, rootMetas, saveFile))
            printLine("Snapshot saved.");
        else
            printLine("Snapshot save FAILED.");
    }

    // engine is rebuilt after mutations (!compact / !merge); a unique_ptr lets
    // us replace it since SearchEngine holds a const Index& and isn't assignable.
    auto engine = std::make_unique<SearchEngine>(index);

    for (;;) {
        printU8("> ");
        std::wstring line;
        if (!readLine(line))
            break;  // EOF / Ctrl+Z
        if (line.empty())
            break;  // empty line exits

        // Meta-commands for testing core operations.
        if (line == L"!roots") {
            const auto& entries = index.entries();
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].parentIdx == Index::kNoParent)
                    printLine("root[" + std::to_string(i) + "] = " +
                              index.fullPath(static_cast<uint32_t>(i)));
            continue;
        }
        if (line.rfind(L"!removeroot ", 0) == 0) {
            const uint32_t r =
                static_cast<uint32_t>(_wtoi(line.c_str() + 12));
            index.removeRoot(r);
            printLine("removed root " + std::to_string(r) + "; now " +
                      std::to_string(index.size()) + " entries");
            continue;
        }
        // Tombstone a single entry by index (sets the kAttrTombstone bit).
        if (line.rfind(L"!tombstone ", 0) == 0) {
            const uint32_t r =
                static_cast<uint32_t>(_wtoi(line.c_str() + 11));
            index.markDeleted(r);
            printLine("tombstoned entry " + std::to_string(r) +
                      "; tombstones now " +
                      std::to_string(index.tombstoneCount()));
            continue;
        }
        // Physically drop tombstoned entries + cascade (same rule as save).
        if (line == L"!compact") {
            const size_t removed = index.compactTombstones();
            printLine("compacted: removed " + std::to_string(removed) +
                      " entries; now " + std::to_string(index.size()));
            // Rebuild the engine so subsequent searches use the new arrays.
            engine = std::make_unique<SearchEngine>(index);
            continue;
        }
        // Merge another snapshot file into this index via appendIndex.
        if (line.rfind(L"!merge ", 0) == 0) {
            std::wstring f = line.substr(7);
            Index other;
            std::vector<RootMeta> otherRoots;
            if (!loadSnapshot(other, otherRoots, f)) {
                printLine("merge load failed: cannot open " + utf16ToUtf8(f));
                continue;
            }
            const size_t before = index.size();
            index.appendIndex(other);
            engine = std::make_unique<SearchEngine>(index);
            printLine("merged " + std::to_string(other.size()) +
                      " entries (was " + std::to_string(before) + ", now " +
                      std::to_string(index.size()) + ")");
            continue;
        }
        // Count entries / tombstones for round-trip verification.
        if (line == L"!stats") {
            printLine("entries " + std::to_string(index.size()) +
                      ", tombstones " + std::to_string(index.tombstoneCount()));
            continue;
        }
        // Save the current index to a file (compacts tombstones on write).
        if (line.rfind(L"!save ", 0) == 0) {
            std::wstring f = line.substr(6);
            std::vector<RootMeta> rms;
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            const uint64_t nowFt = (static_cast<uint64_t>(ft.dwHighDateTime)
                                    << 32) | ft.dwLowDateTime;
            const auto& es = index.entries();
            for (size_t i = 0; i < es.size(); ++i) {
                if (es[i].parentIdx == Index::kNoParent) {
                    RootMeta rm;
                    rm.rootEntryIdx = static_cast<uint32_t>(i);
                    rm.crawledAtFiletime = nowFt;
                    rm.rootPathU8 = index.name(static_cast<uint32_t>(i));
                    rms.push_back(std::move(rm));
                }
            }
            printLine(saveSnapshot(index, rms, f) ? "saved." : "save FAILED.");
            continue;
        }

        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        SearchResult res = engine->search(line, 50);
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
