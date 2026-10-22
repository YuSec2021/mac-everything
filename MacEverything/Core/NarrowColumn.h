#pragma once
#include "VectorGrowth.h"
#include <absl/container/btree_map.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace me {
// Per-record column stored in a narrower type. The largest narrow value is an
// escape: rows that do not fit keep their exact value in a sparse side map.
template<class Wide, class Narrow>
class NarrowColumn {
public:
    using value_type = Wide;

    NarrowColumn() = default;
    explicit NarrowColumn(const std::vector<Wide>& values) { assign(values); }

    Wide operator[](size_t i) const {
        const Narrow v = values_[i];
        if (v != kEscape) [[likely]] return static_cast<Wide>(v);
        return overflow_.find(static_cast<uint32_t>(i))->second;
    }

    void set(size_t i, Wide v) {
        if (fits(v)) {
            if (values_[i] == kEscape) overflow_.erase(static_cast<uint32_t>(i));
            values_[i] = static_cast<Narrow>(v);
        } else {
            values_[i] = kEscape;
            overflow_[static_cast<uint32_t>(i)] = v;
        }
    }

    void push_back(Wide v) {
        values_.push_back(0);
        set(values_.size() - 1, v);
    }

    void assign(const std::vector<Wide>& values) {
        clear();
        values_.resize(values.size());
        for (size_t i = 0; i < values.size(); ++i) set(i, values[i]);
    }

    void resize(size_t n) {
        overflow_.erase(overflow_.lower_bound(static_cast<uint32_t>(std::min<size_t>(n, UINT32_MAX))),
                        overflow_.end());
        values_.resize(n, 0);
    }

    void reserve(size_t n) { values_.reserve(n); }
    void reserveForAppend(size_t count = 1) { me::reserveForAppend(values_, count); }
    void clear() { values_.clear(); overflow_.clear(); }
    void swap(NarrowColumn& other) noexcept { values_.swap(other.values_); overflow_.swap(other.overflow_); }
    size_t size() const { return values_.size(); }
    bool empty() const { return values_.empty(); }
    size_t overflowCount() const { return overflow_.size(); }
    size_t storageBytes() const {
        return values_.capacity() * sizeof(Narrow) + overflow_.size() * (sizeof(uint32_t) + sizeof(Wide));
    }

private:
    static constexpr Narrow kEscape = std::numeric_limits<Narrow>::max();
    static bool fits(Wide v) {
        if constexpr (std::is_signed_v<Wide>) {
            if (v < 0) return false;
        }
        return static_cast<std::make_unsigned_t<Wide>>(v) < kEscape;
    }

    std::vector<Narrow> values_;
    absl::btree_map<uint32_t, Wide> overflow_;
};

template<class Wide, class Narrow>
void reserveForAppend(NarrowColumn<Wide, Narrow>& column, size_t count = 1) { column.reserveForAppend(count); }
}
