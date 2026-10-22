#include "SearchEngine.h"
#include "StringUtils.h"
#include "Logger.h"
#include <algorithm>
#include <thread>

// ---------------------------------------------------------------------------
// v6 Flat SoA: loadRecordsV6, completePhase2, readViewForV6
// ---------------------------------------------------------------------------

void SearchEngine::loadRecordsV6(StringPool&& origNamePool,
                                  StringPool&& namePool,
                                  std::vector<uint32_t>&& pathIndices,
                                  StringPool&& pathPool,
                                  StringPool&& lowerPathPool,
                                  std::vector<uint8_t>&& types,
                                  std::vector<uint64_t>&& sizes,
                                  std::vector<int64_t>&& modTimes,
                                  std::vector<uint64_t>&& inodes,
                                  std::vector<int32_t>&& devIds) {
    auto lock = lockForMutation();
    compactionGen_.fetch_add(1, std::memory_order_relaxed);

    uint32_t n = origNamePool.entryCount();

    // Install SoA columns directly (zero-copy from v6 file)
    names_.load(origNamePool, namePool);
    origNamePool = StringPool{};
    namePool = StringPool{};
    pathIndices_ = std::move(pathIndices);
    pathPool_ = DirectoryPool(pathPool);
    pathPool = StringPool{};
    lowerPathPool = StringPool{};
    types_ = std::move(types);
    sizes_.assign(sizes);
    std::vector<uint64_t>{}.swap(sizes);
    modTimes_.assign(modTimes);
    std::vector<int64_t>{}.swap(modTimes);
    inodes_ = std::move(inodes);
    devIds_.assign(devIds);
    std::vector<int32_t>{}.swap(devIds);

    // Rebuild pathLookup_ and lowerPathLookup_ from pathPool_ entries
    pathLookup_.clear();
    lowerPathLookup_.clear();
    pathLookup_.reserve(pathPool_.entryCount());
    lowerPathLookup_.reserve(pathPool_.entryCount());
    for (uint32_t i = 0; i < pathPool_.entryCount(); i++) {
        if (pathPool_.isLive(i)) {
            me::setPathRecord(pathLookup_, pathPool_.view(i), i,
                              [this](uint32_t j) { return pathPool_.view(j); });
            me::setPathRecord(lowerPathLookup_, pathPool_.lower().view(i), i,
                              [this](uint32_t j) { return pathPool_.lower().view(j); });
        }
    }

    pathIndex_.clear();
    pathIndex_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (types_[i] != 0) setPathRecord(lowerFullPath(i), i);
    }

    // Tombstone orphaned duplicates: records not in pathIndex_ as winners
    uint32_t actualLive = 0;
    {
        // Collect winner indices from pathIndex_ values
        std::vector<bool> isWinner(n, false);
        for (const auto& [_, idx] : pathIndex_) {
            isWinner[idx] = true;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (types_[i] == 0) continue;
            if (isWinner[i]) {
                actualLive++;
            } else {
                tombstoneAt(i);
            }
        }
    }

    liveCount_.store(actualLive, std::memory_order_relaxed);

    // Phase 2: defer trigram index building to background
    phase2Pending_.store(true, std::memory_order_release);
    phase2StartRecordCount_ = static_cast<uint32_t>(types_.size());

    // Initialize dirty page bitmap
    uint32_t pageCount = (n + kRecordsPerPage - 1) / kRecordsPerPage;
    dirtyPages_.assign(pageCount, false);
    fullRewriteNeeded_.store(false, std::memory_order_relaxed);

    LOG_INFO("SearchEngine", "loadRecordsV6: loaded " << n << " records (" << actualLive
             << " live), Phase 2 pending");
}

void SearchEngine::completePhase2() {
    if (!phase2Pending_.load(std::memory_order_acquire)) return;

    LOG_INFO("SearchEngine", "Phase 2: building trigram indices in background...");

    // Snapshot data needed for building indices (under shared lock)
    // Only snapshot types_ (lightweight ~5MB for 5.5M records) instead of
    // the full SoA data (~440MB if using FileRecord structs).
    std::vector<uint8_t> snapTypes;
    ModTimeColumn snapModTimes;
    StringPool snapNamePool;
    std::vector<uint32_t> snapPathIndices;
    uint32_t snapPathPoolSize;
    uint32_t snapSize;
    {
        std::shared_lock lock(mutex_);
        snapTypes = types_;
        snapModTimes = modTimes_;
        snapNamePool = names_.lower().flatSnapshot();
        snapPathIndices = pathIndices_;
        snapPathPoolSize = pathPool_.entryCount();
        snapSize = static_cast<uint32_t>(types_.size());
    }

    // Build all indices without holding any lock (~3s)
    auto trigramIndex = buildTrigramIndexFromData(snapTypes, snapNamePool);
    auto extensionIndex = buildExtensionIndexFromData(snapTypes, snapNamePool);
    snapNamePool = StringPool{};
    auto pathIdxToRecords = buildPathIdxToRecordsFromData(snapTypes, snapPathIndices, snapPathPoolSize);
    std::vector<uint32_t>{}.swap(snapPathIndices);
    auto recentCache = buildRecentCacheFromData(snapTypes, snapModTimes, kRecentCacheSize);
    ModTimeColumn{}.swap(snapModTimes);

    LOG_INFO("SearchEngine", "Phase 2: indices built, swapping under lock...");

    // Swap under unique lock and replay mutations that occurred during build
    {
        auto lock = lockForMutation();

        nameTrigramIndex_ = std::move(trigramIndex);
        pathIdxToRecords_ = std::move(pathIdxToRecords);
        recentCache_ = std::move(recentCache);
        extensionIndex_ = std::move(extensionIndex);

        // Replay records added during Phase 2 build
        uint32_t currentSize = static_cast<uint32_t>(types_.size());
        uint32_t replayCount = 0;
        for (uint32_t i = snapSize; i < currentSize; i++) {
            if (types_[i] == 0) continue;
            // Add trigrams for this record
            addTrigramsForRecord(i, names_.lower().data(i), names_.lower().length(i));
            addPathRecord(i);
            addExtensionForRecord(i);
            addToRecentCache(i, static_cast<time_t>(modTimes_[i]));
            replayCount++;
        }

        // Replay tombstones: records that were live in snapshot but deleted during build
        for (uint32_t i = 0; i < snapSize; i++) {
            if (i >= types_.size()) break;
            if (snapTypes[i] != 0 && types_[i] == 0) {
                // Was live in snapshot, now tombstoned — trigram was built, need to remove
                // The trigram entry points at index i which is now tombstoned.
                // Query-time type==0 check will filter it, but we can clean up explicitly.
                removeTrigramsForRecord(i);  // Safe: names_.lower()[i] is tombstoned, this is a no-op
            }
        }

        phase2Pending_.store(false, std::memory_order_release);

        LOG_INFO("SearchEngine", "Phase 2 complete: replayed " << replayCount
                 << " mutations, trigram indices active");
    }
}

SearchEngine::V6ReadView SearchEngine::readViewForV6() const {
    return {std::unique_lock(mutationGate_), std::shared_lock(mutex_), names_.original(), names_.lower(), pathIndices_,
            pathPool_, pathPool_.lower(), types_, sizes_, modTimes_, inodes_, devIds_,
            liveCount_.load(std::memory_order_relaxed)};
}
