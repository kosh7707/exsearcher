#include "exsearcher/Index.h"

namespace exsearcher {

uint32_t Index::appendBatch(const std::vector<PendingEntry>& batch) {
    if (batch.empty())
        return UINT32_MAX;

    std::lock_guard<std::mutex> lk(mutex_);
    const uint32_t firstIdx = static_cast<uint32_t>(entries_.size());
    entries_.reserve(entries_.size() + batch.size());

    for (const auto& p : batch) {
        FileEntry fe{};
        fe.nameOffset = static_cast<uint32_t>(nameBuf_.size());
        fe.nameLen = static_cast<uint16_t>(p.name.size());
        fe.parentIdx = p.parentIdx;
        fe.size = p.size;
        fe.mtime = p.mtime;
        fe.attr = p.attr;
        entries_.push_back(fe);

        nameBuf_.append(p.name);
        // Lowercase mirror must align byte-for-byte with the primary buffer.
        // Callers guarantee equal length; defend against drift just in case.
        if (p.nameLower.size() == p.name.size())
            nameLowerBuf_.append(p.nameLower);
        else
            nameLowerBuf_.append(p.name);
    }
    return firstIdx;
}

uint32_t Index::appendOne(const PendingEntry& e) {
    std::vector<PendingEntry> batch{e};
    return appendBatch(batch);
}

void Index::appendIndex(const Index& other) {
    if (other.entries_.empty())
        return;

    std::lock_guard<std::mutex> lk(mutex_);
    const uint32_t offset = static_cast<uint32_t>(entries_.size());
    const uint32_t nameBase = static_cast<uint32_t>(nameBuf_.size());

    // Append name buffers wholesale; offsets shift by nameBase.
    nameBuf_.append(other.nameBuf_);
    nameLowerBuf_.append(other.nameLowerBuf_);

    entries_.reserve(entries_.size() + other.entries_.size());
    for (const FileEntry& fe : other.entries_) {
        FileEntry ne = fe;
        ne.nameOffset = fe.nameOffset + nameBase;
        ne.parentIdx =
            (fe.parentIdx == kNoParent) ? kNoParent : fe.parentIdx + offset;
        if (ne.attr & kAttrTombstone)
            ++tombstoneCount_;
        entries_.push_back(ne);
    }
}

void Index::updateMeta(uint32_t idx, uint64_t size, uint64_t mtime) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (idx >= entries_.size())
        return;
    entries_[idx].size = size;
    entries_[idx].mtime = mtime;
}

void Index::markDeleted(uint32_t idx) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (idx >= entries_.size())
        return;
    if ((entries_[idx].attr & kAttrTombstone) == 0) {
        entries_[idx].attr |= kAttrTombstone;
        ++tombstoneCount_;
    }
}

std::string Index::name(uint32_t idx) const {
    const FileEntry& fe = entries_[idx];
    return nameBuf_.substr(fe.nameOffset, fe.nameLen);
}

std::string Index::fullPath(uint32_t idx) const {
    // Walk parent chain collecting names, then join with '\\'.
    std::vector<std::string> parts;
    uint32_t cur = idx;
    while (cur != kNoParent) {
        const FileEntry& fe = entries_[cur];
        parts.emplace_back(nameBuf_.data() + fe.nameOffset, fe.nameLen);
        cur = fe.parentIdx;
    }

    std::string out;
    for (size_t i = parts.size(); i-- > 0;) {
        out += parts[i];
        if (i != 0) {
            // Root names are like "C:"; append a separator after them too.
            out += '\\';
        }
    }
    return out;
}

size_t Index::memoryBytes() const {
    return entries_.size() * sizeof(FileEntry)
         + nameBuf_.capacity()
         + nameLowerBuf_.capacity();
}

uint32_t Index::rootOf(uint32_t idx) const {
    uint32_t cur = idx;
    while (entries_[cur].parentIdx != kNoParent)
        cur = entries_[cur].parentIdx;
    return cur;
}

void Index::restore(std::vector<FileEntry>&& entries, std::string&& names,
                    std::string&& lower) {
    std::lock_guard<std::mutex> lk(mutex_);
    entries_ = std::move(entries);
    nameBuf_ = std::move(names);
    nameLowerBuf_ = std::move(lower);
    // A snapshot is saved already-compacted (tombstones dropped), so a freshly
    // loaded index normally has none; recount defensively in case a caller
    // restores arrays it built itself.
    tombstoneCount_ = 0;
    for (const FileEntry& fe : entries_)
        if (fe.attr & kAttrTombstone)
            ++tombstoneCount_;
}

size_t Index::compactImpl(
    const std::function<bool(size_t i, bool parentDropped)>& drop) {
    const size_t oldCount = entries_.size();

    // First pass: decide which entries to drop. A single forward pass suffices
    // because parents always precede children in append order (BFS / root-first
    // / appendIndex preserves it), so a parent's "dropped" decision is already
    // known when we reach a child. The predicate gets whether its parent was
    // dropped to implement the cascade rule.
    std::vector<uint8_t> removed(oldCount, 0);
    size_t removedCount = 0;
    for (size_t i = 0; i < oldCount; ++i) {
        const FileEntry& fe = entries_[i];
        const bool parentDropped =
            (fe.parentIdx != kNoParent) && removed[fe.parentIdx] != 0;
        if (drop(i, parentDropped)) {
            removed[i] = 1;
            ++removedCount;
        }
    }
    if (removedCount == 0)
        return 0;

    // Build old->new index map and rebuild buffers for the survivors.
    std::vector<uint32_t> remap(oldCount, kNoParent);
    std::vector<FileEntry> newEntries;
    std::string newNames;
    std::string newLower;
    newEntries.reserve(oldCount - removedCount);
    newNames.reserve(nameBuf_.size());
    newLower.reserve(nameLowerBuf_.size());

    for (size_t i = 0; i < oldCount; ++i) {
        if (removed[i])
            continue;
        const FileEntry& fe = entries_[i];
        FileEntry ne = fe;
        ne.nameOffset = static_cast<uint32_t>(newNames.size());
        newNames.append(nameBuf_.data() + fe.nameOffset, fe.nameLen);
        newLower.append(nameLowerBuf_.data() + fe.nameOffset, fe.nameLen);
        // parentIdx remapped below once the full map is known.
        remap[i] = static_cast<uint32_t>(newEntries.size());
        newEntries.push_back(ne);
    }

    // Second pass: remap parentIdx of survivors using the old->new map.
    for (size_t i = 0; i < oldCount; ++i) {
        if (removed[i])
            continue;
        const uint32_t newIdx = remap[i];
        const uint32_t oldParent = entries_[i].parentIdx;
        newEntries[newIdx].parentIdx =
            (oldParent == kNoParent) ? kNoParent : remap[oldParent];
    }

    entries_ = std::move(newEntries);
    nameBuf_ = std::move(newNames);
    nameLowerBuf_ = std::move(newLower);
    return removedCount;
}

void Index::removeRoot(uint32_t rootIdx) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (rootIdx >= entries_.size())
        return;

    // Drop the target root and (via the cascade) everything under it.
    compactImpl([rootIdx](size_t i, bool parentDropped) {
        return i == rootIdx || parentDropped;
    });
    // Recount tombstones: any tombstoned survivor outside the removed root is
    // still present, but the simplest correct thing is a fresh count.
    tombstoneCount_ = 0;
    for (const FileEntry& fe : entries_)
        if (fe.attr & kAttrTombstone)
            ++tombstoneCount_;
}

size_t Index::compactTombstones() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (tombstoneCount_ == 0)
        return 0;
    const size_t removed = compactImpl([this](size_t i, bool parentDropped) {
        return (entries_[i].attr & kAttrTombstone) != 0 || parentDropped;
    });
    tombstoneCount_ = 0;  // every tombstoned entry (and orphan) is now gone
    return removed;
}

} // namespace exsearcher
