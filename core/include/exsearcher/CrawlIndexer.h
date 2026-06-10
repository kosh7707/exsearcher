#pragma once

#include "exsearcher/Index.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace exsearcher {

struct CrawlStats {
    uint64_t totalFiles = 0;
    uint64_t totalDirs = 0;     // includes root entries
    uint64_t skippedDirs = 0;   // access-denied / unreadable directories
    uint64_t elapsedMs = 0;
};

// Parallel BFS crawler that fills an Index from a set of root paths.
class CrawlIndexer {
public:
    // progress(entriesSoFar) is called at most every ~100k entries.
    using ProgressCallback = std::function<void(uint64_t)>;

    explicit CrawlIndexer(Index& index);

    void setThreadCount(unsigned n) { threadCount_ = n; }
    void setProgressCallback(ProgressCallback cb) { progress_ = std::move(cb); }

    // Crawl all roots (e.g. "C:\\", "D:\\some\\dir"). Blocks until done.
    CrawlStats crawl(const std::vector<std::wstring>& roots);

private:
    struct WorkItem {
        std::wstring path;     // directory path WITHOUT long-path prefix
        uint32_t parentIdx;    // entry index of this directory
    };

    void workerLoop();
    void processDirectory(const WorkItem& item);

    Index& index_;
    unsigned threadCount_ = 0;
    ProgressCallback progress_;

    // Shared work queue + synchronization live in the .cpp via an opaque state
    // pointer to keep windows.h out of this header.
    struct State;
    State* st_ = nullptr;
};

} // namespace exsearcher
