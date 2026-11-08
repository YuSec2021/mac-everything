#pragma once
#include "FileRecord.h"
#include "ContentIndex.h" // for Trigram type and extractTrigrams/makeTrigram
#include "StringPool.h"
#include "DirectoryPool.h"
#include "FoldedNamePool.h"
#include "CompactPathIndex.h"
#include "PackedPostingList.h"
#include "NarrowColumn.h"
#include "DirRecordMap.h"
#include "PathRecordTable.h"
#include "SIMDSearch.h"
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <cstdio>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include <memory>
#include <optional>
#include <map>
#include <set>
#include <mutex>
#include <utility>

#include "IndexWAL.h"
#include "QueryAST.h"
#include "StructuredQueryParser.h"

/// Deduplication table for directory path strings.
/// Interns unique paths and returns a compact uint32_t index.
class PathTable {
public:
    /// Look up or insert a path, returning its index.
    uint32_t intern(const std::string& path) {
        auto it = lookup_.find(path);
        if (it != lookup_.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(paths_.size());
        paths_.push_back(path);
        lookup_[path] = idx;
        return idx;
    }

    /// Resolve an index back to its path string.
    /// Returns empty string if index is out of range.
    const std::string& resolve(uint32_t index) const {
        static const std::string empty;
        if (index >= paths_.size()) return empty;
        return paths_[index];
    }

    /// Number of unique paths stored.
    uint32_t size() const { return static_cast<uint32_t>(paths_.size()); }

    /// Clear all entries.
    void clear() { paths_.clear(); lookup_.clear(); }

    PathTable() = default;
    PathTable(PathTable&&) = default;
    PathTable& operator=(PathTable&&) = default;
    PathTable(const PathTable&) = default;
    PathTable& operator=(const PathTable&) = default;

private:
    std::vector<std::string> paths_;                    // index -> path
    std::unordered_map<std::string, uint32_t> lookup_;  // path -> index
};

/// Metadata stored in the index header (v3+). Extensible key-value pairs.
struct IndexMetadata {
    uint32_t formatVersion = 0; // populated on load
    int64_t  timestamp = 0;     // creation timestamp (seconds since epoch)
    uint64_t lastEventId = 0;   // FSEvents stream ID for incremental replay
    std::map<std::string, std::string> extra; // extensible key-value pairs

    // Well-known metadata keys
    static constexpr const char* kScanRoot     = "scan_root";      // e.g. "/"
    static constexpr const char* kAppVersion   = "app_version";    // e.g. "1.0.0"
    static constexpr const char* kOSVersion    = "os_version";     // e.g. "14.5"
    static constexpr const char* kRecordFormat = "record_format";  // e.g. "v2_inode"
};

/// Detailed timing breakdown from a search query.
struct QueryTimingInfo {
    double totalMs = 0;
    double lockWaitMs = 0;
    double trigramMs = 0;     // candidate filtering (name trigrams or directory scan)
    double phase1Ms = 0;      // name verification (trigram path only)
    double phase2Ms = 0;      // path-only match / NEON full scan
    double sortMs = 0;
    double lockHeldMs = 0;
    size_t totalRecords = 0;
    size_t candidates = 0;    // trigram candidates count
    size_t nameMatches = 0;   // phase1 results (name matches)
    size_t pathMatches = 0;   // phase2 results (path-only matches)
    size_t resultCount = 0;
    bool usedTrigram = false;
    std::string searchPath;   // "trigram", "trigram-split", or "linear"
};

/// Bundled cancellation context passed through query call chain.
struct QueryCancelCtx {
    const std::atomic<uint64_t>* gen;
    uint64_t myGen;
    bool cancelled() const {
        return gen->load(std::memory_order_relaxed) != myGen;
    }
};

class SearchEngine {
public:
    // Narrow per-record columns; out-of-range values keep exact side entries.
    using SizeColumn = me::NarrowColumn<uint64_t, uint32_t>;
    using ModTimeColumn = me::NarrowColumn<int64_t, uint32_t>;

    /// Takes ownership of scanned records and pre-computes lowercase names.
    void loadRecords(std::vector<FileRecord>&& records);

    /// v5 fast-path load: accepts pre-separated data from on-disk path dictionary.
    /// Skips toLower of names and path dedup since they are pre-computed.
    void loadRecordsV5(std::vector<FileRecord>&& records,
                       std::vector<std::string>&& lowerNames,
                       std::vector<uint32_t>&& pathIndices,
                       StringPool&& pathDict,
                       StringPool&& lowerPathDict);

    /// v6 fast-path load: accepts pre-separated SoA columns and StringPools.
    /// Only builds pathIndex_ (required for mutations). Trigram indices deferred to Phase 2.
    void loadRecordsV6(StringPool&& origNamePool,
                       StringPool&& namePool,
                       std::vector<uint32_t>&& pathIndices,
                       StringPool&& pathPool,
                       StringPool&& lowerPathPool,
                       std::vector<uint8_t>&& types,
                       std::vector<uint64_t>&& sizes,
                       std::vector<int64_t>&& modTimes);

    /// Complete Phase 2 of two-stage startup: build trigram indices in background,
    /// then swap under lock and replay mutations that occurred during build.
    void completePhase2();

    /// Whether Phase 2 is pending (trigram indices not yet built).
    bool isPhase2Pending() const { return phase2Pending_.load(std::memory_order_acquire); }

    /// Borrow immutable columns under a shared lock while serializing v6.
    struct V6ReadView {
        std::unique_lock<std::mutex> mutationGate;
        std::shared_lock<std::shared_mutex> lock;
        FoldedNamePool::View origNamePool;
        FoldedNamePool::View namePool;
        const std::vector<uint32_t>& pathIndices;
        const DirectoryPool& pathPool;
        DirectoryPool::LowerView lowerPathPool;
        const std::vector<uint8_t>& types;
        const SizeColumn& sizes;
        const ModTimeColumn& modTimes;
        uint32_t liveCount;
    };
    V6ReadView readViewForV6() const;

    /// Case-insensitive substring search. Returns indices into the records array.
    /// If maxResults > 0, stops early once enough matches are found.
    /// If useTrigram is false, bypasses trigram index and does NEON full scan.
    std::vector<uint32_t> query(const std::string& keyword, uint32_t maxResults = 0,
                                bool useTrigram = true, uint64_t sessionId = 0) const;

    /// Same as query() but also populates timing breakdown.
    std::vector<uint32_t> query(const std::string& keyword, uint32_t maxResults,
                                bool useTrigram, QueryTimingInfo& timing,
                                uint64_t sessionId = 0) const;

    /// Advanced query: evaluate an AST with boolean operators, quoted phrases,
    /// and filter nodes. Uses trigram index for TERM nodes, then applies
    /// AND/OR/NOT logic. Called automatically by query() when advanced syntax
    /// is detected.
    std::vector<uint32_t> queryAdvanced(const std::string& input, uint32_t maxResults,
                                         bool useTrigram, QueryTimingInfo& timing,
                                         uint64_t myGen = 0,
                                         const std::atomic<uint64_t>* genPtr = nullptr) const;

    /// Cancel in-flight queries for the given session. Lock-free on the generation atomic.
    void cancelSession(uint64_t sessionId) const;

    /// Get or create the generation atomic for a session. Returns {atomicPtr, currentGenValue}.
    std::pair<std::shared_ptr<std::atomic<uint64_t>>, uint64_t>
    acquireSessionGeneration(uint64_t sessionId) const;

    FileRecord getRecord(uint32_t index) const;
    uint32_t recordCount() const;

    // --- Incremental mutation (for FSEvents) ---

    /// Batch mutation operation descriptor.
    struct MutationOp {
        /// REMOVE_TREE removes the path and everything below it (a deleted directory).
        enum Type { REMOVE, UPDATE, REMOVE_TREE };
        Type type;
        std::string path;
        FileRecord record;  // only used for UPDATE
    };

    /// Append a new record. Returns the new index. Thread-safe.
    uint32_t addRecord(FileRecord&& record);

    /// Mark record at fullPath as deleted (tombstone). Thread-safe.
    /// Returns true if found and removed.
    bool removeByPath(const std::string& fullPath);

    /// Remove old record at fullPath (if any) and add updated record. Thread-safe.
    void updateByPath(const std::string& fullPath, FileRecord&& updated);

    /// Apply multiple mutations under a single write lock. Thread-safe.
    /// Reduces lock contention compared to calling removeByPath/updateByPath individually.
    void batchMutate(std::vector<MutationOp>&& ops);

    /// Remove all records whose full path starts with the given prefix. Thread-safe.
    /// Returns the number of records removed.
    uint32_t removeByPathPrefix(const std::string& pathPrefix);

    /// Batch-replace all records under pathPrefix with freshRecords.
    /// Tombstones old records, adds fresh records, and rebuilds trigram index once.
    /// Returns the number of old records removed. Thread-safe.
    uint32_t batchRescanPrefix(const std::string& pathPrefix,
                               std::vector<FileRecord>&& freshRecords);

    /// Replay WAL entries in-place using pathIndex_ for O(1) lookups.
    /// Skips WAL writes (entries came FROM the WAL). Thread-safe.
    void replayWALEntries(std::vector<WALEntry>&& entries);

    /// Number of live (non-tombstoned) records.
    uint32_t liveRecordCount() const { return liveCount_.load(std::memory_order_relaxed); }

    /// Compact in-memory records by removing tombstoned entries.
    /// Rebuilds pathIndex_ and names_.lower(). Thread-safe.
    /// Returns a mapping from old index → new index for live records.
    /// Returns empty map if nothing was compacted, including when another
    /// compaction is already running.
    std::unordered_map<uint32_t, uint32_t> compactRecords();

    /// Save live records to a binary file (format v3). Thread-safe.
    /// metadata.extra can contain arbitrary key-value pairs for forward compatibility.
    bool saveToFile(const std::string& filePath, const IndexMetadata& metadata) const;

    /// Legacy overload for backward compatibility.
    bool saveToFile(const std::string& filePath, uint64_t lastEventId = 0) const;

    /// Load records from a binary file (v1, v2, or v3), rebuilding indices. Thread-safe.
    /// outMetadata receives all metadata including formatVersion, timestamp, lastEventId, and extra pairs.
    bool loadFromFile(const std::string& filePath, IndexMetadata* outMetadata = nullptr);

    /// Legacy overload for backward compatibility.
    bool loadFromFile(const std::string& filePath, uint64_t* outLastEventId);

    /// Attach a WAL for logging mutations. Must be already opened.
    void attachWAL(std::shared_ptr<IndexWAL> wal);

    /// Detach the WAL (e.g. before compaction).
    void detachWAL();

    /// Records per page for paged persistence
    static constexpr uint32_t kRecordsPerPage = 1024;

    /// Get the list of dirty page numbers (pages modified since last clearDirtyPages). Thread-safe.
    std::vector<uint32_t> getDirtyPageNumbers() const;

    /// Clear the dirty page bitmap. Thread-safe.
    void clearDirtyPages();

    /// Whether a full rewrite is needed (set after compactRecords renumbers indices).
    bool needsFullRewrite() const { return fullRewriteNeeded_.load(std::memory_order_relaxed); }
    void clearFullRewriteNeeded() { fullRewriteNeeded_.store(false, std::memory_order_relaxed); }

    /// Monotonically increasing generation counter, incremented on each compaction.
    uint64_t compactionGeneration() const { return compactionGen_.load(std::memory_order_relaxed); }

    /// Return indices of the most recently modified live records, sorted by modTime descending.
    /// Accesses recentCache_ directly under the lock to avoid per-record copy overhead.
    std::vector<uint32_t> recentIndices(uint32_t count) const;

    /// Testing aid: every trigram ("t" + code) and extension ("e" + ext)
    /// posting list, sorted by key.
    std::vector<std::pair<std::string, std::vector<uint32_t>>> postingListsForTesting() const;
    /// Below this many records, prefix removal updates the name indexes per record.
    static constexpr size_t kBatchIndexRemoval = 64;

    /// Look up the record index for a given full path. Returns UINT32_MAX if not found.
    uint32_t indexForPath(const std::string& fullPath) const;

    /// Export a copy of all live records with their paths restored. Thread-safe.
    std::vector<FileRecord> exportRecords() const;

    /// Build the full path from a record's path and name components.
    static std::string makeFullPath(std::string_view path, std::string_view name);

    /// Resolve a record's path via pathPool_. Thread-safe (acquires shared_lock).
    /// Returns by value to prevent dangling references during concurrent compaction.
    std::string resolveRecordPath(uint32_t index) const {
        std::shared_lock lock(mutex_);
        if (index >= pathIndices_.size()) return {};
        return pathPool_.str(pathIndices_[index]);
    }

    /// Batch callback access under a single shared_lock. Reconstructs FileRecord from SoA.
    /// Callback signature: void(uint32_t idx, const FileRecord& record, const std::string& path)
    template<typename Func>
    bool forEachRecordWithPath(const std::vector<uint32_t>& indices, Func&& func,
                               std::optional<uint64_t> expectedGeneration = std::nullopt) const {
        std::shared_lock lock(mutex_);
        if (expectedGeneration && *expectedGeneration != compactionGen_.load(std::memory_order_relaxed)) return false;
        for (uint32_t idx : indices) {
            if (idx >= types_.size() || types_[idx] == 0) continue;
            std::string path = pathPool_.str(pathIndices_[idx]);
            FileRecord rec;
            rec.name = names_.original().str(idx);
            rec.path = path;
            rec.type = types_[idx];
            rec.size = sizes_[idx];
            rec.modTime = static_cast<time_t>(modTimes_[idx]);
            func(idx, rec, path);
        }
        return true;
    }

    /// Batch callback for a contiguous range of records, including tombstones.
    /// Used by PagedIndexWriter to serialize pages with stable index positions.
    /// Callback signature: void(uint32_t idx, const FileRecord& record, const std::string& path)
    template<typename Func>
    void forEachRecordInRange(uint32_t startIdx, uint32_t count, Func&& func) const {
        std::shared_lock lock(mutex_);
        uint32_t end = std::min(startIdx + count, static_cast<uint32_t>(types_.size()));
        for (uint32_t i = startIdx; i < end; i++) {
            std::string path = pathPool_.str(pathIndices_[i]);
            FileRecord rec;
            rec.name = names_.original().str(i);
            rec.path = path;
            rec.type = types_[i];
            rec.size = sizes_[i];
            rec.modTime = static_cast<time_t>(modTimes_[i]);
            func(i, rec, path);
        }
    }

    /// Batch callback for v5 serialization. Provides pathIdx and lower name data directly.
    /// Callback signature: void(uint32_t idx, const FileRecord& rec, uint32_t pathIdx,
    ///                          const char* lowerName, uint16_t lowerNameLen)
    template<typename Func>
    void forEachRecordInRangeV5(uint32_t startIdx, uint32_t count, Func&& func) const {
        std::shared_lock lock(mutex_);
        uint32_t end = std::min(startIdx + count, static_cast<uint32_t>(types_.size()));
        for (uint32_t i = startIdx; i < end; i++) {
            FileRecord rec;
            rec.name = names_.original().str(i);
            rec.type = types_[i];
            rec.size = sizes_[i];
            rec.modTime = static_cast<time_t>(modTimes_[i]);
            func(i, rec, pathIndices_[i],
                 names_.lower().data(i), names_.lower().length(i));
        }
    }

    /// Thread-safe copy of pathPool_ for serialization.
    StringPool pathPoolSnapshot() const {
        std::shared_lock lock(mutex_);
        return pathPool_.flatSnapshot();
    }

    /// Thread-safe copy of pathPool_.lower() for serialization.
    StringPool lowerPathPoolSnapshot() const {
        std::shared_lock lock(mutex_);
        return pathPool_.lower().flatSnapshot();
    }

    /// Thread-safe snapshot of PathTable. Reconstructs from pathPool_ for compatibility.
    PathTable pathTableSnapshot() const {
        std::shared_lock lock(mutex_);
        PathTable pt;
        for (uint32_t i = 0; i < pathPool_.entryCount(); i++) {
            if (pathPool_.isLive(i)) {
                pt.intern(pathPool_.str(i));
            }
        }
        return pt;
    }

private:
    FoldedNamePool names_;                 // original/lower views share unchanged bytes
    std::vector<uint32_t> pathIndices_;    // per-record index into pathPool_
    DirectoryPool pathPool_;               // parent-linked directory components

    // SoA columnar arrays — the canonical per-record storage
    std::vector<uint8_t>  types_;          // 1=file, 2=directory, 0=tombstone
    SizeColumn            sizes_;          // file size in bytes
    ModTimeColumn         modTimes_;       // modification time (Unix epoch)
    me::CompactPathIndex pathLookup_; // original path hash -> pathPool_ index
    me::CompactPathIndex lowerPathLookup_; // lowercase path hash -> pathPool_ index
    // Live record per lowercase full path. The key is (canonical parent
    // directory, lowercase name): directories whose lowercase paths join to the
    // same full path share a canonical ID, so keys match full-path equality.
    me::PathRecordTable pathIndex_;
    std::vector<uint32_t> dirCanon_; // pathPool_ index -> canonical directory
    static constexpr uint32_t kNoRecord = me::PathRecordTable::kNone;
    std::string lowerFullPath(uint32_t index) const;

    /// Key accessors over one generation of record data (live, snapshot or compacted).
    struct PathKeys {
        FoldedNamePool::View names;
        const std::vector<uint32_t>& pathIndices;
        const std::vector<uint32_t>& canon;
        uint64_t hashOf(uint32_t r) const { return pathKeyHash(canon[pathIndices[r]], names.view(r)); }
        bool matches(uint32_t r, uint32_t dir, std::string_view name) const {
            return canon[pathIndices[r]] == dir && names.view(r) == name;
        }
    };
    PathKeys pathKeys() const { return {names_.lower(), pathIndices_, dirCanon_}; }
    static uint64_t pathKeyHash(uint32_t canonDir, std::string_view lowerName);
    /// Canonical ID for a new directory whose lowercase path is lowerDir; call
    /// before adding it to lowerLookup.
    static uint32_t canonicalDir(const me::CompactPathIndex& lowerLookup, DirectoryPool::LowerView lower,
                                 const std::vector<uint32_t>& canon, const std::string& lowerDir, uint32_t self);
    /// Record whose lowercase full path equals the argument, or kNoRecord.
    uint32_t findPathRecord(std::string_view lowerFull) const;
    /// Map the record's own path to it, replacing any record with that path.
    void setPathRecord(uint32_t index);
    /// Unmap a record. Call before tombstoneAt, which clears its name.
    void erasePathRecord(uint32_t index);
    /// Rebuild pathIndex_ from live records (later duplicates win) and tombstone losers.
    uint32_t rebuildPathIndex();
    /// Rebuild pathLookup_, lowerPathLookup_ and dirCanon_ from pathPool_.
    void rebuildDirLookups();
    /// Mapped records at or below a lowercase path prefix, ascending. With
    /// mirrorsFilesystem (the index follows the filesystem, so every record has
    /// directory records up to the prefix), the subtree is walked instead of
    /// scanning every directory.
    std::vector<uint32_t> pathRecordsUnder(const std::string& lowerPrefix, bool mirrorsFilesystem = false) const;
    std::atomic<uint32_t> liveCount_{0};
    mutable std::shared_mutex mutex_;
    mutable std::mutex mutationGate_;
    struct MutationLock {
        std::unique_lock<std::mutex> gate;
        std::unique_lock<std::shared_mutex> data;
    };
    MutationLock lockForMutation() {
        return {std::unique_lock(mutationGate_), std::unique_lock(mutex_)};
    }

    // Trigram inverted index for fast filename search
    std::unordered_map<Trigram, me::PackedPostingList> nameTrigramIndex_; // trigram -> record indices

    // Directory-to-record mapping; path queries scan the deduplicated directory pool.
    me::DirRecordMap pathIdxToRecords_; // pathIdx -> runs of record indices (tombstones skipped)

    // Extension index: extension string -> sorted record indices (files only)
    std::unordered_map<std::string, std::vector<uint32_t>> extensionIndex_;

    static std::unordered_map<Trigram, me::PackedPostingList>
        buildTrigramIndexFromData(const std::vector<uint8_t>& types, FoldedNamePool::View pool);
    static std::unordered_map<std::string, std::vector<uint32_t>>
        buildExtensionIndexFromData(const std::vector<uint8_t>& types, FoldedNamePool::View pool);

    /// Intern a directory path into pathPool_ and pathPool_.lower(). Returns pathPool_ index.
    /// Deduplicates via pathLookup_. Must be called under unique_lock.
    uint32_t internPath(const std::string& path);

    /// Tombstone a record at idx: clear SoA columns. Must be called under unique_lock.
    void tombstoneAt(uint32_t idx);

    /// Append a record to SoA columns. Must be called under unique_lock.
    void pushRecord(FileRecord&& rec);

    /// Unlocked variants — caller must hold unique_lock on mutex_.
    bool removeByPathUnlocked(const std::string& fullPath);
    void updateByPathUnlocked(const std::string& fullPath, FileRecord&& updated);
    uint32_t removeByPathPrefixUnlocked(const std::string& pathPrefix, bool mirrorsFilesystem = false);

    /// Build trigram index from names_.lower() (called inside loadRecords/compactRecords under lock)
    void buildTrigramIndex();
    /// Add trigrams for a single record to the index
    void addTrigramsForRecord(uint32_t idx, const char* data, uint16_t len);
    /// Remove trigrams for a single record from the index
    void removeTrigramsForRecord(uint32_t idx);

    /// Build extension index from names_.lower() (maps extension -> sorted record indices)
    void buildExtensionIndex();
    /// Add a record to the extension index (sorted insert)
    void addExtensionForRecord(uint32_t idx);
    /// Remove a record from the extension index
    void removeExtensionForRecord(uint32_t idx);
    /// Remove ascending records from the trigram and extension indexes, one
    /// rewrite per posting list. Same result as the per-record removals.
    void removeIndexEntriesForRecords(const std::vector<uint32_t>& sortedIdx);

    /// Rebuild pathIdx -> record indices mapping
    void rebuildPathIdxToRecords();
    /// Add a newly appended record to pathIdxToRecords_
    void addPathRecord(uint32_t idx);
    /// Removal is lazy: pathIdxToRecords_ skips tombstoned records
    void removePathRecord(uint32_t) {}

    static bool globMatch(const std::string& pattern, const std::string& text);

    /// Match result from search: (record index, priority, full path length).
    /// Priority: 0=exact, 1=starts-with, 2=contains, 3=path-only match.
    struct Match { uint32_t idx; uint8_t priority; uint32_t pathLen; };

    /// Node-centric structured query: name trigram → name verify → path constraint verify.
    /// Handles SEGMENTS and DIR_EXACT modes.
    void queryStructured(const ParsedQuery& pq,
                         size_t totalSize, const QueryCancelCtx& cancel,
                         std::vector<Match>& merged) const;

    /// DIR_LIST mode: find matching directories, return their direct children.
    void queryDirList(const ParsedQuery& pq,
                      size_t totalSize, const QueryCancelCtx& cancel,
                      std::vector<Match>& merged) const;

public:
    /// Check if a directory path matches the path segment constraints.
    static bool pathSegmentsMatch(std::string_view dirPath,
                                  const std::vector<PathSegment>& segments);
private:

    /// Estimate trigram candidate count for a keyword (min posting list size).
    /// Returns SIZE_MAX if keyword is < 3 chars, 0 if any trigram not in index.
    size_t estimateTrigramCost(const std::string& keyword) const;

    /// Tree-walk from anchor directory down to target segment, collecting matches.
    void treeWalkDown(uint32_t dirIdx, const ParsedQuery& pq,
                      int fromSegIdx, int toSegIdx,
                      size_t totalSize, const QueryCancelCtx& cancel,
                      std::vector<Match>& merged) const;

    /// Anchor on namePattern: trigram intersect + path verify. Returns false to fall back.
    bool queryStructuredNameAnchor(const ParsedQuery& pq,
                                   size_t totalSize, const QueryCancelCtx& cancel,
                                   std::vector<Match>& merged) const;

    /// Anchor on path segment: trigram intersect + tree-walk. Returns false to fall back.
    bool queryStructuredPathAnchor(const ParsedQuery& pq,
                                   size_t anchorIdx,
                                   size_t totalSize, const QueryCancelCtx& cancel,
                                   std::vector<Match>& merged) const;

    std::vector<bool> dirtyPages_;                // dirty page bitmap
    std::atomic<bool> fullRewriteNeeded_{false};   // set by compactRecords()

    /// Mark the page containing the given record index as dirty. Must be called under lock.
    void markPageDirty(uint32_t recordIndex);

    std::shared_ptr<IndexWAL> wal_;
    std::atomic<uint64_t> compactionGen_{0};
    std::mutex compactionMutex_;  // held for a whole compactRecords() run

    // Phase 2 two-stage startup: trigram indices built in background
    std::atomic<bool> phase2Pending_{false};
    uint32_t phase2StartRecordCount_{0};

    // Per-session query generation for cancellation.
    // Key: sessionId (0 = no cancellation). Value: shared atomic counter.
    mutable std::unordered_map<uint64_t, std::shared_ptr<std::atomic<uint64_t>>> sessionGenerations_;
    mutable std::mutex sessionGenMutex_;  // protects sessionGenerations_ map only

    // Recent files cache: top-K records by modTime, maintained incrementally
    static constexpr uint32_t kRecentCacheSize = 200;
    struct RecentEntry {
        time_t modTime;
        uint32_t index;
        bool operator<(const RecentEntry& o) const {
            return modTime > o.modTime || (modTime == o.modTime && index > o.index);
        }
    };
    std::set<RecentEntry> recentCache_;
    void rebuildRecentCache();
    void addToRecentCache(uint32_t idx, time_t modTime);
    void removeFromRecentCache(uint32_t idx, time_t modTime);

    /// Compute match priority: 0=exact, 1=starts-with, 2=contains.
    static uint8_t namePriority(const char* nameData, uint16_t nameLen,
                                const char* keyData, size_t keyLen);

    /// Intersect posting lists from a trigram index for a given keyword.
    /// Returns sorted candidate indices, or empty if any trigram is missing.
    /// Sets allFound to false if any trigram is not in the index.
    static std::vector<uint32_t> intersectPostingLists(
        const std::unordered_map<Trigram, me::PackedPostingList>& index,
        const std::string& keyword,
        bool& allFound);

    /// Intersect trigrams from multiple literal segments for complex glob pre-filtering.
    static std::vector<uint32_t> intersectPostingListsMulti(
        const std::unordered_map<Trigram, me::PackedPostingList>& index,
        const std::vector<std::string>& segments,
        bool& allFound);

    /// Union posting lists across multiple atoms for regex pre-filtering.
    /// Each atom's trigrams are intersected internally, then results are unioned.
    /// Safe for both AND and OR semantics from FilteredRE2.
    static std::vector<uint32_t> unionPostingListsMulti(
        const std::unordered_map<Trigram, me::PackedPostingList>& index,
        const std::vector<std::string>& segments);

    /// Build full path in a reusable buffer: path + '/' + name.
    /// Lowercases the path portion via SIMD. Returns the full path length.
    /// Buffer is resized if needed (with 2x growth).
    static size_t buildFullPathBuf(std::vector<char>& buf,
                                   const char* pathData, uint16_t pathLen,
                                   const char* nameData, uint16_t nameLen);

    /// Build trigram index from standalone data (no member access, used by COW compaction)
    static std::unordered_map<Trigram, me::PackedPostingList>
        buildTrigramIndexFromData(const std::vector<uint8_t>& types,
                                  const StringPool& namePool);

    /// Build extension index from standalone data (no member access, used by COW compaction)
    static std::unordered_map<std::string, std::vector<uint32_t>>
        buildExtensionIndexFromData(const std::vector<uint8_t>& types,
                                    const StringPool& namePool);


    /// Build pathIdx -> record indices mapping from standalone data (used by COW compaction)
    static me::DirRecordMap
        buildPathIdxToRecordsFromData(const std::vector<uint8_t>& types,
                                      const std::vector<uint32_t>& pathIndices,
                                      uint32_t pathPoolSize);

    /// Build recent cache from standalone data (no member access, used by COW compaction)
    static std::set<RecentEntry>
        buildRecentCacheFromData(const std::vector<uint8_t>& types,
                                 const ModTimeColumn& modTimes,
                                 uint32_t cacheSize);
};
