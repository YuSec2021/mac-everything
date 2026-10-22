#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <chrono>
#include <cstdio>
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }
// Mutation-path latency through the public API only, so it links against any version.
int main(int, char** argv) {
    SearchEngine e; IndexMetadata meta;
    if (!FlatIndexWriter(argv[1]).load(e, &meta)) return 3;
    e.completePhase2();
    const uint32_t n = e.recordCount();
    std::vector<std::string> paths;
    uint64_t seed = 7;
    while (paths.size() < 100000) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const uint32_t i = uint32_t(seed >> 33) % n;
        auto r = e.getRecord(i);
        if (r.type) paths.push_back(SearchEngine::makeFullPath(r.path, r.name));
    }
    auto t = Clock::now(); size_t hits = 0;
    for (auto& p : paths) hits += e.indexForPath(p) != UINT32_MAX;
    std::printf("lookup_hit_us=%.3f hits=%zu\n", ms(t) * 1000 / paths.size(), hits);
    t = Clock::now(); size_t misses = 0;
    for (auto& p : paths) misses += e.indexForPath(p + ".missing") == UINT32_MAX;
    std::printf("lookup_miss_us=%.3f misses=%zu\n", ms(t) * 1000 / paths.size(), misses);
    t = Clock::now();
    for (size_t k = 0; k < 20000; ++k) {
        const uint32_t i = e.indexForPath(paths[k]);
        if (i == UINT32_MAX) continue;
        auto r = e.getRecord(i); r.size += 1;
        e.updateByPath(paths[k], std::move(r));
    }
    std::printf("update_us=%.3f live=%u\n", ms(t) * 1000 / 20000, e.liveRecordCount());
    // Small prefix: one directory taken from a sampled record.
    const std::string small = paths[12345].substr(0, paths[12345].rfind('/'));
    t = Clock::now();
    const uint32_t smallRemoved = e.removeByPathPrefix(small);
    std::printf("small_prefix_remove_ms=%.2f removed=%u\n", ms(t), smallRemoved);
    t = Clock::now();
    const uint32_t rescanRemoved = e.batchRescanPrefix("/Library/Fonts", {});
    std::printf("rescan_prefix_ms=%.1f removed=%u\n", ms(t), rescanRemoved);
    t = Clock::now();
    const uint32_t removed = e.removeByPathPrefix("/System/Library/Frameworks");
    std::printf("prefix_remove_ms=%.1f removed=%u live=%u\n", ms(t), removed, e.liveRecordCount());
}
