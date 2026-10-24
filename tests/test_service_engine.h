#pragma once
// Part 40: ServiceEngine unit tests
// Verifies ServiceEngine construction, full scan on a tmpdir, query, and shutdown.

// ServiceEngine.h is included via test_all.cpp
#include <fstream>

static bool runPart40() {
    std::cout << "\n=== Part 40: ServiceEngine unit tests ===\n";
    bool allOk = true;

    // Create a tmpdir with some test files
    auto tmpBase = fs::temp_directory_path() / "maceverything_test_se";
    fs::remove_all(tmpBase);
    fs::create_directories(tmpBase / "subdir");

    // Create test files
    { std::ofstream f(tmpBase / "hello.txt"); f << "hello world"; }
    { std::ofstream f(tmpBase / "test.cpp"); f << "int main() {}"; }
    { std::ofstream f(tmpBase / "subdir" / "nested.log"); f << "log entry"; }

    std::string cachePath = (tmpBase / "cache").string();
    fs::create_directories(cachePath);

    // Test 1: Construction
    {
        ServiceConfig config;
        config.scanRoot = tmpBase.string();
        config.cachePath = cachePath;
        config.logPath = (tmpBase / "logs").string();

        auto engine = std::make_unique<ServiceEngine>(config);
        check(engine != nullptr, "ServiceEngine construction succeeds");

        bool completed = false;
        engine->onContentIndexComplete = [&](uint32_t count) { completed = count == 0; };
        engine->rebuildContentIndex();
        check(completed, "Disabled content indexing completes without scheduling a scan");

        check(engine->recordCount() == 0, "Initial recordCount is 0");
        check(!engine->isScanning(), "Not scanning initially");
        check(!engine->isMonitoring(), "Not monitoring initially");
    }

    // Test 2: Full scan on tmpdir
    {
        ServiceConfig config;
        config.scanRoot = tmpBase.string();
        config.cachePath = cachePath;
        config.logPath = (tmpBase / "logs").string();

        auto se = std::make_unique<ServiceEngine>(config);
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        uint32_t finalCount = 0;
        bool didFullScan = false;

        se->startFullScan([&](uint32_t count, bool fullScan) {
            finalCount = count;
            didFullScan = fullScan;
            dispatch_semaphore_signal(sem);
        });

        long result = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
        check(result == 0, "Full scan completed within timeout");
        check(didFullScan, "Reported as full scan");
        // Should have at least: hello.txt, test.cpp, subdir, nested.log, tmpdir itself
        check(finalCount >= 4, "Found at least 4 records");

        // Test 3: Query on scanned data
        auto searchEngine = se->safeEngine();
        check(searchEngine != nullptr, "safeEngine returns non-null after scan");

        if (searchEngine) {
            auto indices = searchEngine->query("hello", 10);
            check(!indices.empty(), "Query 'hello' finds hello.txt");

            auto indices2 = searchEngine->query("nested", 10);
            check(!indices2.empty(), "Query 'nested' finds nested.log");

            auto indices3 = searchEngine->query("nonexistent_xyz", 10);
            check(indices3.empty(), "Query 'nonexistent_xyz' returns empty");
        }

        // Test 4: State queries
        check(!se->isScanning(), "Not scanning after scan completion");
        check(se->liveRecordCount() == finalCount, "liveRecordCount matches finalCount");

        // Queued rescans must keep their own copy of the path: the caller's string
        // is gone before the block runs, and an empty path would match every record.
        if (searchEngine) {
            { std::ofstream f(tmpBase / "subdir" / "added_later.txt"); f << "x"; }
            const uint32_t before = searchEngine->liveRecordCount();
            dispatch_semaphore_t rescanned = dispatch_semaphore_create(0);
            se->onIndexChanged = [&] { dispatch_semaphore_signal(rescanned); };
            // Same spelling as the scan root, so the rescan replaces the scanned records.
            se->rescanSubtree((tmpBase / "subdir").string());
            const bool done = dispatch_semaphore_wait(rescanned, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0;
            se->onIndexChanged = nullptr;
            check(done && !searchEngine->query("added_later", 10).empty(),
                  "Queued rescan indexes the directory it was given");
            check(searchEngine->liveRecordCount() == before + 1 && !searchEngine->query("hello", 10).empty(),
                  "Queued rescan leaves records outside the directory untouched");
        }

        // Test 5: Shutdown
        se->shutdown();
        check(!se->isScanning(), "Not scanning after shutdown");
    }

    // Test 6: Incremental load (cold start = full scan fallback)
    {
        // Use a fresh cache so there's no cached index
        std::string freshCache = (tmpBase / "cache_incr").string();
        fs::create_directories(freshCache);

        ServiceConfig config;
        config.scanRoot = tmpBase.string();
        config.cachePath = freshCache;
        config.logPath = (tmpBase / "logs").string();

        auto se = std::make_unique<ServiceEngine>(config);
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        uint32_t finalCount = 0;

        se->startIncremental([&](uint32_t count, bool) {
            finalCount = count;
            dispatch_semaphore_signal(sem);
        });

        long result = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC));
        check(result == 0, "Incremental (cold) completed within timeout");
        check(finalCount >= 4, "Incremental found at least 4 records");

        se->shutdown();
    }

    // FSEvents translation, with synthetic events: macOS may report a deleted or
    // moved directory as a single event for the directory alone.
    {
        const auto base = fs::temp_directory_path() / ("me_fsev_" + std::to_string(getpid()));
        fs::create_directories(base / "kept");
        const std::string root = fs::canonical(base).string();
        SearchEngine engine;
        auto add = [&](const std::string& dir, const std::string& name, uint8_t type) {
            FileRecord rec;
            rec.name = name;
            rec.path = dir;
            rec.type = type;
            engine.addRecord(std::move(rec));
        };
        add(root, "gone", 2);
        add(root + "/gone", "a.txt", 1);
        add(root + "/gone", "sub", 2);
        add(root + "/gone/sub", "b.txt", 1);
        add(root, "gone.txt", 1); // shares the prefix but is not below the directory
        add(root, "kept", 2);

        auto translate = [&](std::vector<FileSystemWatcher::Event> events) {
            ServiceEngine::FSEventMutations out;
            ServiceEngine::collectFSEventMutations(events, engine, true, out);
            return out;
        };
        const auto removedDir = kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemIsDir;
        auto out = translate({{root + "/gone", removedDir}});
        check(out.ops.size() == 1 && out.ops[0].type == SearchEngine::MutationOp::REMOVE_TREE && out.rescanDirs.empty(),
              "FSEvents: removed directory becomes one subtree removal");
        engine.batchMutate(std::move(out.ops));
        check(engine.indexForPath(root + "/gone") == UINT32_MAX && engine.indexForPath(root + "/gone/a.txt") == UINT32_MAX &&
              engine.indexForPath(root + "/gone/sub/b.txt") == UINT32_MAX && engine.indexForPath(root + "/gone.txt") != UINT32_MAX,
              "FSEvents: subtree removal clears the directory and everything below it, nothing beside it");

        // Removals below a subtree removal in the same batch collapse into it, and
        // later events for directories already emptied change nothing.
        add(root, "deep", 2);
        add(root + "/deep", "sub", 2);
        add(root + "/deep/sub", "x.txt", 1);
        add(root, "deeper.txt", 1);
        out = translate({{root + "/deep/sub/x.txt", kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemIsFile},
                         {root + "/deep/sub", removedDir}, {root + "/deep", removedDir}});
        check(out.ops.size() == 1 && out.ops[0].type == SearchEngine::MutationOp::REMOVE_TREE && out.ops[0].path == root + "/deep",
              "FSEvents: removals inside a removed directory collapse into one subtree removal");
        engine.batchMutate(std::move(out.ops));
        const uint32_t liveAfterTree = engine.liveRecordCount();
        out = translate({{root + "/deep/sub", removedDir}});
        engine.batchMutate(std::move(out.ops));
        check(engine.indexForPath(root + "/deep/sub/x.txt") == UINT32_MAX && engine.indexForPath(root + "/deeper.txt") != UINT32_MAX &&
              engine.liveRecordCount() == liveAfterTree,
              "FSEvents: a late event for an emptied directory removes nothing else");

        // No file/dir flag: the index decides whether the path was a directory.
        add(root, "old", 2);
        add(root + "/old", "c.txt", 1);
        out = translate({{root + "/old", kFSEventStreamEventFlagItemRenamed}, {root + "/gone.txt", kFSEventStreamEventFlagItemRemoved}});
        check(out.ops.size() == 2 && out.ops[0].type == SearchEngine::MutationOp::REMOVE_TREE &&
              out.ops[1].type == SearchEngine::MutationOp::REMOVE,
              "FSEvents: unflagged events use the indexed type (directory vs file)");

        // A directory that arrives by rename is recorded and rescanned.
        out = translate({{root + "/kept", kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemIsDir}});
        check(out.ops.size() == 1 && out.ops[0].type == SearchEngine::MutationOp::UPDATE &&
              out.rescanDirs == std::vector<std::string>{root + "/kept"},
              "FSEvents: directory renamed into place is updated and rescanned");

        // Removed and recreated within one batch: resync instead of deleting.
        out = translate({{root + "/kept", removedDir}});
        check(out.ops.size() == 1 && out.ops[0].type == SearchEngine::MutationOp::UPDATE && out.rescanDirs.size() == 1,
              "FSEvents: directory removed then recreated is rescanned, not deleted");

        // Rescans keep the directory's own record and replace what is below it.
        std::ofstream(base / "kept" / "fresh.txt") << "x";
        add(root + "/kept", "stale.txt", 1);
        ServiceEngine::rescanDirectoryInto(engine, root + "/kept");
        check(engine.indexForPath(root + "/kept") != UINT32_MAX && engine.indexForPath(root + "/kept/fresh.txt") != UINT32_MAX &&
              engine.indexForPath(root + "/kept/stale.txt") == UINT32_MAX,
              "FSEvents: rescan keeps the directory record and syncs its contents");
        fs::remove_all(base);
    }

    // Clean up
    fs::remove_all(tmpBase);

    return allOk;
}
