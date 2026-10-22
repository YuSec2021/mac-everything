#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <mach/mach.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
using Clock = std::chrono::steady_clock;
static double footprint() {
    task_vm_info_data_t info{}; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n)) std::abort();
    return info.phys_footprint / 1048576.0;
}
template<class Engine> size_t pathBytes(const Engine& e) {
    size_t bytes = 0;
    if constexpr (requires { e.pathTrigramIndex_; })
        for (const auto& [_, list] : e.pathTrigramIndex_) bytes += list.storageBytes();
    return bytes;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    SearchEngine e; IndexMetadata meta;
    const auto start = Clock::now();
    if (!FlatIndexWriter(argv[1]).load(e, &meta)) return 3;
    e.completePhase2();
    std::cout << "ready_mib=" << footprint() << " live=" << e.liveRecordCount()
        << " path_postings_mib=" << pathBytes(e)/1048576.0 << " startup_s="
        << std::chrono::duration<double>(Clock::now()-start).count() << std::endl;
    for (const char* q : {"readme", "config", "*.swift", "size:>1mb", "type:folder", "/Applications/",
            "MacEverything/readme", "site-packages/config", "site-packages/a",
            "MacEverything/a", "com.apple.Safari/a", "MacEverything/.swift", "my_product/a",
            "path:MacEverything ext:swift", "RareSegment/a | readme", "!MacEverything/a"}) {
        e.query(q, 10000);
        std::vector<double> times;
        std::vector<uint32_t> result;
        QueryTimingInfo timing;
        for (int i=0; i<5; ++i) {
            const auto t=Clock::now(); result=e.query(q, 10000, true, timing);
            times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-t).count());
        }
        std::sort(times.begin(),times.end());
        uint64_t hash=1469598103934665603ULL;
        for(auto id:result) hash=(hash^id)*1099511628211ULL;
        std::cout << "query=" << std::quoted(q) << " median_ms=" << times[2]
            << " count=" << result.size() << " hash=" << hash << " mode=" << timing.searchPath << std::endl;
    }
    for (const char* q : {"MacEverything/a", "com.apple.Safari/a", "MacEverything/.swift",
                          "site-packages/a", "my_product/a", "MacEverything/a | MacEverything/.swift", "!MacEverything/a"}) {
        for(uint32_t limit:{0u,10000u}) {
            const auto fast=e.query(q,limit,true), full=e.query(q,limit,false);
            std::cout<<"verify="<<std::quoted(q)<<" limit="<<limit<<" fast="<<fast.size()
                <<" full="<<full.size()<<" equal="<<(fast==full)<<std::endl;
            if(fast!=full)return 4;
        }
    }
}
