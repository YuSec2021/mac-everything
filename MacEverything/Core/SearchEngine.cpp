#include "SearchEngine.h"
#include "StringUtils.h"
#include "IndexWAL.h"
#include "Logger.h"
#include <algorithm>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstring>

std::string SearchEngine::makeFullPath(std::string_view path, std::string_view name) {
    if (path.empty() || path.back() == '/') {
        std::string result;
        result.reserve(path.size() + name.size());
        result.append(path);
        result.append(name);
        return result;
    }
    std::string result;
    result.reserve(path.size() + 1 + name.size());
    result.append(path);
    result += '/';
    result.append(name);
    return result;
}

std::string SearchEngine::lowerFullPath(uint32_t index) const {
    return makeFullPath(pathPool_.lower().view(pathIndices_[index]), names_.lower().view(index));
}

uint64_t SearchEngine::pathKeyHash(uint32_t canonDir, std::string_view lowerName) {
    uint64_t h = std::hash<std::string_view>{}(lowerName) ^ (uint64_t(canonDir) * 0x9e3779b97f4a7c15ULL);
    // splitmix64 finalizer: the table picks buckets from the high bits.
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    return h ^ (h >> 31);
}

uint32_t SearchEngine::canonicalDir(const me::CompactPathIndex& lowerLookup, DirectoryPool::LowerView lower,
                                    const std::vector<uint32_t>& canon, const std::string& lowerDir, uint32_t self) {
    auto resolve = [&](uint32_t i) { return lower.view(i); };
    auto it = me::findPathRecord(lowerLookup, lowerDir, resolve);
    if (it != lowerLookup.end()) return canon[it->second];
    // makeFullPath joins "a" and "a/" alike, so both spellings share an ID.
    if (lowerDir.empty()) return self;
    std::string alternate;
    if (lowerDir.back() != '/') alternate = lowerDir + '/';
    else if (lowerDir.size() >= 2 && lowerDir[lowerDir.size() - 2] != '/') alternate = lowerDir.substr(0, lowerDir.size() - 1);
    else return self;
    it = me::findPathRecord(lowerLookup, alternate, resolve);
    return it != lowerLookup.end() ? canon[it->second] : self;
}

uint32_t SearchEngine::findPathRecord(std::string_view lowerFull) const {
    const size_t slash = lowerFull.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? lowerFull : lowerFull.substr(slash + 1);
    if (name.empty()) return kNoRecord;
    // makeFullPath(dir, name) gives lowerFull for dir "prefix/" or "prefix", or "" without a slash.
    const auto keys = pathKeys();
    auto probe = [&](const std::string& dir, bool& dirFound) {
        auto it = me::findPathRecord(lowerPathLookup_, dir, [this](uint32_t i) { return pathPool_.lower().view(i); });
        dirFound = it != lowerPathLookup_.end();
        if (!dirFound) return kNoRecord;
        const uint32_t canon = dirCanon_[it->second];
        return pathIndex_.find(pathKeyHash(canon, name), [&](uint32_t r) { return keys.matches(r, canon, name); });
    };
    bool dirFound = false;
    if (slash == std::string_view::npos) return probe(std::string(), dirFound);
    const uint32_t found = probe(std::string(lowerFull.substr(0, slash + 1)), dirFound);
    // Both spellings share a canonical ID, so one existing directory settles it.
    if (dirFound || slash == 0 || lowerFull[slash - 1] == '/') return found;
    return probe(std::string(lowerFull.substr(0, slash)), dirFound);
}

void SearchEngine::setPathRecord(uint32_t index) {
    const auto keys = pathKeys();
    const uint32_t canon = dirCanon_[pathIndices_[index]];
    const auto name = names_.lower().view(index);
    pathIndex_.assign(pathKeyHash(canon, name), index,
                      [&](uint32_t r) { return keys.matches(r, canon, name); },
                      [&](uint32_t r) { return keys.hashOf(r); });
}

void SearchEngine::erasePathRecord(uint32_t index) {
    if (pathIndex_.empty() || index >= pathIndices_.size()) return;
    const auto keys = pathKeys();
    const size_t slot = pathIndex_.slotOf(index, keys.hashOf(index));
    if (slot != me::PathRecordTable::npos) pathIndex_.eraseSlot(slot, [&](uint32_t r) { return keys.hashOf(r); });
}

uint32_t SearchEngine::rebuildPathIndex() {
    const auto keys = pathKeys();
    const uint32_t n = static_cast<uint32_t>(types_.size());
    pathIndex_.clear();
    pathIndex_.reserve(n - static_cast<uint32_t>(std::count(types_.begin(), types_.end(), 0)),
                       [&](uint32_t r) { return keys.hashOf(r); });
    for (uint32_t i = 0; i < n; ++i) {
        if (types_[i] != 0) setPathRecord(i);
    }
    // Tombstone orphaned duplicates: records not in pathIndex_ as winners
    std::vector<bool> isWinner(n, false);
    pathIndex_.forEach([&](uint32_t r) { isWinner[r] = true; });
    uint32_t live = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (types_[i] == 0) continue;
        if (isWinner[i]) live++;
        else tombstoneAt(i);
    }
    return live;
}

std::vector<uint32_t> SearchEngine::pathRecordsUnder(const std::string& lowerPrefix, bool mirrorsFilesystem) const {
    std::vector<uint32_t> matches;
    // Paths below the prefix start with "prefix/". The root is already "/";
    // appending another slash would give "//", which no path starts with.
    const std::string below = lowerPrefix == "/" ? lowerPrefix : lowerPrefix + '/';
    if (pathIdxToRecords_.empty()) {
        // No directory map yet: check every mapped record.
        pathIndex_.forEach([&](uint32_t idx) {
            const auto path = lowerFullPath(idx);
            if (path == lowerPrefix || path.compare(0, below.size(), below) == 0) {
                matches.push_back(idx);
            }
        });
    } else {
        // lowerFullPath is the directory joined as "dir/" plus the name. A path
        // below the prefix therefore has a joined directory starting with
        // "prefix/"; only a record equal to the prefix sits in a shallower one.
        const auto lower = pathPool_.lower();
        const auto keys = pathKeys();
        auto mapped = [&](uint32_t idx) {
            // Orphaned duplicates are live but unmapped, and stay untouched.
            return pathIndex_.slotOf(idx, keys.hashOf(idx)) != me::PathRecordTable::npos;
        };
        if (mirrorsFilesystem) {
            // When the index follows the filesystem, every record below a directory
            // hangs off a chain of directory records, so walking the directory map
            // down from the prefix visits exactly the subtree, in time proportional
            // to its size instead of to the number of directories in the index.
            std::vector<std::string> pending{lowerPrefix};
            while (!pending.empty()) {
                const std::string dirLower = std::move(pending.back());
                pending.pop_back();
                // makeFullPath joins "a" and "a/" alike, so look up both spellings.
                const std::string alternate = !dirLower.empty() && dirLower.back() == '/'
                    ? dirLower.substr(0, dirLower.size() - 1) : dirLower + '/';
                for (const std::string* key : {&dirLower, &alternate}) {
                    auto it = me::findPathRecord(lowerPathLookup_, *key, [&](uint32_t i) { return lower.view(i); });
                    if (it == lowerPathLookup_.end()) continue;
                    pathIdxToRecords_.forEach(it->second, types_, [&](uint32_t idx) {
                        if (mapped(idx)) matches.push_back(idx);
                        if (types_[idx] == 2 || types_[idx] == 5) pending.push_back(lowerFullPath(idx));
                    });
                }
            }
        } else {
            std::string dir;
            for (uint32_t d = 0; d < lower.entryCount(); ++d) {
                if (size_t(lower.length(d)) + 1 < below.size()) continue;
                lower.reconstruct(d, dir);
                if (!dir.empty() && dir.back() != '/') dir += '/';
                if (dir.compare(0, below.size(), below) != 0) continue;
                pathIdxToRecords_.forEach(d, types_, [&](uint32_t idx) {
                    if (mapped(idx)) matches.push_back(idx);
                });
            }
        }
        if (const uint32_t idx = findPathRecord(lowerPrefix); idx != kNoRecord) matches.push_back(idx);
    }
    // Removing while iterating would shift entries, so callers mutate afterwards.
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return matches;
}

void SearchEngine::rebuildDirLookups() {
    pathLookup_.clear();
    lowerPathLookup_.clear();
    const auto lower = pathPool_.lower();
    dirCanon_.resize(pathPool_.entryCount());
    for (uint32_t i = 0; i < pathPool_.entryCount(); i++) {
        dirCanon_[i] = i;
        if (!pathPool_.isLive(i)) continue;
        me::setPathRecord(pathLookup_, pathPool_.view(i), i,
                          [this](uint32_t j) { return pathPool_.view(j); });
        const auto lowerDir = lower.view(i);
        dirCanon_[i] = canonicalDir(lowerPathLookup_, lower, dirCanon_, lowerDir, i);
        me::setPathRecord(lowerPathLookup_, lowerDir, i, [&](uint32_t j) { return lower.view(j); });
    }
}

uint32_t SearchEngine::internPath(const std::string& path) {
    auto it = me::findPathRecord(pathLookup_, path, [this](uint32_t i) { return pathPool_.view(i); });
    if (it != pathLookup_.end()) return it->second;
    uint32_t pIdx = pathPool_.append(path);
    pathLookup_.emplace(me::PathHash{}(path), pIdx);
    const auto lowerDir = pathPool_.lower().view(pIdx);
    me::reserveForAppend(dirCanon_);
    dirCanon_.resize(pIdx + 1, pIdx);
    dirCanon_[pIdx] = canonicalDir(lowerPathLookup_, pathPool_.lower(), dirCanon_, lowerDir, pIdx);
    me::setPathRecord(lowerPathLookup_, lowerDir, pIdx,
                      [this](uint32_t i) { return pathPool_.lower().view(i); });
    return pIdx;
}

void SearchEngine::tombstoneAt(uint32_t idx) {
    erasePathRecord(idx); // needs the record's name, cleared below
    types_[idx] = 0;
    markPageDirty(idx);
    sizes_.set(idx, 0);
    modTimes_.set(idx, 0);
    names_.tombstone(idx);
}

void SearchEngine::pushRecord(FileRecord&& rec) {
    me::reserveForAppend(types_);
    me::reserveForAppend(sizes_);
    me::reserveForAppend(modTimes_);
    me::reserveForAppend(pathIndices_);
    types_.push_back(rec.type);
    sizes_.push_back(rec.size);
    modTimes_.push_back(static_cast<int64_t>(rec.modTime));
}

void SearchEngine::loadRecords(std::vector<FileRecord>&& records) {
    auto lock = lockForMutation();
    compactionGen_.fetch_add(1, std::memory_order_relaxed);

    const size_t n = records.size();

    // Build SoA columns from incoming records
    types_.resize(n);
    sizes_.resize(n);
    modTimes_.resize(n);
    for (size_t i = 0; i < n; i++) {
        types_[i] = records[i].type;
        sizes_.set(i, records[i].size);
        modTimes_.set(i, static_cast<int64_t>(records[i].modTime));
    }

    // Pre-compute lowercase names into a temporary vector (parallel),
    // then bulk-load into names_.lower() (single-threaded append)
    std::vector<std::string> tempLowerNames(n);
    {
        unsigned numThreads = std::thread::hardware_concurrency();
        if (numThreads < 1) numThreads = 1;
        if (numThreads > 32) numThreads = 32;
        size_t chunkSize = (n + numThreads - 1) / numThreads;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (unsigned t = 0; t < numThreads; t++) {
            size_t start = t * chunkSize;
            size_t end = std::min(start + chunkSize, n);
            if (start >= end) break;
            threads.emplace_back([&records, &tempLowerNames, start, end] {
                for (size_t i = start; i < end; i++) {
                    tempLowerNames[i] = me::toLower(records[i].name);
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    names_.clear();
    size_t nameBytes = 0;
    for (size_t i = 0; i < n; ++i) nameBytes += records[i].name.size() + (records[i].name == tempLowerNames[i] ? 0 : tempLowerNames[i].size());
    names_.reserve(n, nameBytes);
    for (size_t i = 0; i < n; ++i) names_.append(records[i].name, tempLowerNames[i]);
    std::vector<std::string>{}.swap(tempLowerNames);

    // Path deduplication: intern paths into pathPool_ + pathPool_.lower() + pathLookup_
    pathPool_.clear();
    pathLookup_.clear();
    lowerPathLookup_.clear();
    dirCanon_.clear();
    pathIndex_.clear();
    pathIndices_.resize(n);
    for (size_t i = 0; i < n; i++) {
        pathIndices_[i] = internPath(records[i].path);
    }

    uint32_t actualLive = rebuildPathIndex();

    // Build trigram index for fast filename search
    buildTrigramIndex();
    rebuildPathIdxToRecords();
    // Build extension index for fast ext: filter queries
    buildExtensionIndex();
    rebuildRecentCache();

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Initialize dirty page bitmap (no pages dirty after initial load)
    uint32_t pageCount = (static_cast<uint32_t>(n) + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);
}

void SearchEngine::loadRecordsV5(std::vector<FileRecord>&& records,
                                  std::vector<std::string>&& lowerNames,
                                  std::vector<uint32_t>&& pathIndices,
                                  StringPool&& pathDict,
                                  StringPool&& lowerPathDict) {
    auto lock = lockForMutation();
    compactionGen_.fetch_add(1, std::memory_order_relaxed);

    const size_t n = records.size();

    // Build SoA columns from incoming records
    types_.resize(n);
    sizes_.resize(n);
    modTimes_.resize(n);
    for (size_t i = 0; i < n; i++) {
        types_[i] = records[i].type;
        sizes_.set(i, records[i].size);
        modTimes_.set(i, static_cast<int64_t>(records[i].modTime));
    }

    names_.clear();
    size_t nameBytes = 0;
    for (size_t i = 0; i < n; ++i) nameBytes += records[i].name.size() + (records[i].name == lowerNames[i] ? 0 : lowerNames[i].size());
    names_.reserve(n, nameBytes);
    for (size_t i = 0; i < n; ++i) names_.append(records[i].name, lowerNames[i]);
    std::vector<std::string>{}.swap(lowerNames);

    // Install pre-built path dictionaries (skip path dedup + internPath loop)
    pathPool_ = DirectoryPool(pathDict);
    pathDict = StringPool{};
    lowerPathDict = StringPool{};
    pathIndices_ = std::move(pathIndices);

    // Rebuild pathLookup_ and lowerPathLookup_ from pathPool_ entries
    pathLookup_.clear();
    lowerPathLookup_.clear();
    pathLookup_.reserve(pathPool_.entryCount());
    lowerPathLookup_.reserve(pathPool_.entryCount());
    rebuildDirLookups();

    uint32_t actualLive = rebuildPathIndex();

    // Build trigram index for fast filename search
    buildTrigramIndex();
    rebuildPathIdxToRecords();
    buildExtensionIndex();
    rebuildRecentCache();

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Initialize dirty page bitmap
    uint32_t pageCount = (static_cast<uint32_t>(n) + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);
}

FileRecord SearchEngine::getRecord(uint32_t index) const {
    std::shared_lock lock(mutex_);
    if (index >= types_.size()) return {};
    FileRecord r;
    r.type = types_[index];
    if (r.type == 0) return r;
    r.name = names_.original().str(index);
    r.path = pathPool_.str(pathIndices_[index]);
    r.size = sizes_[index];
    r.modTime = static_cast<time_t>(modTimes_[index]);
    return r;
}

uint32_t SearchEngine::recordCount() const {
    std::shared_lock lock(mutex_);
    return static_cast<uint32_t>(types_.size());
}

uint32_t SearchEngine::addRecord(FileRecord&& record) {
    auto lock = lockForMutation();

    uint32_t idx = static_cast<uint32_t>(types_.size());
    std::string fullPath = makeFullPath(record.path, record.name);
    std::string lowerFull = me::toLower(fullPath);
    std::string lower = me::toLower(record.name);

    if (wal_) wal_->append(WALOp::Add, fullPath, record);

    // Tombstone existing record at same path to prevent orphaned duplicates
    if (const uint32_t oldIdx = findPathRecord(lowerFull); oldIdx != kNoRecord) {
        time_t oldModTime = static_cast<time_t>(modTimes_[oldIdx]);
        removeTrigramsForRecord(oldIdx);
        removePathRecord(oldIdx);
        tombstoneAt(oldIdx);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(oldIdx, oldModTime);
    }

    // Intern path before moving record
    uint32_t pIdx = internPath(record.path);
    record.path.clear();
    record.path.shrink_to_fit();

    names_.append(record.name, lower);
    pushRecord(std::move(record));
    pathIndices_.push_back(pIdx);
    setPathRecord(idx);

    // Update trigram index
    addTrigramsForRecord(idx, names_.lower().data(idx), names_.lower().length(idx));
    addPathRecord(idx);
    addExtensionForRecord(idx);

    // Dirty page tracking
    if (idx / kRecordsPerPage >= dirtyPages_.size()) {
        dirtyPages_.resize(idx / kRecordsPerPage + 1, false);
    }
    markPageDirty(idx);

    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(idx, static_cast<time_t>(modTimes_[idx]));

    return idx;
}

bool SearchEngine::removeByPathUnlocked(const std::string& fullPath) {
    const uint32_t idx = findPathRecord(me::toLower(fullPath));
    if (idx == kNoRecord) return false;

    if (wal_) wal_->append(WALOp::Remove, fullPath);

    time_t oldModTime = static_cast<time_t>(modTimes_[idx]);
    removeTrigramsForRecord(idx);
    removePathRecord(idx);
    removeExtensionForRecord(idx);
    tombstoneAt(idx);

    liveCount_.fetch_sub(1, std::memory_order_relaxed);
    removeFromRecentCache(idx, oldModTime);

    return true;
}

bool SearchEngine::removeByPath(const std::string& fullPath) {
    auto lock = lockForMutation();
    return removeByPathUnlocked(fullPath);
}

uint32_t SearchEngine::removeByPathPrefix(const std::string& pathPrefix) {
    auto lock = lockForMutation();
    return removeByPathPrefixUnlocked(pathPrefix);
}

uint32_t SearchEngine::removeByPathPrefixUnlocked(const std::string& pathPrefix, bool mirrorsFilesystem) {
    std::string lowerPrefix = me::toLower(pathPrefix);
    uint32_t removed = 0;
    const auto matches = pathRecordsUnder(lowerPrefix, mirrorsFilesystem);
    // Name indexes must be cleaned before tombstoning clears names_.lower().
    const bool batched = matches.size() >= kBatchIndexRemoval;
    if (batched) removeIndexEntriesForRecords(matches);
    for (uint32_t idx : matches) {
        // H-4: Write WAL entry for each removed path
        if (wal_) {
            std::string fullPath = makeFullPath(pathPool_.str(pathIndices_[idx]), names_.original().str(idx));
            wal_->append(WALOp::Remove, fullPath);
        }

        time_t oldModTime = static_cast<time_t>(modTimes_[idx]);
        if (!batched) removeTrigramsForRecord(idx);
        removePathRecord(idx);
        if (!batched) removeExtensionForRecord(idx);
        tombstoneAt(idx);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(idx, oldModTime);
        removed++;
    }

    return removed;
}

uint32_t SearchEngine::batchRescanPrefix(const std::string& pathPrefix,
                                         std::vector<FileRecord>&& freshRecords) {
    auto lock = lockForMutation();

    // ── Phase 1: Tombstone old records matching prefix ──
    // Remove trigrams incrementally for each tombstoned record.
    std::string lowerPrefix = me::toLower(pathPrefix);
    uint32_t removed = 0;
    const auto matches = pathRecordsUnder(lowerPrefix);
    const bool batched = matches.size() >= kBatchIndexRemoval;
    if (batched) removeIndexEntriesForRecords(matches);
    for (uint32_t idx : matches) {
        // Write WAL Remove entry for each removed path
        if (wal_) {
            std::string fullPath = makeFullPath(pathPool_.str(pathIndices_[idx]), names_.original().str(idx));
            wal_->append(WALOp::Remove, fullPath);
        }

        if (!batched) removeTrigramsForRecord(idx);
        removePathRecord(idx);
        if (!batched) removeExtensionForRecord(idx);
        tombstoneAt(idx);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removed++;
    }

    // ── Phase 2: Add fresh records with incremental trigram insertion ──
    for (auto& record : freshRecords) {
        uint32_t newIdx = static_cast<uint32_t>(types_.size());
        std::string fullPath = makeFullPath(record.path, record.name);
        std::string lower = me::toLower(record.name);

        if (wal_) wal_->append(WALOp::Update, fullPath, record);

        uint32_t pIdx = internPath(record.path);
        record.path.clear();
        record.path.shrink_to_fit();

        names_.append(record.name, lower);
        pushRecord(std::move(record));
        pathIndices_.push_back(pIdx);
        setPathRecord(newIdx);
        addTrigramsForRecord(newIdx, names_.lower().data(newIdx), names_.lower().length(newIdx));
        addPathRecord(newIdx);
        addExtensionForRecord(newIdx);
        if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
            dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
        }
        markPageDirty(newIdx);
        liveCount_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Phase 3: Rebuild recent cache ──
    rebuildRecentCache();

    return removed;
}

void SearchEngine::updateByPathUnlocked(const std::string& fullPath, FileRecord&& updated) {
    if (wal_) wal_->append(WALOp::Update, fullPath, updated);

    // Remove old record if exists (case-insensitive lookup)
    if (const uint32_t idx = findPathRecord(me::toLower(fullPath)); idx != kNoRecord) {
        time_t oldModTime = static_cast<time_t>(modTimes_[idx]);
        // Clean up trigram index (must happen before tombstoning names_.lower())
        removeTrigramsForRecord(idx);
        removePathRecord(idx);
        removeExtensionForRecord(idx);
        tombstoneAt(idx);
        liveCount_.fetch_sub(1, std::memory_order_relaxed);
        removeFromRecentCache(idx, oldModTime);
    }

    // Add new record
    uint32_t newIdx = static_cast<uint32_t>(types_.size());
    std::string lower = me::toLower(updated.name);

    // Intern path before moving record
    uint32_t pIdx = internPath(updated.path);
    updated.path.clear();
    updated.path.shrink_to_fit();

    names_.append(updated.name, lower);
    pushRecord(std::move(updated));
    pathIndices_.push_back(pIdx);
    setPathRecord(newIdx);
    addTrigramsForRecord(newIdx, names_.lower().data(newIdx), names_.lower().length(newIdx));
    addPathRecord(newIdx);
    addExtensionForRecord(newIdx);
    if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
        dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
    }
    markPageDirty(newIdx);
    liveCount_.fetch_add(1, std::memory_order_relaxed);
    addToRecentCache(newIdx, static_cast<time_t>(modTimes_[newIdx]));
}

void SearchEngine::updateByPath(const std::string& fullPath, FileRecord&& updated) {
    auto lock = lockForMutation();
    updateByPathUnlocked(fullPath, std::move(updated));
}

void SearchEngine::batchMutate(std::vector<MutationOp>&& ops) {
    if (ops.empty()) return;
    auto lock = lockForMutation();
    for (auto& op : ops) {
        if (op.type == MutationOp::REMOVE) {
            removeByPathUnlocked(op.path);
        } else if (op.type == MutationOp::REMOVE_TREE) {
            removeByPathPrefixUnlocked(op.path, /*mirrorsFilesystem=*/true);
        } else {
            updateByPathUnlocked(op.path, std::move(op.record));
        }
    }
}

std::unordered_map<uint32_t, uint32_t> SearchEngine::compactRecords() {
    // Compaction renumbers every record, and Phase 3 compares its snapshot with the
    // live data by those numbers. A second run overlapping the first would see every
    // snapshot path as deleted, so it leaves the work to the run in progress.
    std::unique_lock compactionGuard(compactionMutex_, std::try_to_lock);
    if (!compactionGuard.owns_lock()) return {};

    // ── Phase 1: Snapshot under shared_lock ──
    // Queries continue unblocked during snapshot copy.
    std::vector<uint8_t> snapTypes;
    SizeColumn snapSizes;
    ModTimeColumn snapModTimes;
    FoldedNamePool snapNames;
    const auto snapNamePool = snapNames.lower();
    const auto snapOrigNamePool = snapNames.original();
    std::vector<uint32_t> snapPathIndices;
    DirectoryPool snapPathPool;
    me::PathRecordTable snapPathIndex;
    std::vector<uint32_t> snapDirCanon;
    uint32_t snapSize;
    uint64_t snapGeneration;
    {
        std::shared_lock lock(mutex_);
        uint32_t live = liveCount_.load(std::memory_order_relaxed);
        if (live == types_.size()) return {}; // nothing to compact
        snapGeneration = compactionGen_.load(std::memory_order_relaxed);
        snapTypes = types_;
        snapSizes = sizes_;
        snapModTimes = modTimes_;
        snapNames = names_;
        snapPathIndices = pathIndices_;
        snapPathPool = pathPool_;
        snapPathIndex = pathIndex_;
        snapDirCanon = dirCanon_;
        snapSize = static_cast<uint32_t>(types_.size());
    }
    // Live records not referenced by pathIndex_ are orphaned duplicates.
    std::vector<bool> snapWinner(snapSize, false);
    snapPathIndex.forEach([&](uint32_t r) { snapWinner[r] = true; });

    LOG_INFO("SearchEngine", "COW compaction Phase 2: building compacted data from "
             << snapSize << " records");

    // ── Phase 2: Build compacted data (no lock held) ──
    // Mutations (addRecord/removeByPath/updateByPath/batchRescanPrefix) continue
    // on the live data; they will be replayed in Phase 3.
    std::unordered_map<uint32_t, uint32_t> remap;
    remap.reserve(snapSize);

    std::vector<uint8_t> cdTypes;
    SizeColumn cdSizes;
    ModTimeColumn cdModTimes;
    cdTypes.reserve(snapSize);
    cdSizes.reserve(snapSize);
    cdModTimes.reserve(snapSize);
    FoldedNamePool cdNames;
    const auto cdNamePool = cdNames.lower();
    const auto cdOrigNamePool = cdNames.original();
    std::vector<uint32_t> cdPathIndices;
    cdPathIndices.reserve(snapSize);
    DirectoryPool cdPathPool;
    const auto cdLowerPathPool = cdPathPool.lower();
    me::CompactPathIndex cdPathLookup;
    me::CompactPathIndex cdLowerPathLookup;
    std::vector<uint32_t> cdDirCanon;

    for (size_t i = 0; i < snapSize; i++) {
        if (snapTypes[i] == 0 || !snapWinner[i]) continue;
        std::string origPath = snapPathPool.str(snapPathIndices[i]);
        uint32_t newIdx = static_cast<uint32_t>(cdTypes.size());
        remap[static_cast<uint32_t>(i)] = newIdx;
        // Intern path into compacted pool (both original and lowered)
        uint32_t newPIdx;
        auto cdPlIt = me::findPathRecord(cdPathLookup, origPath, [&](uint32_t i) { return cdPathPool.view(i); });
        if (cdPlIt != cdPathLookup.end()) {
            newPIdx = cdPlIt->second;
        } else {
            newPIdx = cdPathPool.append(origPath);
            cdPathLookup.emplace(me::PathHash{}(origPath), newPIdx);
            const auto lowerDir = cdLowerPathPool.view(newPIdx);
            cdDirCanon.resize(newPIdx + 1, newPIdx);
            cdDirCanon[newPIdx] = canonicalDir(cdLowerPathLookup, cdLowerPathPool, cdDirCanon, lowerDir, newPIdx);
            me::setPathRecord(cdLowerPathLookup, lowerDir, newPIdx,
                              [&](uint32_t i) { return cdLowerPathPool.view(i); });
        }
        // Copy name from snapshot pool into compacted pool
        cdNames.append(snapOrigNamePool.view(i), snapNamePool.view(i));
        cdPathIndices.push_back(newPIdx);
        cdTypes.push_back(snapTypes[i]);
        cdSizes.push_back(snapSizes[i]);
        cdModTimes.push_back(snapModTimes[i]);
    }
    uint32_t cdLiveCount = static_cast<uint32_t>(cdTypes.size());
    me::PathRecordTable cdPathIndex;
    {
        const PathKeys keys{cdNamePool, cdPathIndices, cdDirCanon};
        auto hashOf = [&](uint32_t r) { return keys.hashOf(r); };
        cdPathIndex.reserve(cdLiveCount, hashOf);
        for (uint32_t r = 0; r < cdLiveCount; ++r) {
            const uint32_t canon = cdDirCanon[cdPathIndices[r]];
            const auto name = cdNamePool.view(r);
            cdPathIndex.assign(keys.hashOf(r), r, [&](uint32_t o) { return keys.matches(o, canon, name); }, hashOf);
        }
    }
    // These snapshot columns are no longer needed for the replay step.
    std::vector<uint8_t>{}.swap(snapTypes);
    SizeColumn{}.swap(snapSizes);
    ModTimeColumn{}.swap(snapModTimes);

    // Build trigram index, path trigram index, extension index, and recent cache outside any lock
    auto cdTrigramIndex = buildTrigramIndexFromData(cdTypes, cdNamePool);
    auto cdPathIdxToRecords = buildPathIdxToRecordsFromData(cdTypes, cdPathIndices, cdPathPool.entryCount());
    auto cdExtensionIndex = buildExtensionIndexFromData(cdTypes, cdNamePool);
    auto cdRecentCache = buildRecentCacheFromData(cdTypes, cdModTimes, kRecentCacheSize);

    LOG_INFO("SearchEngine", "COW compaction Phase 3: swapping data, compacted "
             << snapSize << " -> " << cdLiveCount << " records");

    // ── Phase 3: Swap + replay under unique_lock ──
    // This lock is held only long enough to move data and replay the small
    // number of mutations that occurred during Phase 2 (~milliseconds).
    {
        auto lock = lockForMutation();

        // A reload during Phase 2 replaced the records this snapshot describes.
        if (compactionGen_.load(std::memory_order_relaxed) != snapGeneration) {
            LOG_WARN("SearchEngine", "COW compaction abandoned: records were reloaded during Phase 2");
            return {};
        }

        // Move out current (mutated-during-Phase2) state
        auto oldTypes = std::move(types_);
        auto oldSizes = std::move(sizes_);
        auto oldModTimes = std::move(modTimes_);
        auto oldNames = std::move(names_);
        const auto oldOrigNamePool = oldNames.original();
        const auto oldNamePool = oldNames.lower();
        auto oldPathIndices = std::move(pathIndices_);
        auto oldPathPool = std::move(pathPool_);
        auto oldPathLookup = std::move(pathLookup_);
        auto oldLowerPathLookup = std::move(lowerPathLookup_);
        auto oldPathIndex = std::move(pathIndex_);
        auto oldDirCanon = std::move(dirCanon_);

        // Install compacted data
        types_ = std::move(cdTypes);
        sizes_ = std::move(cdSizes);
        modTimes_ = std::move(cdModTimes);
        names_ = std::move(cdNames);
        pathIndices_ = std::move(cdPathIndices);
        pathPool_ = std::move(cdPathPool);
        pathLookup_ = std::move(cdPathLookup);
        lowerPathLookup_ = std::move(cdLowerPathLookup);
        pathIndex_ = std::move(cdPathIndex);
        dirCanon_ = std::move(cdDirCanon);
        nameTrigramIndex_ = std::move(cdTrigramIndex);
        pathIdxToRecords_ = std::move(cdPathIdxToRecords);
        extensionIndex_ = std::move(cdExtensionIndex);
        recentCache_ = std::move(cdRecentCache);

        // Replay new records appended during Phase 2
        uint32_t replayedAdds = 0;
        for (size_t i = snapSize; i < oldTypes.size(); i++) {
            if (oldTypes[i] == 0) continue;
            uint32_t newIdx = static_cast<uint32_t>(types_.size());
            std::string origPath = oldPathPool.str(oldPathIndices[i]);
            uint32_t pIdx = internPath(origPath);
            // Copy name from old pool into current pool
            names_.append(oldOrigNamePool.view(i), oldNamePool.view(i));
            pathIndices_.push_back(pIdx);
            setPathRecord(newIdx);
            addTrigramsForRecord(newIdx, names_.lower().data(newIdx), names_.lower().length(newIdx));
            addToRecentCache(newIdx, static_cast<time_t>(oldModTimes[i]));
            // Push SoA columns for replayed record
            types_.push_back(oldTypes[i]);
            sizes_.push_back(oldSizes[i]);
            modTimes_.push_back(oldModTimes[i]);
            addPathRecord(newIdx);
            addExtensionForRecord(newIdx);
            cdLiveCount++;
            replayedAdds++;
        }

        // Replay tombstones: paths in snapshot but removed during Phase 2
        // Snapshot and old data share record and directory IDs below snapSize,
        // and canonical directory IDs never change, so snapshot keys probe old data.
        uint32_t replayedDeletes = 0;
        const PathKeys oldKeys{oldNamePool, oldPathIndices, oldDirCanon};
        snapPathIndex.forEach([&](uint32_t snapIdx) {
            const uint32_t canon = snapDirCanon[snapPathIndices[snapIdx]];
            const auto name = snapNamePool.view(snapIdx);
            if (oldPathIndex.find(pathKeyHash(canon, name),
                                  [&](uint32_t r) { return oldKeys.matches(r, canon, name); }) != kNoRecord) return;
            // This path was deleted during Phase 2
            auto it = remap.find(snapIdx);
            if (it == remap.end()) return; // was already tombstoned in snapshot
            uint32_t newIdx = it->second;
            if (newIdx < types_.size() && types_[newIdx] != 0) {
                removeTrigramsForRecord(newIdx);
                removePathRecord(newIdx);
                removeExtensionForRecord(newIdx);
                time_t oldMod = static_cast<time_t>(modTimes_[newIdx]);
                tombstoneAt(newIdx);
                removeFromRecentCache(newIdx, oldMod);
                cdLiveCount--;
                replayedDeletes++;
            }
        });

        liveCount_.store(cdLiveCount, std::memory_order_relaxed);
        compactionGen_.fetch_add(1, std::memory_order_relaxed);

        LOG_INFO("SearchEngine", "COW compaction done: replayed " << replayedAdds
                 << " adds, " << replayedDeletes << " deletes, live=" << cdLiveCount);
    }

    fullRewriteNeeded_.store(true, std::memory_order_relaxed);
    phase2Pending_.store(false, std::memory_order_release);

    return remap;
}

void SearchEngine::attachWAL(std::shared_ptr<IndexWAL> wal) {
    auto lock = lockForMutation();
    wal_ = std::move(wal);
}

void SearchEngine::detachWAL() {
    auto lock = lockForMutation();
    wal_.reset();
}

std::vector<uint32_t> SearchEngine::recentIndices(uint32_t count) const {
    std::shared_lock lock(mutex_);
    std::vector<uint32_t> result;
    uint32_t n = std::min(count, static_cast<uint32_t>(recentCache_.size()));
    result.reserve(n);
    auto it = recentCache_.begin();
    for (uint32_t i = 0; i < n; ++i, ++it) {
        result.push_back(it->index);
    }
    return result;
}

std::set<SearchEngine::RecentEntry>
SearchEngine::buildRecentCacheFromData(const std::vector<uint8_t>& types,
                                       const ModTimeColumn& modTimes,
                                       uint32_t cacheSize) {
    if (cacheSize == 0) return {};
    std::vector<RecentEntry> best;
    best.reserve(std::min<size_t>(cacheSize, types.size()));
    for (size_t i = 0; i < types.size(); ++i) {
        if (types[i] == 0) continue;
        RecentEntry entry{static_cast<time_t>(modTimes[i]), static_cast<uint32_t>(i)};
        if (best.size() < cacheSize) {
            best.push_back(entry);
            std::push_heap(best.begin(), best.end());
        } else if (entry < best.front()) {
            std::pop_heap(best.begin(), best.end());
            best.back() = entry;
            std::push_heap(best.begin(), best.end());
        }
    }
    return std::set<RecentEntry>(best.begin(), best.end());
}

void SearchEngine::rebuildRecentCache() {
    recentCache_ = buildRecentCacheFromData(types_, modTimes_, kRecentCacheSize);
}

void SearchEngine::addToRecentCache(uint32_t idx, time_t modTime) {
    recentCache_.insert({modTime, idx});
    if (recentCache_.size() > kRecentCacheSize) {
        recentCache_.erase(std::prev(recentCache_.end()));
    }
}

void SearchEngine::removeFromRecentCache(uint32_t idx, time_t modTime) {
    recentCache_.erase({modTime, idx});
}

uint32_t SearchEngine::indexForPath(const std::string& fullPath) const {
    std::shared_lock lock(mutex_);
    return findPathRecord(me::toLower(fullPath));
}

std::vector<FileRecord> SearchEngine::exportRecords() const {
    std::shared_lock lock(mutex_);
    std::vector<FileRecord> result;
    result.reserve(liveCount_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < types_.size(); i++) {
        if (types_[i] == 0) continue; // skip tombstones
        FileRecord r;
        r.name = names_.original().str(i);
        r.type = types_[i];
        r.size = sizes_[i];
        r.modTime = static_cast<time_t>(modTimes_[i]);
        r.path = pathPool_.str(pathIndices_[i]);
        result.push_back(std::move(r));
    }
    return result;
}

void SearchEngine::replayWALEntries(std::vector<WALEntry>&& entries) {
    if (entries.empty()) return;

    auto lock = lockForMutation();

    for (auto& e : entries) {
        std::string lowerFull = me::toLower(e.fullPath);

        switch (e.op) {
        case WALOp::Add: {
            // If path already exists (duplicate Add), tombstone old record first
            if (const uint32_t oldIdx = findPathRecord(lowerFull); oldIdx != kNoRecord) {
                removeTrigramsForRecord(oldIdx);
                removePathRecord(oldIdx);
                removeExtensionForRecord(oldIdx);
                tombstoneAt(oldIdx);
                liveCount_.fetch_sub(1, std::memory_order_relaxed);
            }

            uint32_t idx = static_cast<uint32_t>(types_.size());
            std::string lower = me::toLower(e.record.name);
            uint32_t pIdx = internPath(e.record.path);
            e.record.path.clear();
            e.record.path.shrink_to_fit();

            names_.append(e.record.name, lower);
            pushRecord(std::move(e.record));
            pathIndices_.push_back(pIdx);
            setPathRecord(idx);
            addTrigramsForRecord(idx, names_.lower().data(idx), names_.lower().length(idx));
            addPathRecord(idx);
            addExtensionForRecord(idx);
            if (idx / kRecordsPerPage >= dirtyPages_.size()) {
                dirtyPages_.resize(idx / kRecordsPerPage + 1, false);
            }
            markPageDirty(idx);
            liveCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case WALOp::Remove: {
            const uint32_t idx = findPathRecord(lowerFull);
            if (idx == kNoRecord) break; // silently ignore
            removeTrigramsForRecord(idx);
            removePathRecord(idx);
            removeExtensionForRecord(idx);
            tombstoneAt(idx);
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
            break;
        }
        case WALOp::Update: {
            // Remove old record if exists
            if (const uint32_t oldIdx = findPathRecord(lowerFull); oldIdx != kNoRecord) {
                removeTrigramsForRecord(oldIdx);
                removePathRecord(oldIdx);
                removeExtensionForRecord(oldIdx);
                tombstoneAt(oldIdx);
                liveCount_.fetch_sub(1, std::memory_order_relaxed);
            }
            // Add updated record
            uint32_t newIdx = static_cast<uint32_t>(types_.size());
            std::string lower = me::toLower(e.record.name);
            uint32_t pIdx = internPath(e.record.path);
            e.record.path.clear();
            e.record.path.shrink_to_fit();

            names_.append(e.record.name, lower);
            pushRecord(std::move(e.record));
            pathIndices_.push_back(pIdx);
            setPathRecord(newIdx);
            addTrigramsForRecord(newIdx, names_.lower().data(newIdx), names_.lower().length(newIdx));
            addPathRecord(newIdx);
            addExtensionForRecord(newIdx);
            if (newIdx / kRecordsPerPage >= dirtyPages_.size()) {
                dirtyPages_.resize(newIdx / kRecordsPerPage + 1, false);
            }
            markPageDirty(newIdx);
            liveCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        }
    }

    rebuildRecentCache();
}
