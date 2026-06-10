#include "exsearcher/SearchEngine.h"
#include "WinText.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <unordered_set>

namespace exsearcher {

namespace {

// Substring search of needle within [hay, hay+hayLen). Plain memmem-style.
bool contains(const char* hay, size_t hayLen, const std::string& needle) {
    if (needle.empty())
        return true;
    if (needle.size() > hayLen)
        return false;
    const char first = needle[0];
    const size_t nlen = needle.size();
    const char* end = hay + (hayLen - nlen) + 1;
    const char* p = hay;
    while (p < end) {
        const char* hit =
            static_cast<const char*>(std::memchr(p, first, static_cast<size_t>(end - p)));
        if (!hit)
            return false;
        if (std::memcmp(hit, needle.data(), nlen) == 0)
            return true;
        p = hit + 1;
    }
    return false;
}

std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ')
            ++i;
        size_t start = i;
        while (i < s.size() && s[i] != ' ')
            ++i;
        if (i > start)
            tokens.emplace_back(s.substr(start, i - start));
    }
    return tokens;
}

} // namespace

SearchEngine::SearchEngine(const Index& index) : index_(index) {}

SearchResult SearchEngine::search(const std::wstring& query,
                                  uint32_t maxResults,
                                  const std::vector<uint32_t>* allowedRoots) const {
    SearchResult result;

    // Case-fold the query the same way names were folded during indexing.
    std::wstring folded = wintext::caseFold(query);
    std::string foldedU8 = wintext::toUtf8(folded);
    std::vector<std::string> tokens = tokenize(foldedU8);
    if (tokens.empty())
        return result;  // empty query = no results

    const auto& entries = index_.entries();
    const size_t total = entries.size();
    if (total == 0)
        return result;
    const char* lower = index_.lowerData();

    // Build fast lookup set for allowed roots once (empty allowedRoots = allow all).
    std::unordered_set<uint32_t> rootSet;
    const bool filterRoots = (allowedRoots != nullptr) && !allowedRoots->empty();
    if (filterRoots)
        rootSet.insert(allowedRoots->begin(), allowedRoots->end());

    unsigned threads = threadCount_;
    if (threads == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        threads = hc ? (hc < 8u ? hc : 8u) : 4u;
    }
    if (threads > total)
        threads = static_cast<unsigned>(total);
    if (threads == 0)
        threads = 1;

    auto nameMatches = [&](const FileEntry& fe) -> bool {
        // Tombstoned (logically deleted) entries never match. Cheap branch on
        // the already-loaded attr word; runs in both the single- and
        // multi-threaded paths via this shared lambda.
        if (fe.attr & kAttrTombstone)
            return false;
        const char* name = lower + fe.nameOffset;
        const size_t len = fe.nameLen;
        for (const auto& tok : tokens)
            if (!contains(name, len, tok))
                return false;
        return true;
    };

    auto rootAllowed = [&](uint32_t idx) -> bool {
        if (!filterRoots)
            return true;
        return rootSet.count(index_.rootOf(idx)) > 0;
    };

    if (threads == 1) {
        for (size_t i = 0; i < total; ++i) {
            if (nameMatches(entries[i]) && rootAllowed(static_cast<uint32_t>(i))) {
                ++result.totalMatches;
                if (result.indices.size() < maxResults)
                    result.indices.push_back(static_cast<uint32_t>(i));
            }
        }
        return result;
    }

    struct Chunk {
        std::vector<uint32_t> indices;
        uint64_t count = 0;
    };
    std::vector<Chunk> chunks(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);

    const size_t per = (total + threads - 1) / threads;
    for (unsigned t = 0; t < threads; ++t) {
        const size_t begin = std::min(static_cast<size_t>(t) * per, total);
        const size_t end = std::min(begin + per, total);
        pool.emplace_back([&, begin, end, t] {
            Chunk& c = chunks[t];
            for (size_t i = begin; i < end; ++i) {
                if (nameMatches(entries[i]) && rootAllowed(static_cast<uint32_t>(i))) {
                    ++c.count;
                    // Collect up to the cap per chunk; merge trims globally.
                    if (c.indices.size() < maxResults)
                        c.indices.push_back(static_cast<uint32_t>(i));
                }
            }
        });
    }
    for (auto& th : pool)
        th.join();

    for (auto& c : chunks)
        result.totalMatches += c.count;

    // Merge in chunk order (ascending index) up to the cap.
    for (auto& c : chunks) {
        for (uint32_t idx : c.indices) {
            if (result.indices.size() >= maxResults)
                break;
            result.indices.push_back(idx);
        }
        if (result.indices.size() >= maxResults)
            break;
    }

    return result;
}

} // namespace exsearcher
