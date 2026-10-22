#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <mach/mach.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <algorithm>
using Clock=std::chrono::steady_clock;
static double footprint(){task_vm_info_data_t i{};mach_msg_type_number_t n=TASK_VM_INFO_COUNT;if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&i,&n))std::abort();return i.phys_footprint/1048576.0;}
template<class Map> static void lists(const char* label,const Map& map){size_t count=0,cap=0;for(const auto&[key,v]:map){count+=v.size();cap+=v.capacity();}std::cout<<label<<" count="<<map.size()<<" used_mib="<<count*4/1048576.0<<" capacity_mib="<<cap*4/1048576.0<<std::endl;}
int main(int argc,char**argv){
 if(argc!=3)return 2;SearchEngine e;FlatIndexWriter r(argv[1]);IndexMetadata meta;if(!r.load(e,&meta))return 3;e.completePhase2();
 std::cout<<"ready_mib="<<footprint()<<" records="<<e.recordCount()<<" live="<<e.liveRecordCount()<<std::endl;
 lists("name_postings",e.nameTrigramIndex_);lists("path_postings",e.pathTrigramIndex_);lists("extension_postings",e.extensionIndex_);
 for(const char*q:{"readme","*.swift","config","/Applications/","size:>1mb","type:folder"}){auto t=Clock::now();size_t count=0;for(int i=0;i<10;i++)count=e.query(q,100).size();std::cout<<"query="<<q<<" mean_ms="<<std::chrono::duration<double,std::milli>(Clock::now()-t).count()/10<<" count="<<count<<std::endl;}
 const double before=footprint();std::atomic<bool>done{false},monitorDone{false};double peak=before,maxQuery=0;size_t queries=0;std::atomic<int>mutations{0};double maxMutation=0;
 std::thread sample([&]{while(!monitorDone){peak=std::max(peak,footprint());std::this_thread::sleep_for(std::chrono::milliseconds(2));}});
 std::thread query([&]{while(!done){auto t=Clock::now();e.query("readme",100);maxQuery=std::max(maxQuery,std::chrono::duration<double,std::milli>(Clock::now()-t).count());queries++;std::this_thread::sleep_for(std::chrono::milliseconds(5));}});
 std::thread mutate([&]{for(int i=0;i<100;i++){FileRecord rec;rec.name="synthetic-"+std::to_string(i)+".txt";rec.path="/memory-check-two";rec.type=1;rec.modTime=1;auto started=Clock::now();e.addRecord(std::move(rec));maxMutation=std::max(maxMutation,std::chrono::duration<double,std::milli>(Clock::now()-started).count());mutations++;std::this_thread::sleep_for(std::chrono::milliseconds(5));}});
 auto begin=Clock::now();FlatIndexWriter w(argv[2]);bool ok=w.fullRewrite(e,meta);double duration=std::chrono::duration<double>(Clock::now()-begin).count();done=true;query.join();mutate.join();monitorDone=true;sample.join();peak=std::max(peak,footprint());
 std::cout<<"write_ok="<<ok<<" write_seconds="<<duration<<" before_write_mib="<<before<<" peak_mib="<<peak<<" after_write_mib="<<footprint()<<" max_concurrent_query_ms="<<maxQuery<<" queries="<<queries<<" mutations="<<mutations<<" max_mutation_ms="<<maxMutation<<std::endl;
 SearchEngine check;IndexMetadata saved;if(!ok||!FlatIndexWriter(argv[2]).load(check,&saved))return 4;std::cout<<"reload_live="<<check.liveRecordCount()<<std::endl;
}
