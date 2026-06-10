#include "exsearcher/CrawlIndexer.h"
#include "WinText.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace exsearcher {

struct CrawlIndexer::State {
    std::mutex queueMutex;
    std::condition_variable cv;
    std::deque<WorkItem> queue;

    // pending = directories pushed but not yet fully processed.
    // Termination is driven by this hitting zero, NOT by an empty queue.
    std::atomic<uint64_t> pending{0};
    bool done = false;

    std::atomic<uint64_t> totalFiles{0};
    std::atomic<uint64_t> totalDirs{0};
    std::atomic<uint64_t> skippedDirs{0};

    // Progress: entries indexed so far, reported every ~100k.
    std::atomic<uint64_t> entriesIndexed{0};
    std::atomic<uint64_t> nextProgressMark{100000};
};

namespace {

// Build a path with the "\\?\" long-path prefix for FindFirstFileExW so paths
// >260 chars work. The search pattern "\*" is appended by the caller.
std::wstring withLongPrefix(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0)
        return path;
    if (path.rfind(L"\\\\", 0) == 0) {
        // UNC path: \\server\share -> \\?\UNC\server\share
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        return dir + name;
    return dir + L'\\' + name;
}

} // namespace

CrawlIndexer::CrawlIndexer(Index& index) : index_(index) {}

CrawlStats CrawlIndexer::crawl(const std::vector<std::wstring>& roots) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    State st;
    st_ = &st;

    unsigned threads = threadCount_;
    if (threads == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        threads = hc ? (hc < 8u ? hc : 8u) : 4u;
    }

    // Seed the queue with root directory entries.
    for (const auto& rawRoot : roots) {
        // Normalize: strip any long-path prefix and a single trailing separator
        // for the stored name, but keep "C:\" form for enumeration.
        std::wstring root = rawRoot;
        if (root.rfind(L"\\\\?\\", 0) == 0)
            root = root.substr(4);

        // Root entry name: e.g. "C:" (drive) or the last path component.
        std::wstring storeName = root;
        while (!storeName.empty() &&
               (storeName.back() == L'\\' || storeName.back() == L'/'))
            storeName.pop_back();

        // For a bare drive like "C:" the stored name is "C:".
        // For "D:\foo\bar" the root entry name is the full "D:\foo\bar" so its
        // children reconstruct correctly (we don't index ancestors).
        DWORD attrs = GetFileAttributesW(withLongPrefix(root).c_str());
        uint32_t dirAttr = FILE_ATTRIBUTE_DIRECTORY;
        uint64_t mtime = 0;
        if (attrs != INVALID_FILE_ATTRIBUTES)
            dirAttr = attrs;

        PendingEntry rootEntry;
        rootEntry.name = wintext::toUtf8(storeName);
        std::wstring folded = wintext::caseFold(storeName);
        std::string foldedU8 = wintext::toUtf8(folded);
        rootEntry.nameLower =
            (foldedU8.size() == rootEntry.name.size()) ? foldedU8 : rootEntry.name;
        rootEntry.parentIdx = Index::kNoParent;
        rootEntry.size = 0;
        rootEntry.mtime = mtime;
        rootEntry.attr = dirAttr;

        uint32_t idx = index_.appendOne(rootEntry);
        st.totalDirs.fetch_add(1, std::memory_order_relaxed);
        st.entriesIndexed.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(st.queueMutex);
            st.pending.fetch_add(1, std::memory_order_relaxed);
            st.queue.push_back(WorkItem{storeName, idx});
        }
    }
    st.cv.notify_all();

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned i = 0; i < threads; ++i)
        pool.emplace_back([this] { workerLoop(); });
    for (auto& t : pool)
        t.join();

    st_ = nullptr;

    CrawlStats stats;
    stats.totalFiles = st.totalFiles.load();
    stats.totalDirs = st.totalDirs.load();
    stats.skippedDirs = st.skippedDirs.load();
    stats.elapsedMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start)
            .count());
    return stats;
}

void CrawlIndexer::workerLoop() {
    State& st = *st_;
    for (;;) {
        WorkItem item;
        {
            std::unique_lock<std::mutex> lk(st.queueMutex);
            st.cv.wait(lk, [&] { return !st.queue.empty() || st.done; });
            if (st.queue.empty()) {
                if (st.done)
                    return;
                continue;
            }
            item = std::move(st.queue.front());
            st.queue.pop_front();
        }

        // Skip the actual enumeration when cancelled, but still account for the
        // pending decrement below so termination logic stays correct.
        if (!cancel_.load(std::memory_order_relaxed))
            processDirectory(item);

        // Decrement pending AFTER finishing this directory. When the last
        // outstanding directory completes, signal all workers to exit.
        if (st.pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(st.queueMutex);
            st.done = true;
            st.cv.notify_all();
        }
    }
}

void CrawlIndexer::processDirectory(const WorkItem& item) {
    State& st = *st_;

    const std::wstring pattern = joinPath(withLongPrefix(item.path), L"*");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        st.skippedDirs.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::vector<PendingEntry> batch;
    std::vector<std::wstring> subdirNames;  // names of subdirs to descend into
    std::vector<size_t> subdirBatchPos;     // batch positions of those subdirs

    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0')))
            continue;  // skip "." and ".."

        std::wstring wname(n);

        PendingEntry pe;
        pe.name = wintext::toUtf8(wname);
        std::wstring folded = wintext::caseFold(wname);
        std::string foldedU8 = wintext::toUtf8(folded);
        pe.nameLower =
            (foldedU8.size() == pe.name.size()) ? foldedU8 : pe.name;
        pe.parentIdx = item.parentIdx;
        pe.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        pe.mtime = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                   fd.ftLastWriteTime.dwLowDateTime;
        pe.attr = fd.dwFileAttributes;

        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isReparse =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        const size_t batchPos = batch.size();
        batch.push_back(std::move(pe));

        if (isDir) {
            st.totalDirs.fetch_add(1, std::memory_order_relaxed);
            // Index the reparse point itself but do NOT descend into it
            // (avoids junction/symlink loops, e.g. C:\Users\... junctions).
            if (!isReparse) {
                subdirNames.push_back(std::move(wname));
                subdirBatchPos.push_back(batchPos);
            }
        } else {
            st.totalFiles.fetch_add(1, std::memory_order_relaxed);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);

    // Append the whole directory under a single lock, then map subdir names to
    // their freshly-assigned entry indices to enqueue them.
    if (batch.empty())
        return;

    const uint32_t firstIdx = index_.appendBatch(batch);

    const uint64_t indexedNow =
        st.entriesIndexed.fetch_add(batch.size(), std::memory_order_relaxed) +
        batch.size();
    // Fire progress at most every ~100k entries.
    if (progress_) {
        uint64_t mark = st.nextProgressMark.load(std::memory_order_relaxed);
        while (indexedNow >= mark &&
               st.nextProgressMark.compare_exchange_weak(
                   mark, mark + 100000, std::memory_order_relaxed)) {
            progress_(indexedNow);
            mark += 100000;
        }
    }

    if (subdirNames.empty())
        return;

    // Batch entries are contiguous from firstIdx in push order, so a subdir's
    // entry index is firstIdx + its recorded batch position. We do NOT re-read
    // the shared index here: another thread may be reallocating entries_.
    std::vector<WorkItem> toPush;
    toPush.reserve(subdirNames.size());
    for (size_t k = 0; k < subdirNames.size(); ++k) {
        const uint32_t childIdx =
            firstIdx + static_cast<uint32_t>(subdirBatchPos[k]);
        toPush.push_back(WorkItem{joinPath(item.path, subdirNames[k]), childIdx});
    }

    {
        std::lock_guard<std::mutex> lk(st.queueMutex);
        st.pending.fetch_add(toPush.size(), std::memory_order_relaxed);
        for (auto& w : toPush)
            st.queue.push_back(std::move(w));
    }
    st.cv.notify_all();
}

} // namespace exsearcher
