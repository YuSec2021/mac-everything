#pragma once
#include "VectorGrowth.h"
#include <absl/container/btree_map.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace me {
// Directory -> record IDs. Scans write records directory by directory, so a
// built map stores runs of consecutive IDs in CSR form. Records appended later
// extend a directory's last run or go to its tail list. Removal is lazy: the
// visitor skips records whose type is 0, so IDs stay ascending per directory.
class DirRecordMap {
public:
    struct Run { uint32_t start, length; };

    DirRecordMap() = default;

    static DirRecordMap build(const std::vector<uint8_t>& types,
                              const std::vector<uint32_t>& pathIndices, uint32_t dirCount) {
        DirRecordMap map;
        map.built_ = true;
        map.firstRun_.assign(size_t(dirCount) + 1, 0);
        // Count runs per directory, then place them (records ascend within each).
        std::vector<uint32_t> lastRecord(dirCount, UINT32_MAX);
        for (uint32_t i = 0; i < types.size(); ++i) {
            if (types[i] == 0 || pathIndices[i] >= dirCount) continue;
            const uint32_t dir = pathIndices[i];
            if (lastRecord[dir] == UINT32_MAX || lastRecord[dir] + 1 != i) ++map.firstRun_[dir + 1];
            lastRecord[dir] = i;
        }
        for (uint32_t d = 0; d < dirCount; ++d) map.firstRun_[d + 1] += map.firstRun_[d];
        map.runs_.resize(map.firstRun_[dirCount]);
        std::vector<uint32_t> next(map.firstRun_.begin(), map.firstRun_.end() - 1);
        std::fill(lastRecord.begin(), lastRecord.end(), UINT32_MAX);
        for (uint32_t i = 0; i < types.size(); ++i) {
            if (types[i] == 0 || pathIndices[i] >= dirCount) continue;
            const uint32_t dir = pathIndices[i];
            if (lastRecord[dir] != UINT32_MAX && lastRecord[dir] + 1 == i) ++map.runs_[next[dir] - 1].length;
            else map.runs_[next[dir]++] = {i, 1};
            lastRecord[dir] = i;
        }
        return map;
    }

    bool empty() const { return !built_; }

    // Records must be appended in increasing ID order.
    void append(uint32_t dir, uint32_t record) {
        built_ = true;
        auto tail = tails_.find(dir);
        if (tail == tails_.end() && dir + 1 < firstRun_.size() && firstRun_[dir] != firstRun_[dir + 1]) {
            Run& last = runs_[firstRun_[dir + 1] - 1];
            if (last.start + last.length == record) { ++last.length; return; }
        }
        auto& list = tail == tails_.end() ? tails_[dir] : tail->second;
        me::reserveForAppend(list);
        list.push_back(record);
    }

    // Visit live records of a directory in ascending ID order.
    template<class Fn>
    void forEach(uint32_t dir, const std::vector<uint8_t>& types, Fn&& fn) const {
        if (dir + 1 < firstRun_.size()) {
            for (uint32_t r = firstRun_[dir]; r < firstRun_[dir + 1]; ++r) {
                for (uint32_t i = runs_[r].start, end = i + runs_[r].length; i < end; ++i)
                    if (types[i] != 0) fn(i);
            }
        }
        if (auto tail = tails_.find(dir); tail != tails_.end()) {
            for (uint32_t i : tail->second)
                if (types[i] != 0) fn(i);
        }
    }

    // Live records of a directory, ascending, for loops that exit early.
    std::vector<uint32_t> collect(uint32_t dir, const std::vector<uint8_t>& types) const {
        std::vector<uint32_t> records;
        forEach(dir, types, [&](uint32_t i) { records.push_back(i); });
        return records;
    }

    size_t runCount() const { return runs_.size(); }
    size_t storageBytes() const {
        size_t bytes = firstRun_.capacity() * sizeof(uint32_t) + runs_.capacity() * sizeof(Run)
            + tails_.size() * (sizeof(uint32_t) + sizeof(std::vector<uint32_t>));
        for (const auto& [_, list] : tails_) bytes += list.capacity() * sizeof(uint32_t);
        return bytes;
    }

private:
    bool built_ = false;
    std::vector<uint32_t> firstRun_;  // dir -> first run; firstRun_[dir + 1] ends it
    std::vector<Run> runs_;
    absl::btree_map<uint32_t, std::vector<uint32_t>> tails_;
};
}
