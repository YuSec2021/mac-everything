#pragma once
#include <cstdint>
#include <functional>
#include <string_view>
#include <absl/container/btree_map.h>

namespace me {
// 32-bit keys pack twice as many slots per B-tree node as size_t keys.
// Collisions are expected; every lookup confirms the resolved path.
struct PathHash {
    uint32_t operator()(std::string_view path) const {
        const uint64_t wide = std::hash<std::string_view>{}(path);
        return static_cast<uint32_t>(wide ^ (wide >> 32));
    }
};

// Paths already live in StringPools; retain only a hash and the record index.
class CompactPathIndex : public absl::btree_multimap<uint32_t, uint32_t> {
public:
    // B-tree nodes pack entries together and need no hash-bucket reservation.
    void reserve(size_t) {}
};

template<class Index, class Resolve, class Hash = PathHash>
auto findPathRecord(Index& index, std::string_view path, Resolve resolve, Hash hash = {}) {
    auto [first, last] = index.equal_range(static_cast<uint32_t>(hash(path)));
    for (auto it = first; it != last; ++it) {
        // A hash match is never enough to identify a file.
        if (resolve(it->second) == path) return it;
    }
    return index.end();
}

template<class Resolve, class Hash = PathHash>
void setPathRecord(CompactPathIndex& index, std::string_view path, uint32_t record,
                   Resolve resolve, Hash hash = {}) {
    auto it = findPathRecord(index, path, resolve, hash);
    if (it == index.end()) index.emplace(static_cast<uint32_t>(hash(path)), record);
    else it->second = record;
}
}
