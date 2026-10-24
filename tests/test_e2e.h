#pragma once
// ═══════════════════════════════════════════════════════
//  Part 6: End-to-End Integration Test
//  (FSEvents -> SearchEngine pipeline)
// ═══════════════════════════════════════════════════════

static void runEndToEndTest() {
    std::cout << "========================================\n";
    std::cout << "  Part 6: End-to-End Integration\n";
    std::cout << "  (FSEvents -> SearchEngine pipeline)\n";
    std::cout << "========================================\n\n";

    std::string tmpDir = "/tmp/maceverything_e2e_" + std::to_string(getpid());
    fs::create_directories(tmpDir);
    // FSEvents reports resolved paths (/private/tmp/...), and so does the index.
    const std::string realDir = fs::canonical(tmpDir).string();

    SearchEngine engine;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<int> changeCount{0};

    // Same translation and rescans as the app's live monitoring, applied synchronously.
    FileSystemWatcher watcher;
    watcher.start(tmpDir, [&](std::vector<FileSystemWatcher::Event> events) {
        ServiceEngine::FSEventMutations mutations;
        ServiceEngine::collectFSEventMutations(events, engine, /*honorMustScanSubDirs=*/true, mutations);
        engine.batchMutate(std::move(mutations.ops));
        for (const auto& dir : mutations.rescanDirs) ServiceEngine::rescanDirectoryInto(engine, dir);
        changeCount.fetch_add(1);
        cv.notify_all();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Poll the index: event batches can split one change across several callbacks.
    auto waitUntil = [&](auto done, int timeoutMs) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::unique_lock<std::mutex> lock(mtx);
        while (!done()) {
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout) return done();
        }
        return true;
    };
    auto count = [&](const std::string& q) { return engine.query(q, 0).size(); };

    // Create a file
    fsInChild("printf 'e2e test content\\n' > '" + tmpDir + "/e2e_test.txt'");
    waitUntil([&] { return count("e2e_test") == 1; }, 3000);

    auto res = engine.query("e2e_test");
    check(res.size() == 1, "E2E: Created file appears in search");
    if (!res.empty()) {
        const auto& r = engine.getRecord(res[0]);
        check(r.type == 1, "E2E: Record type is file (1)");
        check(r.size > 0, "E2E: Record size > 0");
    }
    check(engine.liveRecordCount() >= 1, "E2E: liveRecordCount >= 1");

    // Rename the file
    fsInChild("mv '" + tmpDir + "/e2e_test.txt' '" + tmpDir + "/e2e_renamed.txt'");
    waitUntil([&] { return count("e2e_renamed") == 1 && count("e2e_test") == 0; }, 3000);
    check(count("e2e_renamed") == 1, "E2E: Renamed file appears in search");
    check(engine.indexForPath(realDir + "/e2e_test.txt") == UINT32_MAX, "E2E: Old path no longer in index");

    // Delete the file
    fsInChild("rm '" + tmpDir + "/e2e_renamed.txt'");
    waitUntil([&] { return count("e2e_renamed") == 0; }, 3000);
    check(count("e2e_renamed") == 0, "E2E: Deleted file removed from search");

    // Delete a directory tree. FSEvents may report only the top directory, so the
    // whole subtree must go with it.
    fsInChild("mkdir -p '" + tmpDir + "/tree/nested/deep' && cd '" + tmpDir + "/tree' && "
              "i=0; while [ $i -lt 20 ]; do touch e2etree_top$i.log nested/e2etree_mid$i.log nested/deep/e2etree_low$i.log; i=$((i+1)); done");
    bool treeIndexed = waitUntil([&] { return count("e2etree") == 60; }, 5000);
    check(treeIndexed, "E2E: New directory tree is indexed");
    fsInChild("rm -rf '" + tmpDir + "/tree'");
    waitUntil([&] { return count("e2etree") == 0 && engine.indexForPath(realDir + "/tree") == UINT32_MAX; }, 5000);
    check(count("e2etree") == 0, "E2E: Deleting a directory tree removes everything below it");
    check(engine.indexForPath(realDir + "/tree") == UINT32_MAX &&
          engine.indexForPath(realDir + "/tree/nested") == UINT32_MAX,
          "E2E: Deleted directories leave no records");

    // Rename a directory: the old paths disappear and the moved files are indexed
    // under the new path, although they produce no events of their own.
    fsInChild("mkdir -p '" + tmpDir + "/src/inner' && cd '" + tmpDir + "/src/inner' && "
              "i=0; while [ $i -lt 10 ]; do touch e2emove$i.txt; i=$((i+1)); done");
    waitUntil([&] { return count("e2emove") == 10; }, 5000);
    fsInChild("mv '" + tmpDir + "/src' '" + tmpDir + "/dst'");
    auto movedUnder = [&](const std::string& dir) {
        size_t n = 0;
        for (uint32_t idx : engine.query("e2emove", 0))
            if (engine.resolveRecordPath(idx) == realDir + "/" + dir + "/inner") ++n;
        return n;
    };
    // The moved directory's contents arrive through a rescan of the new path.
    bool moved = waitUntil([&] { return movedUnder("dst") == 10 && movedUnder("src") == 0; }, 5000);
    check(moved, "E2E: Renaming a directory moves its files to the new path");
    check(engine.indexForPath(realDir + "/src/inner/e2emove0.txt") == UINT32_MAX,
          "E2E: Old directory paths no longer in index");
    const uint32_t dstIdx = engine.indexForPath(realDir + "/dst");
    check(dstIdx != UINT32_MAX && engine.getRecord(dstIdx).type == 2,
          "E2E: Renamed directory keeps its own record");

    watcher.stop();
    fs::remove_all(tmpDir);
    std::cout << "\n";
}
