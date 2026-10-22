#include "MallocTuning.h"
#include <mach/mach.h>
#include <cstdio>
#include <cstring>
#include <vector>
static double fp() { task_vm_info_data_t i{}; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
  task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&i, &n); return i.phys_footprint/1048576.0; }
int main(int argc, char** argv) {
  if (argc > 2 && !strcmp(argv[1], "--reexec")) me::ensureMallocLargeCacheDisabled(argv);
  std::vector<void*> v; for (int i=0;i<40;i++){ size_t sz=(1+i%8)*4<<20; void*p=malloc(sz); memset(p,1,sz); v.push_back(p);}
  double a = fp(); for (auto p: v) free(p);
  FILE* out = fopen(argc > 2 ? argv[2] : argv[1], "w");
  fprintf(out, "env=%s allocated=%.0f afterFree=%.0f\n", getenv("MallocLargeCache") ?: "(unset)", a, fp()); fclose(out);
}
