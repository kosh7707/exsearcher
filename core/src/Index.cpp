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
}

void Index::removeRoot(uint32_t rootIdx) {
    std::lock_guard<std::mutex> lk(mutex_);

    const size_t oldCount = entries_.size();
    if (rootIdx >= oldCount)
        return;

    // First pass: decide which entries to keep. An entry is kept when walking
    // its parentIdx chain does NOT terminate at rootIdx. Walking per-entry would
    // be O(n*depth); instead compute "removed" with a single forward pass since
    // parents always precede children in append order (BFS / root-first).
    std::vector<uint8_t> removed(oldCount, 0);
    for (size_t i = 0; i < oldCount; ++i) {
        const FileEntry& fe = entries_[i];
        if (i == rootIdx) {
            removed[i] = 1;
        } else if (fe.parentIdx != kNoParent && removed[fe.parentIdx]) {
            // Parent was removed => this descendant is removed too. Relies on
            // parentIdx < i for all non-root entries (guaranteed by crawl/load
            // ordering: a directory is appended before its children).
            removed[i] = 1;
        }
    }

    // Build old->new index map and rebuild buffers for the survivors.
    std::vector<uint32_t> remap(oldCount, kNoParent);
    std::vector<FileEntry> newEntries;
    std::string newNames;
    std::string newLower;
    newEntries.reserve(oldCount);
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
        // parentIdx remapped below once the full map is known. Store old parent
        // for now; we resolve in a second pass.
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
}

} // namespace exsearcher
