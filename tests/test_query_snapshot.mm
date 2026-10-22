#import "MacSearchBridge_Internal.h"
#include <cassert>
#include <iostream>

// Standalone bridge check; inject an in-memory engine without scanning or starting services.
int main() {
    @autoreleasepool {
        MacSearchBridge *bridge = [[MacSearchBridge alloc] init];
        auto engine = std::make_shared<SearchEngine>();
        std::vector<FileRecord> records;
        for (int i = 0; i < 1000; ++i)
            records.push_back({"page-" + std::to_string(i) + ".txt", "/snapshot-test", 1, uint64_t(i), i});
        engine->loadRecords(std::move(records));
        bridge->_serviceEngine->setEngine(engine);
        auto snapshot = [bridge querySnapshot:@".txt" maxResults:10000 sessionId:101];
        auto full = [bridge queryResults:@".txt" maxResults:10000];
        assert(snapshot.count == 1000 && full.count == 1000);
        auto first = [snapshot resultsAtOffset:0 count:100];
        auto second = [snapshot resultsAtOffset:100 count:100];
        assert(first.count == 100 && second.count == 100);
        for (NSUInteger i = 0; i < 100; ++i) {
            assert([first[i].name isEqualToString:full[i].name]);
            assert([second[i].name isEqualToString:full[i + 100].name]);
            assert([second[i].path isEqualToString:full[i + 100].path]);
        }
        assert([snapshot resultsAtOffset:950 count:NSIntegerMax].count == 50);
        assert([snapshot resultsAtOffset:1000 count:100].count == 0);
        assert([snapshot resultsAtOffset:NSIntegerMax count:100].count == 0);
        assert([snapshot resultsAtOffset:-1 count:100].count == 0);
        assert([snapshot resultsAtOffset:0 count:-1].count == 0);
        engine->removeByPath(std::string([first[0].path UTF8String]) + "/" + [first[0].name UTF8String]);
        assert([snapshot resultsAtOffset:0 count:100].count == 99);
        engine->compactRecords();
        assert([snapshot resultsAtOffset:0 count:100] == nil);
        auto fresh = [bridge querySnapshot:@".txt" maxResults:10000 sessionId:101];
        assert(fresh.count == 999 && [fresh resultsAtOffset:0 count:100].count == 100);
        auto replacement = std::make_shared<SearchEngine>();
        replacement->loadRecords({{"different.txt", "/new-engine", 1, 1, 1}});
        bridge->_serviceEngine->setEngine(replacement);
        assert([fresh resultsAtOffset:0 count:100].count == 100);
        std::cout << "Snapshot bridge: paging, ordering, bounds, deletion, compaction and engine lifetime passed\n";
    }
}
