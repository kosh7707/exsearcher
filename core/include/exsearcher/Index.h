#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <mutex>

namespace exsearcher {

// Custom attr bit reserved for tombstoned (logically deleted) entries. Real
// FILE_ATTRIBUTE_* flags never use 0x80000000, so this cannot collide with a
// genuine attribute read from the filesystem. Tombstoned entries are skipped by
// the search engine and physically dropped by the next snapshot save (which
// compacts them out), so deletes are O(1) and compaction is deferred/batched.
constexpr uint32_t kAttrTombstone = 0x80000000u;

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

    // Merge another index into this one. Appends other's name buffers, then
    // appends its entries with parentIdx remapped by + the current entry count
    // (kNoParent stays kNoParent). other's parent-before-child order is
    // preserved, so the combined index keeps the "parent precedes child"
    // invariant the search/compaction/path code relies on. Used by the
    // non-blocking rescan swap; caller must hold the controller's mutation
    // barrier (no concurrent search/crawl).
    void appendIndex(const Index& other);

    // Update size/mtime of a single entry in place (live MODIFIED events).
    // Caller holds the mutation barrier. attr is left untouched so a tombstone
    // bit set by a prior delete is not accidentally cleared.
    void updateMeta(uint32_t idx, uint64_t size, uint64_t mtime);

    // Mark an entry as logically deleted by setting the tombstone attr bit. The
    // entry stays in the arrays (offsets/indices unchanged) but is skipped by
    // search and dropped on the next save. Marking a directory does NOT cascade
    // to its descendants — callers tombstone descendants explicitly (the watcher
    // does a prefix/parent-chain scan). Idempotent.
    void markDeleted(uint32_t idx);
    bool isDeleted(uint32_t idx) const {
        return (entries_[idx].attr & kAttrTombstone) != 0;
    }

    // Number of entries currently carrying the tombstone bit.
    size_t tombstoneCount() const { return tombstoneCount_; }

    size_t size() const { return entries_.size(); }

    const FileEntry& entry(uint32_t idx) const { return entries_[idx]; }
    const std::vector<FileEntry>& entries() const { return entries_; }

    // Raw lowercase mirror buffer for the search engine to scan directly.
    const char* lowerData() const { return nameLowerBuf_.data(); }
    size_t lowerSize() const { return nameLowerBuf_.size(); }

    // Raw primary (non-folded) UTF-8 name buffer. Exposed for snapshot save.
    const char* nameData() const { return nameBuf_.data(); }
    size_t nameSize() const { return nameBuf_.size(); }

    // Replace the entire index contents in one shot. Used by snapshot load to
    // restore a previously saved index. Caller guarantees no concurrent access
    // (load happens before any threads touch the index). The two name buffers
    // must align byte-for-byte with the offsets/lengths stored in entries.
    void restore(std::vector<FileEntry>&& entries, std::string&& names,
                 std::string&& lower);

    // Remove every entry whose root is rootIdx and compact the index: rebuild
    // both name buffers and remap parentIdx via an old->new index map. Caller
    // guarantees no concurrent access (search pool drained, no crawl running).
    void removeRoot(uint32_t rootIdx);

    // Physically drop every tombstoned entry and compact (rebuild buffers,
    // remap parentIdx). A tombstoned directory's surviving descendants are
    // dropped too: an entry is removed if it is tombstoned OR its parent was
    // removed. Caller guarantees no concurrent access. Returns the number of
    // entries removed.
    size_t compactTombstones();

    // Reconstruct the full path of an entry by walking the parentIdx chain.
    std::string fullPath(uint32_t idx) const;

    // UTF-8 name of a single entry.
    std::string name(uint32_t idx) const;

    // Approximate memory footprint: entries + both name buffers.
    size_t memoryBytes() const;

    // Walk parentIdx chain to find the root entry (the one with kNoParent).
    uint32_t rootOf(uint32_t idx) const;

private:
    // Keep-predicate compaction core shared by removeRoot/compactTombstones.
    // `drop(i)` decides removal for an entry whose parent (if any) was already
    // decided; it receives the entry index and whether its parent was dropped,
    // and must honor the parent-before-child ordering invariant. Caller holds
    // mutex_. Returns the number of entries removed.
    size_t compactImpl(const std::function<bool(size_t i, bool parentDropped)>& drop);

    mutable std::mutex mutex_;
    std::vector<FileEntry> entries_;
    std::string nameBuf_;       // contiguous UTF-8 names
    std::string nameLowerBuf_;  // parallel lowercase mirror (same offsets/lengths)
    size_t tombstoneCount_ = 0; // entries currently carrying kAttrTombstone
};

} // namespace exsearcher
