#include "SearchEngine.h"
#include "IndexPersistence.h"
#include "FlatIndexWriter.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mach/mach.h>
#include <unistd.h>
namespace fs = std::filesystem;
#include "tests/test_helpers.h"
#include "tests/test_memory_optimizations.h"
#include "tests/test_record_dedup.h"
#include "tests/test_batch_rescan.h"
#include "tests/test_wal_inplace_replay.h"
#include "tests/test_flat_persistence_v6.h"
#include "tests/test_wal_crc.h"
#include "tests/test_recent_cache.h"
int main() {
    runFlatPersistenceV6Tests();
    runWalCrcTests();
    std::cout << "Passed=" << passed << " Failed=" << failed << std::endl;
    return failed ? 1 : 0;
}
