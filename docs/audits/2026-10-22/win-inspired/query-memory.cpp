#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <mach/mach.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
static double footprint(){task_vm_info_data_t info{};mach_msg_type_number_t n=TASK_VM_INFO_COUNT;if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&n))std::abort();return info.phys_footprint/1048576.;}
int main(int argc,char**argv){
 if(argc!=3)return 2;SearchEngine engine;IndexMetadata meta;if(!FlatIndexWriter(argv[1]).load(engine,&meta))return 3;engine.completePhase2();
 const double before=footprint();double peak=before;std::atomic<bool>done{false};
 std::thread sample([&]{while(!done.load()){peak=std::max(peak,footprint());std::this_thread::sleep_for(std::chrono::milliseconds(1));}});
 const auto start=std::chrono::steady_clock::now();auto result=engine.query(argv[2],10000);const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();done=true;sample.join();peak=std::max(peak,footprint());
 uint64_t hash=1469598103934665603ULL;for(auto id:result)hash=(hash^id)*1099511628211ULL;
 std::cout<<"query="<<argv[2]<<" count="<<result.size()<<" hash="<<hash<<" ms="<<ms<<" before_mib="<<before<<" query_peak_mib="<<peak<<" after_mib="<<footprint()<<std::endl;
}
