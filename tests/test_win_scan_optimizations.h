#pragma once

static void runWinScanOptimizationTests() {
    std::cout << "\n── Part 77: Specialized scans and bounded results ──\n";
    {
        DirectoryPool tree;
        std::vector<std::string> paths{"", "/", "/a/b/", "a/b", "//a///b", "/a/deep/b", "/Users/中文/İ", "/ΟΣ/Σ", std::string("/Ä/") + char(255)};
        std::string expanding;
        for (int i = 0; i < 32000; ++i) expanding += "İ";
        paths.push_back(expanding + "/tail");
        for (int i = 0; i < 3000; ++i) paths.push_back("/root/" + std::to_string(i % 17) + "/a/" + (i % 3 ? "deep/" : "") + "b");
        const std::vector<std::vector<PathSegment>> patterns{
            {}, {{"a", true}}, {{"a", true}, {"b", true}}, {{"a", false}, {"b", false}},
            {{"root", true}, {"b", true}}, {{"中文", true}}, {{"i̇", true}}, {{"tail", true}}, {{"missing", false}}
        };
        bool same = true;
        for (const auto& path : paths) {
            uint32_t id = tree.append(path);
            for (const auto& pattern : patterns)
                same &= tree.matchesSegments(id, pattern) == SearchEngine::pathSegmentsMatch(tree.lower().str(id), pattern);
        }
        check(same, "Native directory matching agrees with flattened components, including adjacency, Unicode and truncation");
    }
    SearchEngine engine;
    std::vector<FileRecord> records;
    records.reserve(120000);
    for (int i = 0; i < 120000; ++i) {
        FileRecord record;
        record.name = (i % 10 == 0 ? "needleHit-" : "item_a-") + std::to_string(i) + (i % 20 == 0 ? ".TXT" : ".txt");
        record.path = "/common/deep/目录/" + std::to_string(i % 19);
        record.type = i % 7 == 0 ? 2 : 1;
        record.size = i % 4096;
        record.modTime = 1700000000 + i;
        records.push_back(std::move(record));
    }
    engine.loadRecords(std::move(records));
    for (const char* query : {"needle", "case:needleHit", "a", "*.txt", "regex:needle.*", "ext:txt", "size:>=0",
                              "size:>512", "folder:", "path:common", "!needle", "common/a", "<needle|item> size:>0"}) {
        for (bool useIndex : {false, true}) {
            const auto full = engine.query(query, 0, useIndex);
            for (uint32_t limit : {1u, 3u, 17u, 100u, 10000u}) {
                const std::vector<uint32_t> expected(full.begin(), full.begin() + std::min<size_t>(limit, full.size()));
                check(engine.query(query, limit, useIndex) == expected, (std::string("Bounded worker preserves ordered prefix: ") + query).c_str());
            }
        }
    }
    for (const char* query : {"needle", "case:needleHit", "a", "目录"}) {
        check(engine.query(query, 0, false) == engine.query(std::string(query) + " size:>=0", 0, false),
              (std::string("Single substring loop agrees with generic AST evaluation: ") + query).c_str());
    }
    engine.removeByPath("/common/deep/目录/0/needleHit-0.TXT");
    engine.addRecord({"needleHit-new.TXT", "/new/目录", 1, 123, 1700000000});
    auto all = engine.query("needle", 0);
    check(engine.query("needle", 17) == std::vector<uint32_t>(all.begin(), all.begin() + 17), "Bounded ranking survives incremental deletion and insertion");
}
