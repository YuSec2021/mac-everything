#pragma once
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <mach-o/dyld.h>
#include <unistd.h>

namespace me {
// libmalloc reads MallocLargeCache only at process start. With the cache on,
// freed large blocks from index loading stay in the physical footprint
// (~400 MiB for a 6M-record index). The app bundle sets it via LSEnvironment;
// command-line launches re-exec once with it set. An explicit value is kept.
inline void ensureMallocLargeCacheDisabled(char** argv) {
    if (std::getenv("MallocLargeCache")) return;
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return;
    if (setenv("MallocLargeCache", "0", 1) != 0) return;
    execv(path, argv); // returns only on failure; continue with the default allocator
}
}
