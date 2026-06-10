#include "exsearcher/Snapshot.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <vector>

namespace exsearcher {

namespace {

constexpr char kMagic[5] = {'E', 'X', 'S', 'D', 'B'};
constexpr uint32_t kVersion = 1;

// Append raw bytes of a trivially-copyable value to a byte vector.
template <typename T>
void put(std::vector<char>& out, const T& v) {
    const char* p = reinterpret_cast<const char*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

void putBytes(std::vector<char>& out, const char* data, size_t n) {
    out.insert(out.end(), data, data + n);
}

// Read a trivially-copyable value from a buffer at offset, advancing offset.
// Returns false if there aren't enough bytes left.
template <typename T>
bool get(const std::vector<char>& in, size_t& off, T& v) {
    if (off + sizeof(T) > in.size())
        return false;
    std::memcpy(&v, in.data() + off, sizeof(T));
    off += sizeof(T);
    return true;
}

// Write a buffer to filePath atomically: write "<filePath>.tmp", then
// MoveFileExW over the destination.
bool writeFileAtomic(const std::wstring& filePath, const std::vector<char>& bytes) {
    const std::wstring tmp = filePath + L".tmp";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    size_t written = 0;
    bool ok = true;
    while (written < bytes.size()) {
        const size_t remaining = bytes.size() - written;
        const DWORD chunk = remaining > 0x40000000u
                                ? 0x40000000u
                                : static_cast<DWORD>(remaining);
        DWORD got = 0;
        if (!WriteFile(h, bytes.data() + written, chunk, &got, nullptr) ||
            got == 0) {
            ok = false;
            break;
        }
        written += got;
    }
    CloseHandle(h);

    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }

    if (!MoveFileExW(tmp.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// Read the entire file at filePath into bytes. Returns false on any error.
bool readWholeFile(const std::wstring& filePath, std::vector<char>& bytes) {
    HANDLE h = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h);
        return false;
    }

    bytes.resize(static_cast<size_t>(sz.QuadPart));
    size_t read = 0;
    bool ok = true;
    while (read < bytes.size()) {
        const size_t remaining = bytes.size() - read;
        const DWORD chunk = remaining > 0x40000000u
                                ? 0x40000000u
                                : static_cast<DWORD>(remaining);
        DWORD got = 0;
        if (!ReadFile(h, bytes.data() + read, chunk, &got, nullptr) ||
            got == 0) {
            ok = (read == bytes.size());
            break;
        }
        read += got;
    }
    CloseHandle(h);
    return ok && read == bytes.size();
}

} // namespace

bool saveSnapshot(const Index& index, const std::vector<RootMeta>& roots,
                  const std::wstring& filePath) {
    const std::vector<FileEntry>& srcEntries = index.entries();
    const char* srcNames = index.nameData();
    const char* srcLower = index.lowerData();

    // Compact tombstoned entries out so the persisted file (format v1) never
    // carries logically-deleted rows. An entry is dropped if it is tombstoned
    // OR its parent was dropped (parents precede children, so a single forward
    // pass is enough). Root entry indices in RootMeta are remapped through the
    // same old->new map. When nothing is tombstoned this serializes the buffers
    // as-is via the identity remap, keeping the common path cheap.
    const size_t oldCount = srcEntries.size();
    std::vector<FileEntry> entries;          // compacted copy to serialize
    std::string names;                       // compacted name buffer
    std::string lower;                       // compacted lowercase mirror
    std::vector<uint32_t> remap;             // old idx -> new idx (or kNoParent)

    if (index.tombstoneCount() == 0) {
        // Fast path: identity. Serialize the live buffers directly.
        entries.assign(srcEntries.begin(), srcEntries.end());
        names.assign(srcNames, static_cast<size_t>(index.nameSize()));
        lower.assign(srcLower, static_cast<size_t>(index.lowerSize()));
        remap.resize(oldCount);
        for (size_t i = 0; i < oldCount; ++i)
            remap[i] = static_cast<uint32_t>(i);
    } else {
        std::vector<uint8_t> removed(oldCount, 0);
        for (size_t i = 0; i < oldCount; ++i) {
            const FileEntry& fe = srcEntries[i];
            const bool parentDropped = (fe.parentIdx != Index::kNoParent) &&
                                       removed[fe.parentIdx] != 0;
            if ((fe.attr & kAttrTombstone) != 0 || parentDropped)
                removed[i] = 1;
        }

        remap.assign(oldCount, Index::kNoParent);
        entries.reserve(oldCount);
        names.reserve(index.nameSize());
        lower.reserve(index.lowerSize());

        for (size_t i = 0; i < oldCount; ++i) {
            if (removed[i])
                continue;
            const FileEntry& fe = srcEntries[i];
            FileEntry ne = fe;
            ne.nameOffset = static_cast<uint32_t>(names.size());
            names.append(srcNames + fe.nameOffset, fe.nameLen);
            lower.append(srcLower + fe.nameOffset, fe.nameLen);
            remap[i] = static_cast<uint32_t>(entries.size());
            entries.push_back(ne);
        }
        for (size_t i = 0; i < oldCount; ++i) {
            if (removed[i])
                continue;
            const uint32_t newIdx = remap[i];
            const uint32_t oldParent = srcEntries[i].parentIdx;
            entries[newIdx].parentIdx = (oldParent == Index::kNoParent)
                                            ? Index::kNoParent
                                            : remap[oldParent];
        }
    }

    const uint64_t entryCount = entries.size();
    const uint64_t nameBufSize = names.size();
    const uint64_t lowerBufSize = lower.size();

    std::vector<char> out;
    out.reserve(static_cast<size_t>(
        5 + 4 + 8 * 3 + entryCount * sizeof(FileEntry) + nameBufSize +
        lowerBufSize + 4 + roots.size() * 16));

    putBytes(out, kMagic, sizeof(kMagic));
    put(out, kVersion);
    put(out, entryCount);
    put(out, nameBufSize);
    put(out, lowerBufSize);

    if (entryCount)
        putBytes(out, reinterpret_cast<const char*>(entries.data()),
                 static_cast<size_t>(entryCount) * sizeof(FileEntry));
    if (nameBufSize)
        putBytes(out, names.data(), static_cast<size_t>(nameBufSize));
    if (lowerBufSize)
        putBytes(out, lower.data(), static_cast<size_t>(lowerBufSize));

    // Roots whose entry survived compaction (a root is dropped only if it was
    // itself tombstoned, which the controller never does). Remap indices.
    std::vector<const RootMeta*> keptRoots;
    keptRoots.reserve(roots.size());
    for (const RootMeta& rm : roots) {
        if (rm.rootEntryIdx < oldCount && remap[rm.rootEntryIdx] != Index::kNoParent)
            keptRoots.push_back(&rm);
    }

    const uint32_t rootCount = static_cast<uint32_t>(keptRoots.size());
    put(out, rootCount);
    for (const RootMeta* rmp : keptRoots) {
        const RootMeta& rm = *rmp;
        put(out, remap[rm.rootEntryIdx]);
        put(out, rm.crawledAtFiletime);
        const uint16_t pathLen =
            static_cast<uint16_t>(rm.rootPathU8.size() > 0xFFFFu
                                      ? 0xFFFFu
                                      : rm.rootPathU8.size());
        put(out, pathLen);
        putBytes(out, rm.rootPathU8.data(), pathLen);
    }

    return writeFileAtomic(filePath, out);
}

bool loadSnapshot(Index& index, std::vector<RootMeta>& roots,
                  const std::wstring& filePath) {
    std::vector<char> in;
    if (!readWholeFile(filePath, in))
        return false;

    size_t off = 0;

    // Magic + version.
    if (in.size() < sizeof(kMagic))
        return false;
    if (std::memcmp(in.data(), kMagic, sizeof(kMagic)) != 0)
        return false;
    off += sizeof(kMagic);

    uint32_t version = 0;
    if (!get(in, off, version) || version != kVersion)
        return false;

    uint64_t entryCount = 0, nameBufSize = 0, lowerBufSize = 0;
    if (!get(in, off, entryCount) || !get(in, off, nameBufSize) ||
        !get(in, off, lowerBufSize))
        return false;

    // Reject implausible sizes up front to avoid huge allocations on garbage.
    // entryCount must stay below the u32 kNoParent sentinel; buffers capped
    // at ~8 GiB, generous for this tool.
    if (entryCount >= 0xFFFFFFFFull || nameBufSize > (1ull << 33) ||
        lowerBufSize > (1ull << 33))
        return false;
    if (nameBufSize != lowerBufSize)
        return false;  // mirrors must align byte-for-byte

    const size_t entriesBytes =
        static_cast<size_t>(entryCount) * sizeof(FileEntry);
    if (off + entriesBytes > in.size())
        return false;

    std::vector<FileEntry> entries(static_cast<size_t>(entryCount));
    if (entryCount)
        std::memcpy(entries.data(), in.data() + off, entriesBytes);
    off += entriesBytes;

    if (off + nameBufSize > in.size())
        return false;
    std::string names(in.data() + off, static_cast<size_t>(nameBufSize));
    off += static_cast<size_t>(nameBufSize);

    if (off + lowerBufSize > in.size())
        return false;
    std::string lower(in.data() + off, static_cast<size_t>(lowerBufSize));
    off += static_cast<size_t>(lowerBufSize);

    // Structural validation. Index::name()/fullPath() index the name buffer
    // unchecked, and removeRoot's single-pass compaction relies on parents
    // preceding children — a corrupt file must not get to violate either.
    for (size_t i = 0; i < entries.size(); ++i) {
        const FileEntry& fe = entries[i];
        if (static_cast<uint64_t>(fe.nameOffset) + fe.nameLen > nameBufSize)
            return false;
        if (fe.parentIdx != Index::kNoParent && fe.parentIdx >= i)
            return false;
    }

    uint32_t rootCount = 0;
    if (!get(in, off, rootCount))
        return false;
    if (rootCount > entryCount + 1)  // sanity: cannot exceed entries (+slack)
        return false;

    std::vector<RootMeta> loadedRoots;
    loadedRoots.reserve(rootCount);
    for (uint32_t i = 0; i < rootCount; ++i) {
        RootMeta rm;
        uint16_t pathLen = 0;
        if (!get(in, off, rm.rootEntryIdx) ||
            !get(in, off, rm.crawledAtFiletime) || !get(in, off, pathLen))
            return false;
        if (off + pathLen > in.size())
            return false;
        rm.rootPathU8.assign(in.data() + off, pathLen);
        off += pathLen;
        // Root records must point at actual root entries.
        if (rm.rootEntryIdx >= entryCount ||
            entries[rm.rootEntryIdx].parentIdx != Index::kNoParent)
            return false;
        loadedRoots.push_back(std::move(rm));
    }

    // The file size must match EXACTLY what we computed; trailing garbage or a
    // truncation past the structured part means a corrupt file.
    if (off != in.size())
        return false;

    index.restore(std::move(entries), std::move(names), std::move(lower));
    roots = std::move(loadedRoots);
    return true;
}

} // namespace exsearcher
