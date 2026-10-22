#include <unistd.h>
#include <cstdlib>
#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <cstdio>
static double fp() {
    task_vm_info_data_t info{}; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n);
    return info.phys_footprint / 1048576.0;
}
static void heap(const char* tag) {
    malloc_statistics_t s{}; malloc_zone_statistics(nullptr, &s);
    std::printf("%-28s footprint=%8.1f in_use=%8.1f allocated=%8.1f MiB\n", tag, fp(),
                s.size_in_use / 1048576.0, s.size_allocated / 1048576.0);
}
template<class T> size_t vcap(const std::vector<T>& v) { return v.capacity() * sizeof(T); }
template<class T> void drop(T& x) { T empty; std::swap(x, empty); }
int main(int argc, char** argv) {
    SearchEngine e; IndexMetadata meta;
    if (!FlatIndexWriter(argv[1]).load(e, &meta)) return 3;
    heap("after load");
    e.completePhase2();
    heap("after phase2");
    malloc_zone_pressure_relief(nullptr, 0);
    heap("after pressure relief");
    
    
    { char cmd[128]; std::snprintf(cmd, sizeof cmd, "vmmap --summary %d > %s/vmmap.txt 2>&1", getpid(), getenv("OUT")); std::system(cmd); if (getenv("HOLD")) sleep(40); }
    const double M = 1048576.0;
    size_t soa = vcap(e.pathIndices_) + vcap(e.types_) + e.sizes_.storageBytes() + e.modTimes_.storageBytes() + vcap(e.inodes_) + e.devIds_.storageBytes();
    std::printf("records=%zu dirs=%u trigrams=%zu exts=%zu\n", e.types_.size(), (unsigned)e.pathPool_.paths_.size(),
                e.nameTrigramIndex_.size(), e.extensionIndex_.size());
    std::printf("cap: soa=%.1f (sizes=%.1f mtime=%.1f inode=%.1f dev=%.1f pidx=%.1f type=%.1f)\n", soa/M,
                e.sizes_.storageBytes()/M, e.modTimes_.storageBytes()/M, vcap(e.inodes_)/M, e.devIds_.storageBytes()/M, vcap(e.pathIndices_)/M, vcap(e.types_)/M);
    size_t p2r = vcap(e.pathIdxToRecords_), p2rn = 0;
    for (auto& v : e.pathIdxToRecords_) { p2r += vcap(v); p2rn += v.empty() ? 0 : 1; }
    size_t ext = 0; for (auto& [k, v] : e.extensionIndex_) ext += vcap(v) + k.capacity();
    std::printf("cap: pathIdxToRecords=%.1f (nonempty=%zu) ext=%.1f pathIndex_entries=%zu pathLookup=%zu lowerLookup=%zu\n",
                p2r/M, p2rn, ext/M, e.pathIndex_.size(), e.pathLookup_.size(), e.lowerPathLookup_.size());
    // Measure real reclaim per structure (includes allocator/node overhead).
    double before;
auto inuse=[]{malloc_statistics_t s{}; malloc_zone_statistics(nullptr,&s); return s.size_in_use/1048576.0;};
#define STEP(name, stmt) before = inuse(); stmt; \
    std::printf("drop %-22s -> in_use -%.1f MiB\n", name, before - inuse());
    STEP("pathIndex_", drop(e.pathIndex_));
    STEP("pathLookup+lower", (drop(e.pathLookup_), drop(e.lowerPathLookup_)));
    STEP("pathIdxToRecords_", drop(e.pathIdxToRecords_));
    STEP("extensionIndex_", drop(e.extensionIndex_));
    STEP("nameTrigramIndex_", drop(e.nameTrigramIndex_));
    STEP("names_", drop(e.names_));
    STEP("pathPool_", drop(e.pathPool_));
    STEP("soa", (drop(e.pathIndices_), drop(e.types_), e.sizes_.clear(), e.modTimes_.clear(), SearchEngine::SizeColumn{}.swap(e.sizes_), SearchEngine::ModTimeColumn{}.swap(e.modTimes_), drop(e.inodes_), SearchEngine::DevIdColumn{}.swap(e.devIds_)));
    heap("all dropped");
}
