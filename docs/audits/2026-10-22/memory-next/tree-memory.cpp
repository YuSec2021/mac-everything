#include "SearchEngine.h"
#include "FlatIndexWriter.h"
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <iostream>
double footprint(){task_vm_info_data_t i{};mach_msg_type_number_t n=TASK_VM_INFO_COUNT;task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&i,&n);return i.phys_footprint/1048576.;}
int main(){SearchEngine e;FlatIndexWriter("/tmp/maceverything-memory-2/index.v6").load(e,nullptr);e.completePhase2();std::cout<<"footprint="<<footprint()<<std::endl;for(auto* p:{&e.pathPool_,&e.lowerPathPool_})std::cout<<"tree arrays="<<p->storageBytes()/1048576.<<" nodes="<<p->nodes_.size()<<" components="<<p->components_.size()<<" edges="<<p->edges_.size()<<std::endl;malloc_zone_pressure_relief(nullptr,0);std::cout<<"relieved="<<footprint()<<std::endl;}
