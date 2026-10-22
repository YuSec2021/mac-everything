#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <cstdio>
int main(int, char** argv) {
    SearchEngine a, b; IndexMetadata meta;
    if (!FlatIndexWriter(argv[1]).load(a, &meta)) return 3;
    if (!FlatIndexWriter(argv[2]).fullRewrite(a, meta)) return 4;
    if (!FlatIndexWriter(argv[2]).load(b, &meta)) return 5;
    size_t diff = 0, big = 0, neg = 0;
    for (uint32_t i = 0; i < a.recordCount(); ++i) {
        auto x = a.getRecord(i), y = b.getRecord(i);
        diff += x.size != y.size || x.modTime != y.modTime || x.devId != y.devId || x.inode != y.inode || x.type != y.type || x.name != y.name;
        big += x.size > UINT32_MAX;
    }
    std::printf("records=%u reloaded=%u fieldDiffs=%zu big=%zu sizeOverflow=%zu mtimeOverflow=%zu devDict=%zu devOverflow=%zu\n",
        a.recordCount(), b.recordCount(), diff, big, a.sizes_.overflowCount(), a.modTimes_.overflowCount(),
        a.devIds_.dictionarySize(), a.devIds_.overflowCount());
}
