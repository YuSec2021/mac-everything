#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <chrono>
#include <iostream>
#include <sys/resource.h>
static void memory(const char* stage) {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) std::abort();
    std::cout << stage << " footprint_mib=" << info.phys_footprint/1048576.0 << " rss_mib=" << info.resident_size/1048576.0 << std::endl;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    SearchEngine engine; FlatIndexWriter reader(argv[1]); IndexMetadata meta;
    auto start=std::chrono::steady_clock::now();
    if (!reader.load(engine,&meta)) return 3;
    memory("phase1"); engine.completePhase2(); memory("ready");
    std::cout << "records=" << engine.recordCount() << " live=" << engine.liveRecordCount() << " startup_seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() << std::endl;
    for (const char* query : {"readme", "*.swift", "config", "/Applications/", "size:>1mb", "type:folder"}) {
        double elapsed=0; size_t resultCount=0;
        for(int i=0;i<3;++i){
            auto begin=std::chrono::steady_clock::now(); auto found=engine.query(query,100);
            elapsed+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count(); resultCount=found.size();
        }
        std::cout << "query=" << query << " mean_ms=" << elapsed/3 << " count=" << resultCount << std::endl;
    }
    for (uint32_t i=0;i<engine.recordCount();i+=10000) {
        auto r=engine.getRecord(i); if(!r.type) continue;
        auto path=SearchEngine::makeFullPath(r.path,r.name);
        if(engine.indexForPath(path)!=i) {std::cerr << "path lookup mismatch at " << i << std::endl; return 4;}
    }
    memory("after_queries");
    for (uint32_t i=0;i<100;++i) {
        FileRecord r; r.name="memory-check-" + std::to_string(i) + ".txt";
        r.path="/synthetic-memory-check"; r.type=1; r.modTime=1;
        engine.addRecord(std::move(r));
    }
    memory("after_100_changes");
    malloc_zone_pressure_relief(nullptr,0); memory("after_allocator_relief");
}
