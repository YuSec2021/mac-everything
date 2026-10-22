#include "SearchEngine.h"
#include "StringUtils.h"
#include <algorithm>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Trigram posting list intersection utilities
// ---------------------------------------------------------------------------

std::vector<uint32_t> SearchEngine::intersectPostingLists(
    const std::unordered_map<Trigram, me::PackedPostingList>& index,
    const std::string& keyword,
    bool& allFound) {
    allFound = true;
    auto keyTrigrams = ContentIndex::extractTrigrams(keyword);
    std::unordered_set<Trigram> unique(keyTrigrams.begin(), keyTrigrams.end());
    if (unique.empty()) { allFound = false; return {}; }

    std::vector<const me::PackedPostingList*> postings;
    for (Trigram t : unique) {
        auto it = index.find(t);
        if (it == index.end()) { allFound = false; return {}; }
        postings.push_back(&it->second);
    }

    std::sort(postings.begin(), postings.end(),
        [](const auto* a, const auto* b) { return a->size() < b->size(); });

    std::vector<uint32_t> result;
    result.reserve(postings[0]->size());
    result.assign(postings[0]->begin(), postings[0]->end());

    for (size_t i = 1; i < postings.size() && !result.empty(); i++) {
        const auto& other = *postings[i];
        std::vector<uint32_t> isect;
        isect.reserve(std::min(result.size(), other.size()));
        std::set_intersection(result.begin(), result.end(),
                              other.begin(), other.end(),
                              std::back_inserter(isect));
        result = std::move(isect);
    }
    return result;
}

std::vector<uint32_t> SearchEngine::intersectPostingListsMulti(
    const std::unordered_map<Trigram, me::PackedPostingList>& index,
    const std::vector<std::string>& segments,
    bool& allFound) {
    allFound = true;
    // Collect trigrams from all segments and deduplicate
    std::unordered_set<Trigram> unique;
    for (const auto& seg : segments) {
        auto segTrigrams = ContentIndex::extractTrigrams(seg);
        unique.insert(segTrigrams.begin(), segTrigrams.end());
    }
    if (unique.empty()) { allFound = false; return {}; }

    std::vector<const me::PackedPostingList*> postings;
    for (Trigram t : unique) {
        auto it = index.find(t);
        if (it == index.end()) { allFound = false; return {}; }
        postings.push_back(&it->second);
    }

    std::sort(postings.begin(), postings.end(),
        [](const auto* a, const auto* b) { return a->size() < b->size(); });

    std::vector<uint32_t> result;
    result.reserve(postings[0]->size());
    result.assign(postings[0]->begin(), postings[0]->end());

    for (size_t i = 1; i < postings.size() && !result.empty(); i++) {
        const auto& other = *postings[i];
        std::vector<uint32_t> isect;
        isect.reserve(std::min(result.size(), other.size()));
        std::set_intersection(result.begin(), result.end(),
                              other.begin(), other.end(),
                              std::back_inserter(isect));
        result = std::move(isect);
    }
    return result;
}

std::vector<uint32_t> SearchEngine::unionPostingListsMulti(
    const std::unordered_map<Trigram, me::PackedPostingList>& index,
    const std::vector<std::string>& segments) {
    // For each segment, intersect its trigrams to get that segment's candidates.
    // Then UNION across all segments. This is safe for both AND and OR semantics
    // from FilteredRE2 — wider than pure intersection but never drops results.
    std::vector<uint32_t> result;
    for (const auto& seg : segments) {
        bool allFound = false;
        auto segCands = intersectPostingLists(index, seg, allFound);
        if (!allFound) continue;  // skip atoms with no trigram coverage
        if (result.empty()) {
            result = std::move(segCands);
        } else {
            std::vector<uint32_t> merged;
            merged.reserve(result.size() + segCands.size());
            std::set_union(result.begin(), result.end(),
                           segCands.begin(), segCands.end(),
                           std::back_inserter(merged));
            result = std::move(merged);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Name trigram index build / add / remove
// ---------------------------------------------------------------------------

template<class Pool>
static std::unordered_map<Trigram, me::PackedPostingList>
buildPackedTrigramIndex(const Pool& pool, const std::vector<uint8_t>* types = nullptr) {
    struct Count { uint32_t count = 0, bytes = 0, previous = 0; };
    std::unordered_map<Trigram, Count> counts;
    auto visit = [&](auto&& append) {
        for (uint32_t i = 0; i < pool.entryCount(); ++i) {
            if (!pool.isLive(i) || (types && (*types)[i] == 0)) continue;
            auto trigrams = ContentIndex::extractTrigrams(std::string(pool.view(i)));
            std::sort(trigrams.begin(), trigrams.end());
            trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
            for (Trigram t : trigrams) append(t, i);
        }
    };
    // Allocate each list once; repeated growth leaves large allocator high-water marks.
    visit([&](Trigram t, uint32_t id) {
        auto& c = counts[t];
        ++c.count;
        c.bytes += me::PackedPostingList::encodedBytes(id - c.previous);
        c.previous = id;
    });
    std::unordered_map<Trigram, me::PackedPostingList> index;
    index.reserve(counts.size());
    for (const auto& [t, count] : counts) index[t].reserveEncoded(count.count, count.bytes);
    visit([&](Trigram t, uint32_t id) { index[t].push_back(id); });
    return index;
}

std::unordered_map<Trigram, me::PackedPostingList>
SearchEngine::buildTrigramIndexFromData(const std::vector<uint8_t>& types,
                                      const StringPool& namePool) {
    return buildPackedTrigramIndex(namePool, &types);
}

std::unordered_map<Trigram, me::PackedPostingList>
SearchEngine::buildTrigramIndexFromData(const std::vector<uint8_t>& types, FoldedNamePool::View pool) {
    return buildPackedTrigramIndex(pool, &types);
}

void SearchEngine::buildTrigramIndex() {
    nameTrigramIndex_ = buildTrigramIndexFromData(types_, names_.lower());
}

void SearchEngine::addTrigramsForRecord(uint32_t idx, const char* data, uint16_t len) {
    auto trigrams = ContentIndex::extractTrigrams(std::string(data, len));
    // P-2 fix: sort+unique instead of unordered_set
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());

    for (Trigram t : trigrams) {
        auto& list = nameTrigramIndex_[t];
        // Insert in sorted position to maintain sorted posting lists
        auto pos = std::lower_bound(list.begin(), list.end(), idx);
        const auto offset = pos - list.begin();
        me::reserveForAppend(list);
        list.insert(list.begin() + offset, idx);
    }
}

void SearchEngine::removeTrigramsForRecord(uint32_t idx) {
    if (idx >= names_.lower().entryCount() || !names_.lower().isLive(idx)) return;
    // Recompute trigrams from names_.lower() instead of storing per-record lists
    auto trigrams = ContentIndex::extractTrigrams(std::string(names_.lower().data(idx), names_.lower().length(idx)));
    std::sort(trigrams.begin(), trigrams.end());
    trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
    for (Trigram t : trigrams) {
        auto it = nameTrigramIndex_.find(t);
        if (it != nameTrigramIndex_.end()) {
            auto& list = it->second;
            // Binary search in sorted list
            auto pos = std::lower_bound(list.begin(), list.end(), idx);
            if (pos != list.end() && *pos == idx) {
                list.erase(pos);
            }
            if (list.empty()) {
                nameTrigramIndex_.erase(it);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Extension index build / add / remove
// ---------------------------------------------------------------------------

/// Extract lowercase extension from name data. Returns empty string if no dot.
static std::string extractExtFromName(const char* data, uint16_t len) {
    int dotPos = -1;
    for (int i = static_cast<int>(len) - 1; i >= 0; --i) {
        if (data[i] == '.') { dotPos = i; break; }
    }
    if (dotPos < 0 || dotPos == static_cast<int>(len) - 1) return {};
    // names_.lower() stores lowercase names already
    return std::string(data + dotPos + 1, len - dotPos - 1);
}

template<class Pool>
static std::unordered_map<std::string, std::vector<uint32_t>>
buildExtensions(const std::vector<uint8_t>& types, const Pool& namePool) {
    std::unordered_map<std::string, std::vector<uint32_t>> index;
    for (uint32_t i = 0; i < namePool.entryCount(); i++) {
        if (types[i] == 0 || !namePool.isLive(i)) continue;
        std::string ext = extractExtFromName(namePool.data(i), namePool.length(i));
        if (!ext.empty()) {
            index[ext].push_back(i);
        }
    }
    // Posting lists are already in order since we iterate i = 0..N.
    for (auto& [_, list] : index) list.shrink_to_fit();
    return index;
}

std::unordered_map<std::string, std::vector<uint32_t>>
SearchEngine::buildExtensionIndexFromData(const std::vector<uint8_t>& types, const StringPool& pool) {
    return buildExtensions(types, pool);
}
std::unordered_map<std::string, std::vector<uint32_t>>
SearchEngine::buildExtensionIndexFromData(const std::vector<uint8_t>& types, FoldedNamePool::View pool) {
    return buildExtensions(types, pool);
}

void SearchEngine::buildExtensionIndex() {
    extensionIndex_ = buildExtensionIndexFromData(types_, names_.lower());
}

void SearchEngine::addExtensionForRecord(uint32_t idx) {
    if (idx >= names_.lower().entryCount() || !names_.lower().isLive(idx)) return;
    std::string ext = extractExtFromName(names_.lower().data(idx), names_.lower().length(idx));
    if (ext.empty()) return;
    auto& list = extensionIndex_[ext];
    auto pos = std::lower_bound(list.begin(), list.end(), idx);
    const auto offset = pos - list.begin();
    me::reserveForAppend(list);
    list.insert(list.begin() + offset, idx);
}

void SearchEngine::removeExtensionForRecord(uint32_t idx) {
    if (idx >= names_.lower().entryCount() || !names_.lower().isLive(idx)) return;
    std::string ext = extractExtFromName(names_.lower().data(idx), names_.lower().length(idx));
    if (ext.empty()) return;
    auto it = extensionIndex_.find(ext);
    if (it != extensionIndex_.end()) {
        auto& list = it->second;
        auto pos = std::lower_bound(list.begin(), list.end(), idx);
        if (pos != list.end() && *pos == idx) {
            list.erase(pos);
        }
        if (list.empty()) {
            extensionIndex_.erase(it);
        }
    }
}

void SearchEngine::removeIndexEntriesForRecords(const std::vector<uint32_t>& sortedIdx) {
    std::vector<std::pair<Trigram, uint32_t>> trigramPairs;
    std::unordered_map<std::string, std::vector<uint32_t>> byExt;
    for (uint32_t idx : sortedIdx) {
        if (idx >= names_.lower().entryCount() || !names_.lower().isLive(idx)) continue;
        const std::string name(names_.lower().data(idx), names_.lower().length(idx));
        auto trigrams = ContentIndex::extractTrigrams(name);
        std::sort(trigrams.begin(), trigrams.end());
        trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
        for (Trigram t : trigrams) trigramPairs.emplace_back(t, idx);
        std::string ext = extractExtFromName(name.data(), static_cast<uint16_t>(name.size()));
        if (!ext.empty()) byExt[std::move(ext)].push_back(idx);  // ascending: input is sorted
    }
    std::sort(trigramPairs.begin(), trigramPairs.end());
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < trigramPairs.size();) {
        const Trigram t = trigramPairs[i].first;
        ids.clear();
        for (; i < trigramPairs.size() && trigramPairs[i].first == t; ++i) ids.push_back(trigramPairs[i].second);
        auto it = nameTrigramIndex_.find(t);
        if (it == nameTrigramIndex_.end()) continue;
        it->second.eraseSorted(ids.data(), ids.data() + ids.size());
        if (it->second.empty()) nameTrigramIndex_.erase(it);
    }
    for (auto& [ext, removing] : byExt) {
        auto it = extensionIndex_.find(ext);
        if (it == extensionIndex_.end()) continue;
        auto& list = it->second;
        auto next = removing.begin();
        list.erase(std::remove_if(list.begin(), list.end(), [&](uint32_t id) {
            while (next != removing.end() && *next < id) ++next;
            if (next == removing.end() || *next != id) return false;
            ++next;
            return true;
        }), list.end());
        if (list.empty()) extensionIndex_.erase(it);
    }
}

std::vector<std::pair<std::string, std::vector<uint32_t>>> SearchEngine::postingListsForTesting() const {
    std::shared_lock lock(mutex_);
    std::vector<std::pair<std::string, std::vector<uint32_t>>> out;
    for (const auto& [t, list] : nameTrigramIndex_)
        out.emplace_back("t" + std::to_string(t), std::vector<uint32_t>(list.begin(), list.end()));
    for (const auto& [ext, list] : extensionIndex_) out.emplace_back("e" + ext, list);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Directory-to-record mapping
// ---------------------------------------------------------------------------

me::DirRecordMap
SearchEngine::buildPathIdxToRecordsFromData(const std::vector<uint8_t>& types,
                                            const std::vector<uint32_t>& pathIndices,
                                            uint32_t pathPoolSize) {
    return me::DirRecordMap::build(types, pathIndices, pathPoolSize);
}

void SearchEngine::rebuildPathIdxToRecords() {
    pathIdxToRecords_ = buildPathIdxToRecordsFromData(types_, pathIndices_, pathPool_.entryCount());
}

void SearchEngine::addPathRecord(uint32_t idx) {
    if (idx >= pathIndices_.size()) return;
    pathIdxToRecords_.append(pathIndices_[idx], idx);
}

// ---------------------------------------------------------------------------
// Dirty page tracking
// ---------------------------------------------------------------------------

void SearchEngine::markPageDirty(uint32_t recordIndex) {
    uint32_t page = recordIndex / kRecordsPerPage;
    if (page < dirtyPages_.size()) {
        dirtyPages_[page] = true;
    }
}

std::vector<uint32_t> SearchEngine::getDirtyPageNumbers() const {
    std::shared_lock lock(mutex_);
    std::vector<uint32_t> result;
    for (size_t i = 0; i < dirtyPages_.size(); i++) {
        if (dirtyPages_[i]) result.push_back(static_cast<uint32_t>(i));
    }
    return result;
}

void SearchEngine::clearDirtyPages() {
    auto lock = lockForMutation();
    std::fill(dirtyPages_.begin(), dirtyPages_.end(), false);
}
