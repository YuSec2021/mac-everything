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
static double inuse() { malloc_statistics_t s{}; malloc_zone_statistics(nullptr, &s); return s.size_in_use / 1048576.0; }
template<class T> void drop(T& x) { T empty; std::swap(x, empty); }
int main(int, char** argv) {
    SearchEngine e; IndexMetadata meta;
    if (!FlatIndexWriter(argv[1]).load(e, &meta)) return 3;
    std::printf("%-28s footprint=%8.1f in_use=%8.1f MiB\n", "after load", fp(), inuse());
    e.completePhase2();
    std::printf("%-28s footprint=%8.1f in_use=%8.1f MiB\n", "after phase2", fp(), inuse());
    const double M = 1048576.0;
    std::printf("records=%zu live=%u pathIndex entries=%zu slots_mib=%.1f dirCanon_mib=%.1f dirRuns=%zu dirMap_mib=%.1f\n",
        e.types_.size(), e.liveRecordCount(), e.pathIndex_.size(), e.pathIndex_.storageBytes() / M,
        e.dirCanon_.capacity() * 4 / M, e.pathIdxToRecords_.runCount(), e.pathIdxToRecords_.storageBytes() / M);
    double before;
#define STEP(name, stmt) before = inuse(); stmt; std::printf("drop %-22s -> in_use -%.1f MiB\n", name, before - inuse());
    STEP("pathIndex_+dirCanon_", (drop(e.pathIndex_), drop(e.dirCanon_)));
    STEP("pathLookup+lower", (drop(e.pathLookup_), drop(e.lowerPathLookup_)));
    STEP("pathIdxToRecords_", drop(e.pathIdxToRecords_));
    STEP("extensionIndex_", drop(e.extensionIndex_));
    STEP("nameTrigramIndex_", drop(e.nameTrigramIndex_));
    STEP("names_", drop(e.names_));
    STEP("pathPool_", drop(e.pathPool_));
    STEP("soa", (drop(e.pathIndices_), drop(e.types_), SearchEngine::SizeColumn{}.swap(e.sizes_), SearchEngine::ModTimeColumn{}.swap(e.modTimes_)));
    std::printf("%-28s footprint=%8.1f in_use=%8.1f MiB\n", "all dropped", fp(), inuse());
}
