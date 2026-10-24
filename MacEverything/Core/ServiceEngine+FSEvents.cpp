#include "ServiceEngine.h"
#include "DirectoryScanner.h"
#include "RescanDebounce.h"
#include "Logger.h"
#include <sys/stat.h>
#include <algorithm>
#include <unordered_set>

// ═══════════════════════════════════════════════════════
//  FSEvents → index mutations
// ═══════════════════════════════════════════════════════

bool ServiceEngine::makeRecordForPath(const std::string& path, const struct stat& st, FileRecord& record) {
    std::string dirPath, fileName;
    size_t lastSlash = path.rfind('/');
    if (lastSlash != std::string::npos) {
        dirPath = path.substr(0, lastSlash);
        fileName = path.substr(lastSlash + 1);
    } else {
        dirPath = ".";
        fileName = path;
    }
    if (fileName.empty()) return false;

    uint8_t type = 4;
    if (S_ISREG(st.st_mode))       type = 1;
    else if (S_ISDIR(st.st_mode))   type = pathEndsWithApp(fileName) ? 5 : 2;
    else if (S_ISLNK(st.st_mode))   type = 3;

    record.name = fileName;
    record.path = dirPath;
    record.type = type;
    record.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
    record.modTime = st.st_mtime;
    record.inode = st.st_ino;
    record.devId = static_cast<int32_t>(st.st_dev);
    return true;
}

// Deleting or moving away a directory usually yields one event for the directory
// alone, with none for what was inside, so it removes the whole subtree. A
// directory that arrives by rename, or is recreated after a removal, brings
// children that have no events of their own, so its contents are rescanned.
bool ServiceEngine::collectFSEventMutations(const std::vector<FileSystemWatcher::Event>& events,
                                            const SearchEngine& engine, bool honorMustScanSubDirs,
                                            FSEventMutations& out) {
    bool changed = false;
    out.ops.reserve(out.ops.size() + events.size());

    for (const auto& event : events) {
        const std::string& path = event.path;
        FSEventStreamEventFlags flags = event.flags;

        if (isInsideAppBundle(path)) continue;

        if (honorMustScanSubDirs && (flags & kFSEventStreamEventFlagMustScanSubDirs)) {
            out.rescanDirs.push_back(path);
            continue;
        }

        bool itemRemoved = (flags & kFSEventStreamEventFlagItemRemoved) != 0;
        bool itemRenamed = (flags & kFSEventStreamEventFlagItemRenamed) != 0;

        struct stat st;
        bool exists = (lstat(path.c_str(), &st) == 0);
        bool isDirNow = exists && S_ISDIR(st.st_mode);

        if (itemRemoved || (itemRenamed && !exists)) {
            if (isDirNow) {
                // Removed and recreated within one coalescing window: resync its contents.
                out.rescanDirs.push_back(path);
            } else {
                // Events without a file/dir flag fall back to what the index recorded.
                bool wasDir = (flags & kFSEventStreamEventFlagItemIsDir) != 0;
                if (!wasDir && !(flags & kFSEventStreamEventFlagItemIsFile)) {
                    uint32_t idx = engine.indexForPath(path);
                    uint8_t type = idx == UINT32_MAX ? 0 : engine.getRecord(idx).type;
                    wasDir = type == 2 || type == 5;
                }
                out.contentUpdates.push_back({path, true});
                out.ops.push_back({wasDir ? SearchEngine::MutationOp::REMOVE_TREE
                                          : SearchEngine::MutationOp::REMOVE, path, {}});
                changed = true;
                continue;
            }
        } else if (!exists) {
            continue;
        }

        FileRecord record;
        if (!makeRecordForPath(path, st, record)) continue;
        const bool isFile = record.type == 1;
        out.ops.push_back({SearchEngine::MutationOp::UPDATE, path, std::move(record)});
        changed = true;

        if (isFile) {
            out.contentUpdates.push_back({path, false});
        }
        if (isDirNow && itemRenamed) {
            out.rescanDirs.push_back(path);
        }
    }

    // A subtree removal covers every removal below it in the same batch; dropping
    // those spares one scan per nested directory when a tree arrives as many events.
    std::unordered_set<std::string> treeRoots;
    for (const auto& op : out.ops) {
        if (op.type == SearchEngine::MutationOp::REMOVE_TREE) treeRoots.insert(op.path);
    }
    if (!treeRoots.empty()) {
        auto underTreeRoot = [&](const std::string& path) {
            for (size_t slash = path.rfind('/'); slash != std::string::npos && slash > 0;
                 slash = path.rfind('/', slash - 1)) {
                if (treeRoots.count(path.substr(0, slash))) return true;
            }
            return false;
        };
        out.ops.erase(std::remove_if(out.ops.begin(), out.ops.end(), [&](const SearchEngine::MutationOp& op) {
            return op.type != SearchEngine::MutationOp::UPDATE && underTreeRoot(op.path);
        }), out.ops.end());
    }
    return changed;
}

// ═══════════════════════════════════════════════════════
//  FSEvents application (replay path)
// ═══════════════════════════════════════════════════════

void ServiceEngine::applyFSEvents(
    const std::vector<FileSystemWatcher::Event>& events,
    std::shared_ptr<SearchEngine> engine)
{
    FSEventMutations mutations;
    collectFSEventMutations(events, *engine, /*honorMustScanSubDirs=*/false, mutations);

    // Apply content index updates (uses its own lock)
    for (const auto& [path, isRemove] : mutations.contentUpdates) {
        updateContentForPath(path, isRemove, engine);
    }

    // Apply all search engine mutations in a single lock acquisition
    engine->batchMutate(std::move(mutations.ops));

    if (!mutations.rescanDirs.empty()) {
        scheduleRescanForPaths(mutations.rescanDirs);
    }
}

// ═══════════════════════════════════════════════════════
//  Live monitoring
// ═══════════════════════════════════════════════════════

void ServiceEngine::startMonitoring() {
    if (isMonitoring_.load(std::memory_order_relaxed)) return;

    std::string root = config_.scanRoot;

    // Exclude app's own cache directory from FSEvents
    std::string cacheExclusion = config_.cachePath;
    if (cacheExclusion.empty()) {
        const char* home = std::getenv("HOME");
        if (home) cacheExclusion = std::string(home) + "/Library/Caches/com.maceverything.app";
    }
    if (!cacheExclusion.empty()) {
        watcher_->setExclusionPaths({cacheExclusion});
    }

    watcher_->start(root, [this](std::vector<FileSystemWatcher::Event> events) {
        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        auto engine = safeEngine();
        if (!engine) return;

        FSEventMutations mutations;
        bool changed = collectFSEventMutations(events, *engine, /*honorMustScanSubDirs=*/true, mutations);

        // Apply content index updates (uses its own lock)
        for (const auto& [path, isRemove] : mutations.contentUpdates) {
            updateContentForPath(path, isRemove, engine);
        }

        // Apply all search engine mutations in a single lock acquisition
        engine->batchMutate(std::move(mutations.ops));

        if (!mutations.rescanDirs.empty()) {
            scheduleRescanForPaths(mutations.rescanDirs);
        }

        if (changed && onIndexChanged) {
            onIndexChanged();
        }
    });

    isMonitoring_.store(true, std::memory_order_relaxed);
}

void ServiceEngine::stopMonitoring() {
    watcher_->stop();

    {
        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        if (rescanDebounceTimer_) {
            dispatch_source_cancel(rescanDebounceTimer_);
            rescanDebounceTimer_ = nullptr;
        }
        pendingRescanPaths_.clear();
        lastRescanTime_.clear();
    }

    auto persistence = safePersistence();
    if (persistence) {
        persistence->stopAutoCompactionAndWait();
    }
    auto cp = safeContentPersistence();
    if (cp) {
        cp->stopAutoCompactionAndWait();
    }
    isMonitoring_.store(false, std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════
//  Rescan debounce
// ═══════════════════════════════════════════════════════

void ServiceEngine::scheduleRescanForPaths(const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lock(pendingRescanMutex_);

    pendingRescanPaths_ = mergeRescanPaths(pendingRescanPaths_, paths);

    LOG_INFO("FSWatcher", "Debounce: " << pendingRescanPaths_.size()
             << " pending rescan path(s), scheduling " << kRescanDebounceDelaySec << "s delay");

    if (rescanDebounceTimer_) {
        dispatch_source_cancel(rescanDebounceTimer_);
        rescanDebounceTimer_ = nullptr;
    }

    rescanDebounceTimer_ = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, mutationQueue_);

    uint64_t delaySec = static_cast<uint64_t>(kRescanDebounceDelaySec * NSEC_PER_SEC);
    dispatch_source_set_timer(rescanDebounceTimer_, dispatch_time(DISPATCH_TIME_NOW, delaySec),
                              DISPATCH_TIME_FOREVER, NSEC_PER_SEC / 10);

    dispatch_source_set_event_handler(rescanDebounceTimer_, ^{
        this->flushPendingRescans();
    });
    dispatch_resume(rescanDebounceTimer_);
}

void ServiceEngine::flushPendingRescans() {
    std::set<std::string> pathsToRescan;
    std::set<std::string> throttledPaths;
    {
        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        for (const auto& path : pendingRescanPaths_) {
            if (shouldThrottleRescan(path, lastRescanTime_, kRescanThrottleIntervalSec)) {
                throttledPaths.insert(path);
                LOG_INFO("FSWatcher", "Throttled rescan for: " << path
                         << " (within " << kRescanThrottleIntervalSec << "s window)");
            } else {
                pathsToRescan.insert(path);
            }
        }
        pendingRescanPaths_ = throttledPaths;

        if (rescanDebounceTimer_) {
            dispatch_source_cancel(rescanDebounceTimer_);
            rescanDebounceTimer_ = nullptr;
        }
    }

    if (pathsToRescan.empty() && throttledPaths.empty()) return;

    LOG_INFO("FSWatcher", "Flushing debounced rescan: " << pathsToRescan.size()
             << " path(s) to rescan, " << throttledPaths.size() << " throttled");

    for (const auto& path : pathsToRescan) {
        rescanSubtree(path);

        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        lastRescanTime_[path] = std::chrono::steady_clock::now();
    }

    // Clean up old entries in lastRescanTime_ (> 2x throttle interval)
    {
        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = lastRescanTime_.begin(); it != lastRescanTime_.end(); ) {
            auto elapsed = std::chrono::duration<double>(now - it->second).count();
            if (elapsed > kRescanThrottleIntervalSec * 2.0) {
                it = lastRescanTime_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // If there are throttled paths, schedule a retry when throttle expires
    if (!throttledPaths.empty()) {
        std::lock_guard<std::mutex> lock(pendingRescanMutex_);
        if (!rescanDebounceTimer_) {
            rescanDebounceTimer_ = dispatch_source_create(
                DISPATCH_SOURCE_TYPE_TIMER, 0, 0, mutationQueue_);
            uint64_t delay = static_cast<uint64_t>(kRescanThrottleIntervalSec * NSEC_PER_SEC);
            dispatch_source_set_timer(rescanDebounceTimer_,
                                      dispatch_time(DISPATCH_TIME_NOW, delay),
                                      DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
            dispatch_source_set_event_handler(rescanDebounceTimer_, ^{
                this->flushPendingRescans();
            });
            dispatch_resume(rescanDebounceTimer_);
        }
    }
}

void ServiceEngine::rescanDirectoryInto(SearchEngine& engine, const std::string& dir) {
    // An empty prefix matches every record: replacing it would empty the index.
    if (dir.empty()) {
        LOG_WARN("FSWatcher", "Ignoring rescan of an empty path");
        return;
    }
    DirectoryScanner scanner;
    scanner.scan(dir);
    auto freshRecords = scanner.takeResults();
    // The scanner lists what is inside dir, while the prefix replacement below
    // also drops dir's own record; keep it while the directory exists.
    struct stat st;
    FileRecord self;
    if (lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && makeRecordForPath(dir, st, self)) {
        freshRecords.push_back(std::move(self));
    }
    engine.batchRescanPrefix(dir, std::move(freshRecords));
}

void ServiceEngine::rescanSubtree(const std::string& dirRef) {
    auto engine = safeEngine();
    if (!engine) return;
    // A block captures a reference parameter as a reference, and callers pass
    // strings that are gone before the queued block runs; capture a copy.
    const std::string dir = dirRef;

    dispatch_async(mutationQueue_, ^{
        if (shuttingDown_.load(std::memory_order_relaxed)) return;

        rescanDirectoryInto(*engine, dir);

        // Compact if tombstones exceed 30%
        uint32_t total = engine->recordCount();
        uint32_t live  = engine->liveRecordCount();
        if (total > live && (total - live) > total * 3 / 10) {
            auto remap = engine->compactRecords();
            if (!remap.empty()) {
                auto ci = safeContentIndex();
                if (ci) {
                    ci->remapFileIndices(remap);
                }
            }
        }

        if (onIndexChanged) {
            onIndexChanged();
        }
    });
}
