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

// Per-record column with few distinct values (device IDs): one byte per row
// indexes a small dictionary; rows past the dictionary limit use a side map.
template<class Wide>
class DictColumn {
public:
    using value_type = Wide;

    DictColumn() = default;
    explicit DictColumn(const std::vector<Wide>& values) { assign(values); }

    Wide operator[](size_t i) const {
        const uint8_t code = codes_[i];
        if (code != kEscape) [[likely]] return dict_[code];
        return overflow_.find(static_cast<uint32_t>(i))->second;
    }

    void set(size_t i, Wide v) {
        const uint8_t code = codeFor(v);
        if (codes_[i] == kEscape) overflow_.erase(static_cast<uint32_t>(i));
        codes_[i] = code;
        if (code == kEscape) overflow_[static_cast<uint32_t>(i)] = v;
    }

    void push_back(Wide v) {
        codes_.push_back(0);
        set(codes_.size() - 1, v);
    }

    void assign(const std::vector<Wide>& values) {
        clear();
        codes_.resize(values.size());
        for (size_t i = 0; i < values.size(); ++i) set(i, values[i]);
    }

    void resize(size_t n) {
        overflow_.erase(overflow_.lower_bound(static_cast<uint32_t>(std::min<size_t>(n, UINT32_MAX))),
                        overflow_.end());
        const size_t old = codes_.size();
        codes_.resize(n, 0);
        for (size_t i = old; i < n; ++i) set(i, Wide{});
    }

    void reserve(size_t n) { codes_.reserve(n); }
    void reserveForAppend(size_t count = 1) { me::reserveForAppend(codes_, count); }
    void clear() { codes_.clear(); dict_.clear(); overflow_.clear(); }
    void swap(DictColumn& other) noexcept {
        codes_.swap(other.codes_); dict_.swap(other.dict_); overflow_.swap(other.overflow_);
    }
    size_t size() const { return codes_.size(); }
    bool empty() const { return codes_.empty(); }
    size_t dictionarySize() const { return dict_.size(); }
    size_t overflowCount() const { return overflow_.size(); }
    size_t storageBytes() const {
        return codes_.capacity() + dict_.capacity() * sizeof(Wide)
            + overflow_.size() * (sizeof(uint32_t) + sizeof(Wide));
    }

private:
    static constexpr uint8_t kEscape = 0xFF;

    uint8_t codeFor(Wide v) {
        // Consecutive rows usually share a device, so check the last hit first.
        if (last_ < dict_.size() && dict_[last_] == v) return last_;
        auto it = std::find(dict_.begin(), dict_.end(), v);
        if (it == dict_.end()) {
            if (dict_.size() == kEscape) return kEscape;
            dict_.push_back(v);
            it = dict_.end() - 1;
        }
        last_ = static_cast<uint8_t>(it - dict_.begin());
        return last_;
    }

    std::vector<uint8_t> codes_;
    std::vector<Wide> dict_;
    absl::btree_map<uint32_t, Wide> overflow_;
    uint8_t last_ = 0;
};

template<class Wide, class Narrow>
void reserveForAppend(NarrowColumn<Wide, Narrow>& column, size_t count = 1) { column.reserveForAppend(count); }

template<class Wide>
void reserveForAppend(DictColumn<Wide>& column, size_t count = 1) { column.reserveForAppend(count); }
}
