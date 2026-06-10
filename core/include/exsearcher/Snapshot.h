#pragma once

#include "exsearcher/Index.h"

#include <cstdint>
#include <string>
#include <vector>

namespace exsearcher {

// Per-root metadata persisted alongside the index. rootEntryIdx is the entry
// index of the root in the Index; rootPathU8 is its UTF-8 path (e.g. "C:");
// crawledAtFiletime is a Windows FILETIME (100ns ticks since 1601) of the crawl.
struct RootMeta {
    uint32_t rootEntryIdx = 0;
    uint64_t crawledAtFiletime = 0;
    std::string rootPathU8;
};

// Serialize an Index plus its roots metadata to a binary .exsdb file.
//
// Binary format (little-endian, packed):
//   char[5]  magic   = "EXSDB"
//   u32      version = 1
//   u64      entryCount
//   u64      nameBufSize
//   u64      lowerBufSize
//   FileEntry[entryCount]   raw entries array
//   char[nameBufSize]       raw primary name buffer
//   char[lowerBufSize]      raw lowercase mirror buffer
//   u32      rootCount
//   { u32 rootEntryIdx; u64 crawledAt; u16 pathLen; char[pathLen] pathU8 } * rootCount
//
// Written atomically: data goes to "<filePath>.tmp" first, then renamed over
// the destination via MoveFileExW(MOVEFILE_REPLACE_EXISTING).
bool saveSnapshot(const Index& index, const std::vector<RootMeta>& roots,
                  const std::wstring& filePath);

// Load a snapshot written by saveSnapshot. On success, fills index (via
// Index::restore) and roots, and returns true. On ANY validation failure
// (bad magic/version, size mismatch, truncation), returns false and leaves
// index/roots unspecified — the caller should treat that as "start empty".
// Must be called before any threads touch the index.
bool loadSnapshot(Index& index, std::vector<RootMeta>& roots,
                  const std::wstring& filePath);

} // namespace exsearcher
