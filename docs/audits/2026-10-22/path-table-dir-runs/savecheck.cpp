#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <cstdio>
// Load the fixture, save it, reload, and compare every kept field.
int main(int, char** argv) {
    SearchEngine a, b; IndexMetadata meta;
    if (!FlatIndexWriter(argv[1]).load(a, &meta)) return 3;
    if (!FlatIndexWriter(argv[2]).fullRewrite(a, meta)) return 4;
    if (!FlatIndexWriter(argv[2]).load(b, &meta)) return 5;
    size_t diff = 0, nonzero = 0, unmapped = 0;
    for (uint32_t i = 0; i < a.recordCount(); ++i) {
        auto x = a.getRecord(i), y = b.getRecord(i);
        diff += x.size != y.size || x.modTime != y.modTime || x.type != y.type || x.name != y.name || x.path != y.path;
        nonzero += y.inode != 0 || y.devId != 0;
        if (y.type != 0) unmapped += b.indexForPath(SearchEngine::makeFullPath(y.path, y.name)) != i;
    }
    std::printf("records=%u reloaded=%u live=%u fieldDiffs=%zu nonzeroInodeDev=%zu liveNotFoundByPath=%zu\n",
        a.recordCount(), b.recordCount(), b.liveRecordCount(), diff, nonzero, unmapped);
}
