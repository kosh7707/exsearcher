#pragma once

#include "exsearcher/Index.h"

#include <cstdint>
#include <string>
#include <vector>

namespace exsearcher {

struct SearchResult {
    std::vector<uint32_t> indices;  // matching entry indices (capped)
    uint64_t totalMatches = 0;      // total matches found, even beyond the cap
};

// Multithreaded substring search over the index's lowercase mirror buffer.
class SearchEngine {
public:
    explicit SearchEngine(const Index& index);

    void setThreadCount(unsigned n) { threadCount_ = n; }

    // query: UTF-16 input. Case-folded, converted to UTF-8, split on spaces.
    // AND semantics: an entry matches if EVERY token is a substring of its
    // lowercase name. maxResults caps collected indices but not totalMatches.
    // allowedRoots: if non-null, only entries whose root index is in the set
    // are returned. Filter is applied BEFORE counting so totalMatches reflects
    // the true count under the filter. Pass nullptr to disable filtering.
    SearchResult search(const std::wstring& query, uint32_t maxResults,
                        const std::vector<uint32_t>* allowedRoots = nullptr) const;

private:
    const Index& index_;
    unsigned threadCount_ = 0;
};

} // namespace exsearcher
