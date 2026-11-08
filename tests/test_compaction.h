#pragma once
// ═══════════════════════════════════════════════════════
//  Part 3d: Compact Records Tests
// ═══════════════════════════════════════════════════════

static std::string compactionFilePath(uint32_t i) {
    return "/overlap/dir_" + std::to_string(i % 500) + "/file_" + std::to_string(i) + ".txt";
}

// Leaves the odd-numbered files live and the even-numbered ones tombstoned.
static void loadHalfTombstoned(SearchEngine& engine, uint32_t fileCount) {
    std::vector<FileRecord> records;
    records.reserve(fileCount);
    for (uint32_t i = 0; i < fileCount; i++) {
        records.push_back({"file_" + std::to_string(i) + ".txt", "/overlap/dir_" + std::to_string(i % 500),
                           1, 10, (time_t)i});
    }
    engine.loadRecords(std::move(records));
    for (uint32_t i = 0; i < fileCount; i += 2) {
        engine.removeByPath(compactionFilePath(i));
    }
}

static void runOverlappingCompactionTests() {
    constexpr uint32_t kFiles = 100000;

    bool countsKept = true;
    bool recordsFindable = true;
    bool tombstonesReclaimed = true;
    for (int round = 0; round < 5; round++) {
        SearchEngine engine;
        loadHalfTombstoned(engine, kFiles);

        std::atomic<int> ready{0};
        auto compact = [&] {
            ready.fetch_add(1);
            while (ready.load() < 2) {}
            engine.compactRecords();
        };
        std::thread first(compact);
        std::thread second(compact);
        first.join();
        second.join();

        countsKept = countsKept && engine.liveRecordCount() == kFiles / 2;
        tombstonesReclaimed = tombstonesReclaimed && engine.recordCount() == kFiles / 2;
        for (uint32_t i = 1; i < kFiles; i += 2) {
            if (engine.indexForPath(compactionFilePath(i)) == UINT32_MAX) {
                recordsFindable = false;
                break;
            }
        }
    }
    check(countsKept, "Compact overlap: two simultaneous compactions keep every live record");
    check(recordsFindable, "Compact overlap: every live file is still found by path");
    check(tombstonesReclaimed, "Compact overlap: tombstones are still reclaimed");

    // A reload renumbers every record too, so a compaction it interrupts must not
    // install data built from the replaced records.
    bool reloadKept = true;
    for (int delayMs : {1, 2, 5, 10, 20, 40}) {
        SearchEngine engine;
        loadHalfTombstoned(engine, kFiles);

        std::thread compactor([&] { engine.compactRecords(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        std::vector<FileRecord> fresh;
        fresh.push_back({"fresh.txt", "/reloaded", 1, 1, 1});
        engine.loadRecords(std::move(fresh));
        compactor.join();

        reloadKept = reloadKept && engine.liveRecordCount() == 1 && engine.recordCount() == 1
            && engine.indexForPath("/reloaded/fresh.txt") != UINT32_MAX;
    }
    check(reloadKept, "Compact overlap: a reload during compaction is not overwritten");
}

static void runCompactionTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 3d: Compact Records Tests\n";
    std::cout << "========================================\n\n";

    SearchEngine engine;
    std::vector<FileRecord> records;
    for (int i = 0; i < 100; i++) {
        records.push_back({"compact_" + std::to_string(i) + ".txt", "/test", 1, (uint64_t)i, (time_t)i});
    }
    engine.loadRecords(std::move(records));

    check(engine.recordCount() == 100, "Compact: initial recordCount == 100");
    check(engine.liveRecordCount() == 100, "Compact: initial liveRecordCount == 100");

    // Remove half the records
    for (int i = 0; i < 50; i++) {
        engine.removeByPath("/test/compact_" + std::to_string(i) + ".txt");
    }
    check(engine.liveRecordCount() == 50, "Compact: after removing 50, liveRecordCount == 50");
    check(engine.recordCount() == 100, "Compact: after removing 50, recordCount still 100 (tombstones)");

    // Compact
    engine.compactRecords();
    check(engine.recordCount() == 50, "Compact: after compaction, recordCount == 50");
    check(engine.liveRecordCount() == 50, "Compact: after compaction, liveRecordCount == 50");

    // Verify remaining records are still queryable
    auto res = engine.query("compact_50");
    check(res.size() == 1, "Compact: compact_50.txt still queryable after compaction");
    check(engine.getRecord(res[0]).name == "compact_50.txt", "Compact: correct record data after compaction");

    // Verify removed records are gone
    res = engine.query("compact_0");
    check(res.empty(), "Compact: compact_0.txt not queryable after compaction");

    // Compact when nothing to compact
    engine.compactRecords();
    check(engine.recordCount() == 50, "Compact: no-op compaction preserves recordCount");

    runOverlappingCompactionTests();

    std::cout << "\n";
}
