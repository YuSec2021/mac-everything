#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <cstdio>
#include <algorithm>
int main(int, char** argv) {
    SearchEngine e; IndexMetadata meta;
    if (!FlatIndexWriter(argv[1]).load(e, &meta)) return 3;
    std::vector<uint32_t> cnt(e.pathPool_.entryCount());
    size_t live = 0;
    for (size_t i = 0; i < e.types_.size(); ++i) if (e.types_[i]) { ++cnt[e.pathIndices_[i]]; ++live; }
    const uint32_t th[] = {16, 64, 256, 1024, 4096, 16384};
    for (uint32_t t : th) {
        size_t dirs = 0, recs = 0;
        for (uint32_t c : cnt) if (c > t) { ++dirs; recs += c; }
        std::printf("dirs>%-6u count=%-7zu records=%-8zu (%.1f%%)\n", t, dirs, recs, 100.0 * recs / live);
    }
    std::printf("largest=%u\n", *std::max_element(cnt.begin(), cnt.end()));
    // Cost model: sum over records of its dir size = expected compares per random lookup * live
    double sq = 0; for (uint32_t c : cnt) sq += double(c) * c;
    std::printf("avg dir size seen by a random record=%.0f\n", sq / live);
}
