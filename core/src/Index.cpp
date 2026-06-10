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

} // namespace exsearcher
