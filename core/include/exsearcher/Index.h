#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

namespace exsearcher {

struct FileEntry {
    uint32_t nameOffset;   // offset into name buffer (UTF-8)
    uint16_t nameLen;      // bytes (UTF-8)
    uint32_t parentIdx;    // index of parent directory entry; UINT32_MAX for roots
    uint64_t size;
    uint64_t mtime;        // FILETIME as uint64
    uint32_t attr;         // FILE_ATTRIBUTE_* flags
};

// One file/dir to be appended. Names are UTF-8; the lowercase mirror must have
// identical byte length (callers guarantee this via case-fold-before-UTF8).
struct PendingEntry {
    std::string name;       // UTF-8
    std::string nameLower;  // UTF-8, case-folded, same byte length as name
    uint32_t parentIdx;
    uint64_t size;
    uint64_t mtime;
    uint32_t attr;
};

// In-memory, Everything-style filename index.
// Names live in one contiguous UTF-8 buffer; a parallel lowercase mirror buffer
// uses the SAME offsets/lengths for case-insensitive substring search.
class Index {
public:
    static constexpr uint32_t kNoParent = UINT32_MAX;

    // Append a batch of entries (one directory's worth) under a single lock.
    // Returns the index assigned to the first entry of the batch; subsequent
    // entries are contiguous. Empty batches return UINT32_MAX.
    uint32_t appendBatch(const std::vector<PendingEntry>& batch);

    // Thread-safe single append (used for root entries before crawling).
    uint32_t appendOne(const PendingEntry& e);

    size_t size() const { return entries_.size(); }

    const FileEntry& entry(uint32_t idx) const { return entries_[idx]; }
    const std::vector<FileEntry>& entries() const { return entries_; }

    // Raw lowercase mirror buffer for the search engine to scan directly.
    const char* lowerData() const { return nameLowerBuf_.data(); }
    size_t lowerSize() const { return nameLowerBuf_.size(); }

    // Reconstruct the full path of an entry by walking the parentIdx chain.
    std::string fullPath(uint32_t idx) const;

    // UTF-8 name of a single entry.
    std::string name(uint32_t idx) const;

    // Approximate memory footprint: entries + both name buffers.
    size_t memoryBytes() const;

    // Walk parentIdx chain to find the root entry (the one with kNoParent).
    uint32_t rootOf(uint32_t idx) const;

private:
    mutable std::mutex mutex_;
    std::vector<FileEntry> entries_;
    std::string nameBuf_;       // contiguous UTF-8 names
    std::string nameLowerBuf_;  // parallel lowercase mirror (same offsets/lengths)
};

} // namespace exsearcher
