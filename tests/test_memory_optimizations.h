#pragma once
// ═══════════════════════════════════════════════════════
//  Part 21: Memory Optimization Integration Tests
//  Tests PathTable interning, recordTrigrams_ elimination,
//  forEachRecordWithPath zero-copy, and persistence roundtrip.
// ═══════════════════════════════════════════════════════

static void runMemoryOptimizationTests() {
    std::cout << "========================================\n";
    std::cout << "  Part 21: Memory Optimization Tests\n";
    std::cout << "========================================\n\n";

    {
        std::vector<uint64_t> values(4096, 42);
        const size_t previous = values.capacity();
        me::reserveForAppend(values);
        values.push_back(99);
        check(values.front() == 42 && values.back() == 99 && values.size() == 4097,
              "Growth: appending preserves loaded array contents");
        check(values.capacity() >= values.size() && values.capacity() <= previous + previous / 4,
              "Growth: a loaded array does not double after one file event");
        StringPool pool;
        pool.loadBulk(std::vector<std::string>(4096, "original"));
        for (int i = 0; i < 1000; ++i) pool.append("appended");
        check(pool.view(0) == "original" && pool.view(5095) == "appended",
              "Growth: string and entry buffers remain aligned through repeated growth");
    }

    {
        SearchEngine::SizeColumn sizes;
        const std::vector<uint64_t> wide{0, 1, UINT32_MAX - 1, UINT32_MAX, uint64_t(UINT32_MAX) + 1, UINT64_MAX};
        for (uint64_t v : wide) sizes.push_back(v);
        bool same = sizes.size() == wide.size();
        for (size_t i = 0; same && i < wide.size(); ++i) same = sizes[i] == wide[i];
        check(same && sizes.overflowCount() == 3, "NarrowColumn: sizes at and past 32 bits keep exact values");
        sizes.set(4, 7);
        sizes.set(1, UINT64_MAX - 1);
        check(sizes[4] == 7 && sizes[1] == UINT64_MAX - 1 && sizes.overflowCount() == 3,
              "NarrowColumn: set moves rows between narrow storage and the side map");
        auto copy = sizes;
        sizes.resize(2);
        check(sizes.size() == 2 && sizes.overflowCount() == 1 && copy[5] == UINT64_MAX,
              "NarrowColumn: shrinking drops side entries; copies stay independent");
        sizes.resize(4);
        check(sizes[2] == 0 && sizes[3] == 0, "NarrowColumn: growth fills zeros");

        SearchEngine::ModTimeColumn times(std::vector<int64_t>{-1, 0, 1792577860, INT64_MIN, int64_t(UINT32_MAX)});
        check(times[0] == -1 && times[1] == 0 && times[2] == 1792577860 && times[3] == INT64_MIN
              && times[4] == int64_t(UINT32_MAX) && times.overflowCount() == 3,
              "NarrowColumn: negative and out-of-range modification times keep exact values");

        // 32-bit keys must be produced the same way on insert and lookup.
        me::CompactPathIndex paths;
        std::vector<std::string> names;
        for (int i = 0; i < 20000; ++i) names.push_back("/p/" + std::to_string(i));
        auto resolve = [&](uint32_t i) -> const std::string& { return names[i]; };
        for (uint32_t i = 0; i < names.size(); ++i) me::setPathRecord(paths, names[i], i, resolve);
        paths.emplace(me::PathHash{}("/direct"), 0);
        bool found = paths.size() == names.size() + 1;
        for (uint32_t i = 0; found && i < names.size(); ++i)
            found = me::findPathRecord(paths, names[i], resolve)->second == i;
        check(found && paths.count(me::PathHash{}("/direct")) >= 1,
              "CompactPath: 32-bit hash keys resolve every inserted path");
    }

    {
        // Records 0..N-1 carry keys; a record's key can change between rounds.
        // Compare against a reference map, once with real hashes and once with
        // one shared hash so every probe collides and deletions wrap around.
        for (int mode = 0; mode < 2; ++mode) {
            me::PathRecordTable table;
            std::vector<uint32_t> keyOf(5000);
            std::map<uint32_t, uint32_t> reference; // key -> record
            auto hashKey = [&](uint32_t key) -> uint64_t {
                return mode ? 7 : key * 0x9e3779b97f4a7c15ULL;
            };
            auto hashOf = [&](uint32_t r) { return hashKey(keyOf[r]); };
            uint64_t seed = 12345;
            auto next = [&] { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return uint32_t(seed >> 33); };
            bool same = true;
            for (int step = 0; step < (mode ? 3000 : 40000) && same; ++step) {
                const uint32_t key = next() % 700, op = next() % 3;
                if (op < 2) {
                    const uint32_t record = next() % keyOf.size();
                    if (reference.count(keyOf[record]) && reference[keyOf[record]] == record) continue;
                    keyOf[record] = key;
                    table.assign(hashKey(key), record, [&](uint32_t r) { return keyOf[r] == key; }, hashOf);
                    reference[key] = record;
                } else if (auto it = reference.find(key); it != reference.end()) {
                    const size_t slot = table.slotOf(it->second, hashKey(key));
                    same = slot != me::PathRecordTable::npos;
                    if (same) table.eraseSlot(slot, hashOf);
                    reference.erase(it);
                }
                if (step % 97 == 0 || mode) {
                    same = same && table.size() == reference.size();
                    for (const auto& [k, r] : reference)
                        same = same && table.find(hashKey(k), [&](uint32_t o) { return keyOf[o] == k; }) == r;
                }
            }
            check(same, mode ? "PathTable: colliding keys survive wrap-around deletion"
                             : "PathTable: random assign/erase matches a reference map");
        }

        // Records stored in runs, extended runs and tail lists, minus tombstones.
        std::vector<uint8_t> types;
        std::vector<uint32_t> dirs;
        uint64_t seed = 99;
        auto next = [&] { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return uint32_t(seed >> 33); };
        for (int i = 0; i < 6000; ++i) {
            dirs.push_back(i < 3000 ? (i / 37) % 60 : next() % 70);
            types.push_back(next() % 9 == 0 ? 0 : 1);
        }
        auto map = me::DirRecordMap::build(types, dirs, 70);
        for (int i = 6000; i < 7000; ++i) {
            const uint32_t dir = i % 3 ? dirs.back() : next() % 80;
            dirs.push_back(dir);
            types.push_back(1);
            map.append(dir, i);
        }
        for (int i = 0; i < 400; ++i) types[next() % types.size()] = 0;
        bool same = true;
        for (uint32_t dir = 0; dir < 80 && same; ++dir) {
            std::vector<uint32_t> expected;
            for (uint32_t i = 0; i < types.size(); ++i)
                if (types[i] != 0 && dirs[i] == dir) expected.push_back(i);
            same = map.collect(dir, types) == expected;
        }
        check(same, "DirRecordMap: runs, appends and tombstones match per-directory lists");
        std::vector<uint8_t> scanTypes(3000, 1);
        std::vector<uint32_t> scanDirs(3000);
        for (uint32_t i = 0; i < 3000; ++i) scanDirs[i] = i / 37;
        check(me::DirRecordMap::build(scanTypes, scanDirs, 82).runCount() == 82,
              "DirRecordMap: a directory-ordered scan stores one run per directory");
    }

    {
        auto add = [](SearchEngine& engine, const std::string& dir, const std::string& name) {
            FileRecord rec;
            rec.name = name;
            rec.path = dir;
            rec.type = 1;
            return engine.addRecord(std::move(rec));
        };
        SearchEngine engine;
        const uint32_t upper = add(engine, "/Case/Dir", "Report.TXT");
        const uint32_t lower = add(engine, "/case/dir", "report.txt");
        check(engine.liveRecordCount() == 1 && engine.getRecord(upper).type == 0 &&
              engine.indexForPath("/CASE/DIR/REPORT.txt") == lower,
              "PathIndex: directories differing only in case share one path");
        const uint32_t slashed = add(engine, "/trail/", "x");
        const uint32_t plain = add(engine, "/trail", "x");
        check(engine.getRecord(slashed).type == 0 && engine.indexForPath("/trail/x") == plain,
              "PathIndex: a trailing slash on the directory names the same path");
        const uint32_t root = add(engine, "/", "top");
        const uint32_t relative = add(engine, "", "top");
        check(engine.indexForPath("/top") == root && engine.indexForPath("top") == relative,
              "PathIndex: root and relative records stay distinct");
        check(engine.removeByPath("/CASE/dir/Report.txt") && engine.indexForPath("/case/dir/report.txt") == UINT32_MAX &&
              !engine.removeByPath("/case/dir/report.txt"),
              "PathIndex: case-insensitive removal unmaps the record once");
        for (int i = 0; i < 3000; ++i) add(engine, "/bulk/d" + std::to_string(i % 7), "f" + std::to_string(i));
        check(engine.removeByPathPrefix("/BULK/d3") == 429 && engine.indexForPath("/bulk/d3/f3") == UINT32_MAX &&
              engine.indexForPath("/bulk/d4/f4") != UINT32_MAX,
              "PathIndex: prefix removal unmaps exactly the records below it");
        engine.compactRecords();
        bool mapped = engine.indexForPath("/trail/x") != UINT32_MAX && engine.indexForPath("/top") != UINT32_MAX;
        for (int i = 0; i < 3000; ++i) {
            const auto path = "/bulk/d" + std::to_string(i % 7) + "/f" + std::to_string(i);
            const uint32_t idx = engine.indexForPath(path);
            mapped = mapped && ((i % 7 == 3) == (idx == UINT32_MAX));
            if (idx != UINT32_MAX) mapped = mapped && engine.getRecord(idx).name == "f" + std::to_string(i);
        }
        check(mapped && engine.liveRecordCount() == 3000 - 429 + 3,
              "PathIndex: compaction rebuilds the table for every live path");
    }

    {
        const std::string file = "/tmp/test_memopt_columns_" + std::to_string(getpid()) + ".v6";
        SearchEngine engine;
        const uint64_t bigSize = 5ull << 30;
        for (int i = 0; i < 400; ++i) {
            FileRecord rec;
            rec.name = "col" + std::to_string(i) + ".bin";
            rec.path = "/columns";
            rec.type = 1;
            rec.size = i == 7 ? bigSize : uint64_t(i);
            rec.modTime = i == 8 ? time_t(-86400) : i == 9 ? time_t(int64_t(UINT32_MAX) + 5) : time_t(1700000000 + i);
            rec.inode = i + 1;
            rec.devId = i < 300 ? i * 31 : 5;
            engine.addRecord(std::move(rec));
        }
        IndexMetadata meta;
        check(FlatIndexWriter(file).fullRewrite(engine, meta), "Columns: v6 save succeeds");
        SearchEngine loaded;
        check(FlatIndexWriter(file).load(loaded, &meta), "Columns: v6 reload succeeds");
        loaded.completePhase2();
        bool exact = loaded.recordCount() == 400, dropped = true;
        for (uint32_t i = 0; exact && i < 400; ++i) {
            const auto a = engine.getRecord(i), b = loaded.getRecord(i);
            exact = a.size == b.size && a.modTime == b.modTime && a.name == b.name;
            dropped = dropped && a.inode == 0 && a.devId == 0 && b.inode == 0 && b.devId == 0;
        }
        check(exact, "Columns: v6 round trip keeps oversized sizes and negative times");
        check(dropped, "Columns: inode and device IDs are not kept in memory or on disk");
        const auto big = loaded.query("size:>4gb", 0);
        check(big.size() == 1 && loaded.getRecord(big[0]).size == bigSize,
              "Columns: size filter reads oversized values from the side map");
        std::remove(file.c_str());
    }

    {
        DirectoryPool tree;
        const std::vector<std::string> paths{"", "/", "/a", "/a/b", "/a/b/", "a/b", "//a///b", "/中文/大写A", "/a", std::string(65535, 'x')};
        for (const auto& path : paths) tree.append(path);
        bool same = true;
        for (uint32_t i = 0; i < paths.size(); ++i) same &= tree.str(i) == paths[i] && tree.length(i) == paths[i].size();
        check(same, "Directory tree: exact root, relative, repeated slash, Unicode and maximum-length roundtrip");
        auto copy = tree;
        tree.append("/new/directory");
        check(copy.entryCount() == paths.size() && copy.str(3) == "/a/b", "Directory tree: copied snapshot is independent");
        auto flat = copy.flatSnapshot();
        DirectoryPool restored(flat);
        check(restored.str(6) == paths[6] && !restored.isLive(0), "Directory tree: legacy flat entries preserve IDs and tombstones");
        DirectoryPool::Cursor cursor(tree);
        const auto first = std::string(cursor.data(3), tree.length(3));
        check(first == "/a/b" && std::string(cursor.data(4), tree.length(4)) == "/a/b/", "Directory tree: query cursor reconstructs changing parent IDs");
        DirectoryPool shared;
        for (int i = 0; i < 2000; ++i) shared.append("/a/very/long/shared/directory/prefix/" + std::to_string(i));
        check(shared.storageBytes() < 2000 * 42, "Directory tree: shared prefixes are not stored for every path");
    }

    {
        FoldedNamePool names;
        names.append("readme.txt", "readme.txt");
        names.append("ReadMe.TXT", "readme.txt");
        names.append("中文.txt", "中文.txt");
        names.append("İ", "i̇");
        check(names.original().data(0) == names.lower().data(0) && names.original().data(2) == names.lower().data(2),
              "Folded names: identical ASCII and Unicode names share bytes");
        check(names.original().view(1) == "ReadMe.TXT" && names.lower().view(1) == "readme.txt" && names.lower().length(3) == 3,
              "Folded names: changed case and Unicode byte lengths remain independent");
        auto copy = names;
        names.tombstone(1);
        for (int i = 0; i < 1000; ++i) names.append("extra", "extra");
        check(copy.original().view(1) == "ReadMe.TXT" && !names.lower().isLive(1), "Folded names: copied snapshots survive deletion and growth");
        FoldedNamePool restored;
        restored.load(copy.original().flatSnapshot(), copy.lower().flatSnapshot());
        check(restored.original().view(3) == "İ" && restored.lower().view(3) == "i̇", "Folded names: both views survive legacy flat roundtrip");
    }

    {
        auto backing = std::make_shared<std::string>("original");
        StringPool mapped;
        std::vector<StringPool::Entry> entries{{0, 8}};
        mapped.loadMappedRaw(backing->data(), backing->size(), std::move(entries), backing);
        auto copy = mapped;
        backing.reset();
        mapped.append(mapped.view(0));
        check(mapped.str(1) == "original" && copy.str(0) == "original", "Mapped input: self-append detaches safely while copied owner remains live");
        StringPool moved = std::move(copy);
        copy.append("reuse");
        check(moved.str(0) == "original" && copy.str(0) == "reuse", "Mapped input: move preserves backing and leaves reusable source");
        DirectoryPool tree;
        std::vector<std::string> paths{"/Users/UPPER/中文/İ", "/ΟΣ/Σ", std::string("/Ä/") + char(255), std::string("/A/\0B", 6)};
        bool same = true;
        for (const auto& path : paths) {
            auto id = tree.append(path);
            same &= tree.lower().str(id) == me::toLower(path);
            same &= tree.lower().length(id) == me::toLower(path).size();
        }
        check(same, "Directory lowercase: whole-path Unicode and invalid UTF-8 semantics are preserved");
    }

    // Force every path into one hash bucket to exercise collision handling.
    {
        me::CompactPathIndex paths;
        std::vector<std::string> names{"/one/a.txt", "/two/b.txt", "/one/a.txt"};
        auto resolve = [&](uint32_t i) -> const std::string& { return names[i]; };
        auto sameHash = [](std::string_view) -> size_t { return 1; };
        me::setPathRecord(paths, names[0], 0, resolve, sameHash);
        me::setPathRecord(paths, names[1], 1, resolve, sameHash);
        check(paths.size() == 2, "CompactPath: hash collision preserves distinct files");
        check(me::findPathRecord(paths, names[0], resolve, sameHash)->second == 0,
              "CompactPath: resolves first colliding path");
        check(me::findPathRecord(paths, names[1], resolve, sameHash)->second == 1,
              "CompactPath: resolves second colliding path");
        check(me::findPathRecord(paths, "/missing", resolve, sameHash) == paths.end(),
              "CompactPath: hash match cannot identify a missing file");
        me::setPathRecord(paths, names[2], 2, resolve, sameHash);
        check(paths.size() == 2 && me::findPathRecord(paths, names[0], resolve, sameHash)->second == 2,
              "CompactPath: duplicate path replaces only its own record");
        auto snapshot = paths;
        paths.erase(me::findPathRecord(paths, names[0], resolve, sameHash));
        check(paths.size() == 1 && me::findPathRecord(paths, names[1], resolve, sameHash)->second == 1,
              "CompactPath: deleting one collision preserves the other");
        const auto& saved = snapshot;
        check(me::findPathRecord(saved, names[0], resolve, sameHash)->second == 2,
              "CompactPath: copied const snapshot remains independently queryable");
    }

    // Packed postings must behave like sorted 32-bit IDs, including promotion.
    {
        me::PackedPostingList packed;
        std::vector<uint32_t> expected;
        const uint32_t values[] = {0, 1, 255, 256, 65535, 65536, 0xffffff};
        for (uint32_t v : values) { packed.push_back(v); expected.push_back(v); }
        packed.shrink_to_fit();
        check(packed.storageBytes() < expected.size() * sizeof(uint32_t), "Packed IDs: varint deltas retain full ID range in compact storage");
        packed.insert(std::lower_bound(packed.begin(), packed.end(), 1000), 1000);
        expected.insert(std::lower_bound(expected.begin(), expected.end(), 1000), 1000);
        auto copied = packed;
        packed.push_back(0x1000000);
        packed.push_back(UINT32_MAX);
        expected.push_back(0x1000000);
        expected.push_back(UINT32_MAX);
        check(std::equal(packed.begin(), packed.end(), expected.begin(), expected.end()),
              "Packed IDs: promotion preserves all 24-bit and 32-bit boundaries");
        packed.erase(std::lower_bound(packed.begin(), packed.end(), 256));
        expected.erase(std::lower_bound(expected.begin(), expected.end(), 256));
        check(std::equal(packed.begin(), packed.end(), expected.begin(), expected.end()),
              "Packed IDs: sorted insert and erase preserve values");
        check(copied.size() == 8 && copied[7] == 0xffffff, "Packed IDs: copied list owns independent storage");
        me::PackedPostingList empty;
        check(empty.empty() && empty.begin() == empty.end(), "Packed IDs: empty iterator range");
        for (uint32_t v = 0; v < 2000; ++v) empty.push_back(v * 10);
        bool equal = true;
        for (uint32_t v = 0; v < 2000; ++v) equal &= empty[v] == v * 10;
        check(equal, "Packed IDs: repeated growth preserves posting order");
    }
    {
        me::PackedPostingList packed;
        std::vector<uint32_t> reference;
        uint32_t seed = 7;
        auto random = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
        for (uint32_t i = 0; i < 4096; ++i) { packed.push_back(i * 1000); reference.push_back(i * 1000); }
        bool same = true;
        for (unsigned op = 0; op < 4000; ++op) {
            if (!reference.empty() && random() % 3 == 0) {
                size_t rank = random() % reference.size();
                packed.erase(packed.begin() + rank);
                reference.erase(reference.begin() + rank);
            } else {
                uint32_t value = op % 7 ? random() : 0;
                auto p = std::lower_bound(packed.begin(), packed.end(), value);
                auto r = std::lower_bound(reference.begin(), reference.end(), value);
                same &= p - packed.begin() == r - reference.begin();
                packed.insert(p, value); reference.insert(r, value);
            }
            same &= std::equal(packed.begin(), packed.end(), reference.begin(), reference.end());
            size_t rank = random() % reference.size();
            same &= packed[rank] == reference[rank];
        }
        check(same, "Delta IDs: randomized insert, erase, seek and sequential iteration agree with vector");
        while (!reference.empty()) { packed.erase(packed.begin()); reference.erase(reference.begin()); }
        packed.push_back(UINT32_MAX);
        check(packed.size() == 1 && packed[0] == UINT32_MAX, "Delta IDs: erasing checkpoints to empty permits full-range reuse");
        me::PackedPostingList dense;
        for (uint32_t i = 0; i < 10000; ++i) dense.push_back(i);
        dense.shrink_to_fit();
        check(dense.storageBytes() < dense.size() * 2, "Delta IDs: dense postings including checkpoints use less than two bytes per ID");
    }
    {
        SearchEngine engine;
        engine.loadRecords({{"first.txt", "/page", 1, 1, 1}, {"second.txt", "/page", 1, 2, 2}});
        const auto generation = engine.compactionGeneration();
        auto ids = engine.query(".txt");
        engine.removeByPath("/page/first.txt");
        size_t seen = 0;
        check(engine.forEachRecordWithPath(ids, [&](auto, const auto&, const auto&) { ++seen; }, generation)
                  && seen == 1, "Result pages: tombstones are skipped without shifting record IDs");
        engine.compactRecords();
        seen = 0;
        check(!engine.forEachRecordWithPath(ids, [&](auto, const auto&, const auto&) { ++seen; }, generation)
                  && seen == 0, "Result pages: stale IDs rejected under lock after compaction");
        const auto reloadedGeneration = engine.compactionGeneration();
        engine.loadRecords({{"replacement.txt", "/other", 1, 1, 1}});
        check(!engine.forEachRecordWithPath(ids, [](auto, const auto&, const auto&) {}, reloadedGeneration),
              "Result pages: reload also invalidates outstanding IDs");
    }

    // -- Test 1: Path deduplication in loadRecords --
    std::cout << "  --- Path deduplication in loadRecords ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        // 100 files in same directory — path should be dedup'd to 1 entry
        for (int i = 0; i < 100; i++) {
            records.push_back({"file_" + std::to_string(i) + ".txt", "/shared/dir", 1,
                               static_cast<uint64_t>(i * 10), static_cast<time_t>(1000 + i)});
        }
        // 50 files in another directory
        for (int i = 0; i < 50; i++) {
            records.push_back({"doc_" + std::to_string(i) + ".md", "/other/dir", 1,
                               static_cast<uint64_t>(i * 20), static_cast<time_t>(2000 + i)});
        }
        engine.loadRecords(std::move(records));

        check(engine.recordCount() == 150, "PathDedup: 150 records loaded");
        check(engine.liveRecordCount() == 150, "PathDedup: 150 live records");
        check(engine.pathTableSnapshot().size() == 2, "PathDedup: only 2 unique paths in PathTable");

        // Verify getRecord reconstructs path correctly
        auto r0 = engine.getRecord(0);
        check(r0.path == "/shared/dir", "PathDedup: getRecord(0).path reconstructed");
        check(r0.name == "file_0.txt", "PathDedup: getRecord(0).name correct");

        auto r100 = engine.getRecord(100);
        check(r100.path == "/other/dir", "PathDedup: getRecord(100).path reconstructed");
        check(r100.name == "doc_0.md", "PathDedup: getRecord(100).name correct");
    }

    // -- Test 2: Query results unchanged after path dedup --
    std::cout << "\n  --- Query correctness after path dedup ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"hello.txt", "/tmp", 1, 100, 1000});
        records.push_back({"world.cpp", "/tmp", 1, 200, 2000});
        records.push_back({"readme.md", "/home/user", 1, 300, 3000});
        records.push_back({"test.py", "/home/user/projects", 1, 400, 4000});
        engine.loadRecords(std::move(records));

        auto res = engine.query("hello");
        check(res.size() == 1, "Query: 'hello' finds 1 result");
        check(engine.getRecord(res[0]).name == "hello.txt", "Query: 'hello' correct file");

        res = engine.query("readme");
        check(res.size() == 1, "Query: 'readme' finds 1 result");

        // Path-based match — node-centric: name must contain "user"
        // No record has "user" in its name, so 0 results
        res = engine.query("/home/user");
        check(res.size() == 0, "Query: '/home/user' returns 0 (no node name contains 'user')");

        // Case insensitive
        res = engine.query("README");
        check(res.size() == 1, "Query: case-insensitive 'README' finds 1 result");
    }

    // -- Test 3: Mutation preserves path dedup --
    std::cout << "\n  --- Mutations with path dedup ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"a.txt", "/dir1", 1, 100, 1000});
        records.push_back({"b.txt", "/dir1", 1, 200, 2000});
        engine.loadRecords(std::move(records));

        // addRecord with existing path
        engine.addRecord({"c.txt", "/dir1", 1, 300, 3000});
        auto r = engine.getRecord(2);
        check(r.name == "c.txt", "Mutation: addRecord name correct");
        check(r.path == "/dir1", "Mutation: addRecord path reconstructed from PathTable");

        // addRecord with new path
        engine.addRecord({"d.txt", "/dir2", 1, 400, 4000});
        r = engine.getRecord(3);
        check(r.path == "/dir2", "Mutation: addRecord new path correct");

        // removeByPath
        bool removed = engine.removeByPath("/dir1/a.txt");
        check(removed, "Mutation: removeByPath returns true");
        check(engine.liveRecordCount() == 3, "Mutation: liveCount after remove");

        auto res = engine.query("a.txt");
        check(res.empty(), "Mutation: removed file not queryable");

        // updateByPath
        engine.updateByPath("/dir1/b.txt", {"b_v2.txt", "/dir1", 1, 999, 5000});
        res = engine.query("b_v2");
        check(res.size() == 1, "Mutation: updateByPath new name queryable");
        r = engine.getRecord(res[0]);
        check(r.path == "/dir1", "Mutation: updateByPath path correct");
    }

    // -- Test 4: removeByPathPrefix with path dedup --
    std::cout << "\n  --- removeByPathPrefix with path dedup ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"x.txt", "/prefix/sub", 1, 100, 1000});
        records.push_back({"y.txt", "/prefix/sub", 1, 200, 2000});
        records.push_back({"z.txt", "/other", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        uint32_t removed = engine.removeByPathPrefix("/prefix");
        check(removed == 2, "removeByPathPrefix: removed 2 records");
        check(engine.liveRecordCount() == 1, "removeByPathPrefix: 1 live record");

        auto res = engine.query("z.txt");
        check(res.size() == 1, "removeByPathPrefix: non-removed file still queryable");
    }

    // -- Test 5: Compaction rebuilds PathTable --
    std::cout << "\n  --- Compaction rebuilds PathTable ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        for (int i = 0; i < 10; i++) {
            records.push_back({"file_" + std::to_string(i) + ".txt", "/dir_" + std::to_string(i % 3),
                               1, static_cast<uint64_t>(i * 100), static_cast<time_t>(i)});
        }
        engine.loadRecords(std::move(records));
        check(engine.pathTableSnapshot().size() == 3, "Compact: 3 unique paths before");

        // Remove all files in /dir_0
        for (int i = 0; i < 10; i += 3) {
            engine.removeByPath("/dir_" + std::to_string(i % 3) + "/file_" + std::to_string(i) + ".txt");
        }

        auto remapResult = engine.compactRecords();
        check(!remapResult.empty(), "Compact: remap not empty");

        // After compaction, /dir_0 should be gone from PathTable if all its records were removed
        // But other paths should still work
        auto res = engine.query("file_1");
        check(res.size() == 1, "Compact: file_1 still queryable after compaction");
        auto r = engine.getRecord(res[0]);
        check(r.path == "/dir_1", "Compact: path correctly reconstructed after compaction");
    }

    // -- Test 6: forEachRecordWithPath matches getRecord --
    std::cout << "\n  --- forEachRecordWithPath vs getRecord ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"alpha.txt", "/path/a", 1, 100, 1000});
        records.push_back({"beta.txt", "/path/b", 1, 200, 2000});
        records.push_back({"gamma.txt", "/path/c", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // Collect via forEachRecordWithPath
        struct RecordInfo {
            std::string name;
            std::string path;
            uint8_t type;
            uint64_t size;
        };
        std::vector<RecordInfo> batchResults;
        std::vector<uint32_t> indices = {0, 1, 2};
        engine.forEachRecordWithPath(indices, [&](uint32_t, const FileRecord& r, const std::string& path) {
            batchResults.push_back({r.name, path, r.type, r.size});
        });

        check(batchResults.size() == 3, "forEachRecordWithPath: 3 results");

        // Compare with getRecord
        for (uint32_t i = 0; i < 3; i++) {
            auto single = engine.getRecord(i);
            check(batchResults[i].name == single.name,
                  ("forEachRecordWithPath: name[" + std::to_string(i) + "] matches getRecord").c_str());
            check(batchResults[i].path == single.path,
                  ("forEachRecordWithPath: path[" + std::to_string(i) + "] matches getRecord").c_str());
            check(batchResults[i].type == single.type,
                  ("forEachRecordWithPath: type[" + std::to_string(i) + "] matches getRecord").c_str());
            check(batchResults[i].size == single.size,
                  ("forEachRecordWithPath: size[" + std::to_string(i) + "] matches getRecord").c_str());
        }
    }

    // -- Test 7: forEachRecordWithPath skips tombstones --
    std::cout << "\n  --- forEachRecordWithPath skips tombstones ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"keep.txt", "/dir", 1, 100, 1000});
        records.push_back({"remove.txt", "/dir", 1, 200, 2000});
        records.push_back({"also_keep.txt", "/dir", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        engine.removeByPath("/dir/remove.txt");

        std::vector<std::string> names;
        std::vector<uint32_t> allIdx = {0, 1, 2};
        engine.forEachRecordWithPath(allIdx, [&](uint32_t, const FileRecord& r, const std::string&) {
            names.push_back(r.name);
        });

        check(names.size() == 2, "forEachRecordWithPath: skips tombstoned record");
        check(names[0] == "keep.txt", "forEachRecordWithPath: first live record correct");
        check(names[1] == "also_keep.txt", "forEachRecordWithPath: second live record correct");
    }

    // -- Test 8: Persistence roundtrip (save + load) --
    std::cout << "\n  --- Persistence roundtrip ---\n";
    {
        std::string tmpFile = "/tmp/test_memopt_persist_" + std::to_string(getpid()) + ".bin";

        // Build and save
        {
            SearchEngine engine;
            std::vector<FileRecord> records;
            records.push_back({"persist_a.txt", "/save/dir1", 1, 111, 1001, 9001, 42});
            records.push_back({"persist_b.txt", "/save/dir1", 1, 222, 2002, 9002, 42});
            records.push_back({"persist_c.txt", "/save/dir2", 1, 333, 3003, 9003, 43});
            engine.loadRecords(std::move(records));

            IndexMetadata meta;
            meta.lastEventId = 12345;
            meta.extra["test_key"] = "test_value";
            bool saved = engine.saveToFile(tmpFile, meta);
            check(saved, "Persistence: saveToFile succeeded");
        }

        // Load and verify
        {
            SearchEngine engine;
            IndexMetadata meta;
            bool loaded = engine.loadFromFile(tmpFile, &meta);
            check(loaded, "Persistence: loadFromFile succeeded");
            check(meta.lastEventId == 12345, "Persistence: lastEventId preserved");
            check(meta.extra.count("test_key") && meta.extra["test_key"] == "test_value",
                  "Persistence: extra metadata preserved");

            check(engine.recordCount() == 3, "Persistence: 3 records loaded");
            check(engine.pathTableSnapshot().size() == 2, "Persistence: 2 unique paths after load");

            auto r0 = engine.getRecord(0);
            check(r0.name == "persist_a.txt", "Persistence: record[0].name correct");
            check(r0.path == "/save/dir1", "Persistence: record[0].path reconstructed");
            check(r0.size == 111, "Persistence: record[0].size correct");
            check(r0.inode == 0 && r0.devId == 0, "Persistence: inode and device ID are not retained");

            auto r2 = engine.getRecord(2);
            check(r2.name == "persist_c.txt", "Persistence: record[2].name correct");
            check(r2.path == "/save/dir2", "Persistence: record[2].path reconstructed");

            // Query works after load
            auto res = engine.query("persist_b");
            check(res.size() == 1, "Persistence: query works after load");
        }

        remove(tmpFile.c_str());
    }

    // -- Test 9: WAL replay correctness --
    std::cout << "\n  --- WAL replay correctness ---\n";
    {
        std::string walPath = "/tmp/test_memopt_wal_" + std::to_string(getpid()) + ".wal";

        // Phase 1: create engine, attach WAL, mutate
        {
            SearchEngine engine;
            std::vector<FileRecord> records;
            records.push_back({"base.txt", "/wal/dir", 1, 100, 1000});
            engine.loadRecords(std::move(records));

            auto wal = std::make_shared<IndexWAL>();
            wal->open(walPath);
            engine.attachWAL(wal);

            engine.addRecord({"added.txt", "/wal/dir", 1, 200, 2000});
            engine.removeByPath("/wal/dir/base.txt");
            engine.addRecord({"final.txt", "/wal/other", 1, 300, 3000});

            engine.detachWAL();
        }

        // Phase 2: replay WAL into fresh engine
        {
            SearchEngine engine;
            std::vector<FileRecord> records;
            records.push_back({"base.txt", "/wal/dir", 1, 100, 1000});
            engine.loadRecords(std::move(records));

            auto entries = IndexWAL::readAll(walPath);
            check(entries.size() == 3, "WAL replay: 3 entries in WAL");

            for (const auto& entry : entries) {
                if (entry.op == WALOp::Add) {
                    engine.addRecord(FileRecord{entry.record});
                } else if (entry.op == WALOp::Remove) {
                    engine.removeByPath(entry.fullPath);
                }
            }

            check(engine.liveRecordCount() == 2, "WAL replay: 2 live records after replay");

            auto res = engine.query("added");
            check(res.size() == 1, "WAL replay: 'added' queryable");
            auto r = engine.getRecord(res[0]);
            check(r.path == "/wal/dir", "WAL replay: added record path correct");

            res = engine.query("base");
            check(res.empty(), "WAL replay: 'base' removed");

            res = engine.query("final");
            check(res.size() == 1, "WAL replay: 'final' queryable");
            r = engine.getRecord(res[0]);
            check(r.path == "/wal/other", "WAL replay: final record path correct");
        }

        remove(walPath.c_str());
    }

    // -- Test 10: Trigram index after recordTrigrams_ elimination --
    std::cout << "\n  --- Trigram without recordTrigrams_ ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"document.pdf", "/home", 1, 100, 1000});
        records.push_back({"dockerfile", "/app", 1, 200, 2000});
        records.push_back({"readme.md", "/home", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // Trigram search should work (3+ chars)
        auto res = engine.query("doc");
        check(res.size() == 2, "Trigram-noStore: 'doc' matches 2 files");

        // Add and search
        engine.addRecord({"doc_new.txt", "/home", 1, 400, 4000});
        res = engine.query("doc");
        check(res.size() == 3, "Trigram-noStore: 'doc' matches 3 after add");

        // Remove and verify trigram cleanup works without stored trigrams
        engine.removeByPath("/home/document.pdf");
        res = engine.query("document");
        check(res.empty(), "Trigram-noStore: 'document' not found after remove");

        // Remaining 'doc' matches should still work
        res = engine.query("doc");
        check(res.size() == 2, "Trigram-noStore: 'doc' matches 2 after remove");

        // Compaction should rebuild trigram index correctly
        engine.compactRecords();
        res = engine.query("doc");
        check(res.size() == 2, "Trigram-noStore: 'doc' matches 2 after compaction");
        res = engine.query("readme");
        check(res.size() == 1, "Trigram-noStore: 'readme' matches 1 after compaction");
    }

    // -- Test 11: resolveRecordPath accessor --
    std::cout << "\n  --- resolveRecordPath ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"a.txt", "/resolve/test", 1, 100, 1000});
        records.push_back({"b.txt", "/resolve/test", 1, 200, 2000});
        records.push_back({"c.txt", "/resolve/other", 1, 300, 3000});
        engine.loadRecords(std::move(records));

        // forEachRecordWithPath should provide the resolved path
        std::vector<std::string> paths;
        std::vector<uint32_t> allIdx = {0, 1, 2};
        engine.forEachRecordWithPath(allIdx, [&](uint32_t, const FileRecord&, const std::string& p) {
            paths.push_back(p);
        });

        check(paths[0] == "/resolve/test", "resolveRecordPath: idx 0 correct");
        check(paths[1] == "/resolve/test", "resolveRecordPath: idx 1 same path");
        check(paths[2] == "/resolve/other", "resolveRecordPath: idx 2 different path");
    }

    // -- Test 12: indexForPath still works --
    std::cout << "\n  --- indexForPath ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"target.txt", "/lookup/dir", 1, 100, 1000});
        records.push_back({"other.txt", "/lookup/dir", 1, 200, 2000});
        engine.loadRecords(std::move(records));

        uint32_t idx = engine.indexForPath("/lookup/dir/target.txt");
        check(idx == 0, "indexForPath: found correct index");

        idx = engine.indexForPath("/nonexistent/path.txt");
        check(idx == UINT32_MAX, "indexForPath: returns UINT32_MAX for missing");
    }

    // -- Test 13: PathTable::resolve bounds check --
    std::cout << "\n  --- PathTable bounds check ---\n";
    {
        PathTable pt;
        pt.intern("/a");
        pt.intern("/b");
        check(pt.size() == 2, "PathTable bounds: 2 entries");
        check(pt.resolve(0) == "/a", "PathTable bounds: resolve(0) works");
        check(pt.resolve(1) == "/b", "PathTable bounds: resolve(1) works");
        check(pt.resolve(999).empty(), "PathTable bounds: out-of-range returns empty");
        check(pt.resolve(UINT32_MAX).empty(), "PathTable bounds: UINT32_MAX returns empty");
    }

    // -- Test 14: resolveRecordPath returns value copy, bounds safe --
    std::cout << "\n  --- resolveRecordPath value copy + bounds ---\n";
    {
        SearchEngine engine;
        std::vector<FileRecord> records;
        records.push_back({"x.txt", "/val/copy", 1, 100, 1000});
        engine.loadRecords(std::move(records));

        std::string path = engine.resolveRecordPath(0);
        check(path == "/val/copy", "resolveRecordPath: valid index returns correct path");

        std::string oob = engine.resolveRecordPath(9999);
        check(oob.empty(), "resolveRecordPath: out-of-bounds returns empty");
    }

    std::cout << "\n";
}
