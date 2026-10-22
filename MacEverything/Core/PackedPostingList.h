#pragma once
#include "VectorGrowth.h"
#include <cassert>
#include <compare>
#include <cstdint>
#include <iterator>

namespace me {
// Sorted IDs use varint deltas. Sparse checkpoints bound seeks; mutations
// rewrite neighboring deltas and adjust the later checkpoints in place.
class PackedPostingList {
    struct Checkpoint { uint32_t rank, offset, previous; };
    struct Position { uint32_t value, previous, start, end; };
    static constexpr uint32_t stride = 128;
    std::vector<uint8_t> bytes_;
    std::vector<Checkpoint> checkpoints_;
    uint32_t count_ = 0, last_ = 0;
    static size_t encode(uint32_t value, uint8_t* out) {
        size_t n = 0;
        do { out[n++] = (value & 127) | (value >= 128 ? 128 : 0); value >>= 7; } while (value);
        return n;
    }
    uint32_t decode(uint32_t& offset) const {
        uint32_t value = 0; unsigned shift = 0; uint8_t byte;
        do { byte = bytes_[offset++]; value |= uint32_t(byte & 127) << shift; shift += 7; } while (byte & 128);
        return value;
    }
    Position locate(uint32_t rank) const {
        assert(rank < count_);
        auto it = std::upper_bound(checkpoints_.begin(), checkpoints_.end(), rank,
                                  [](uint32_t r, const Checkpoint& c) { return r < c.rank; });
        --it;
        uint32_t value = it->previous, offset = it->offset;
        for (uint32_t i = it->rank;; ++i) {
            uint32_t previous = value, start = offset;
            value += decode(offset);
            if (i == rank) return {value, previous, start, offset};
        }
    }
    void boundSeeks() {
        if (checkpoints_.empty() || checkpoints_.front().rank != 0)
            checkpoints_.insert(checkpoints_.begin(), {0, 0, 0});
        for (size_t i = 0; i < checkpoints_.size(); ++i) {
            const uint32_t end = i + 1 < checkpoints_.size() ? checkpoints_[i + 1].rank : count_;
            if (end - checkpoints_[i].rank <= stride * 2) continue;
            const uint32_t rank = checkpoints_[i].rank + stride;
            auto p = locate(rank);
            checkpoints_.insert(checkpoints_.begin() + i + 1, {rank, p.start, p.previous});
        }
    }
    void replace(uint32_t first, uint32_t end, const uint8_t* data, size_t length) {
        if (length > end - first) me::reserveForAppend(bytes_, length - (end - first));
        bytes_.erase(bytes_.begin() + first, bytes_.begin() + end);
        bytes_.insert(bytes_.begin() + first, data, data + length);
    }
public:
    static size_t encodedBytes(uint32_t delta) {
        size_t n = 1; while (delta >= 128) { delta >>= 7; ++n; } return n;
    }
    class const_iterator {
        const PackedPostingList* owner_ = nullptr;
        ptrdiff_t index_ = 0;
        mutable ptrdiff_t cached_ = -2;
        mutable uint32_t value_ = 0, next_ = 0;
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = uint32_t;
        using difference_type = ptrdiff_t;
        using reference = uint32_t;
        using pointer = void;
        const_iterator() = default;
        const_iterator(const PackedPostingList* owner, ptrdiff_t index) : owner_(owner), index_(index) {}
        uint32_t operator*() const {
            if (cached_ == index_) return value_;
            if (cached_ + 1 == index_) value_ += owner_->decode(next_);
            else { auto p = owner_->locate(static_cast<uint32_t>(index_)); value_ = p.value; next_ = p.end; }
            cached_ = index_;
            return value_;
        }
        uint32_t operator[](ptrdiff_t n) const { return *(*this + n); }
        const_iterator& operator++() { ++index_; return *this; }
        const_iterator operator++(int) { auto old = *this; ++*this; return old; }
        const_iterator& operator--() { --index_; return *this; }
        const_iterator operator--(int) { auto old = *this; --*this; return old; }
        const_iterator& operator+=(ptrdiff_t n) { index_ += n; return *this; }
        const_iterator& operator-=(ptrdiff_t n) { index_ -= n; return *this; }
        friend const_iterator operator+(const_iterator i, ptrdiff_t n) { return i += n; }
        friend const_iterator operator+(ptrdiff_t n, const_iterator i) { return i += n; }
        friend const_iterator operator-(const_iterator i, ptrdiff_t n) { return i -= n; }
        friend ptrdiff_t operator-(const_iterator a, const_iterator b) { return a.index_ - b.index_; }
        bool operator==(const const_iterator& other) const { return owner_ == other.owner_ && index_ == other.index_; }
        auto operator<=>(const const_iterator& other) const { return index_ <=> other.index_; }
    };
    size_t size() const { return count_; }
    size_t storageBytes() const { return bytes_.capacity() + checkpoints_.capacity() * sizeof(Checkpoint); }
    bool empty() const { return count_ == 0; }
    uint32_t operator[](size_t i) const { return locate(static_cast<uint32_t>(i)).value; }
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, count_}; }
    void shrink_to_fit() { bytes_.shrink_to_fit(); checkpoints_.shrink_to_fit(); }
    void reserve(size_t count) { bytes_.reserve(count); checkpoints_.reserve((count + stride - 1) / stride); }
    void reserveEncoded(size_t count, size_t bytes) { reserve(count); bytes_.reserve(bytes); }
    void push_back(uint32_t value) {
        assert(empty() || value >= last_);
        if (count_ % stride == 0 && (checkpoints_.empty() || checkpoints_.back().rank < count_))
            checkpoints_.push_back({count_, static_cast<uint32_t>(bytes_.size()), last_});
        uint8_t encoded[5];
        const size_t n = encode(value - last_, encoded);
        me::reserveForAppend(bytes_, n);
        bytes_.insert(bytes_.end(), encoded, encoded + n);
        last_ = value; ++count_;
    }
    void insert(const_iterator position, uint32_t value) {
        const uint32_t rank = static_cast<uint32_t>(position - begin());
        if (rank == count_) { push_back(value); return; }
        auto p = locate(rank);
        assert(p.previous <= value && value <= p.value);
        uint8_t encoded[10];
        const size_t first = encode(value - p.previous, encoded);
        const size_t n = first + encode(p.value - value, encoded + first);
        const int64_t delta = int64_t(n) - (p.end - p.start);
        replace(p.start, p.end, encoded, n);
        for (auto& c : checkpoints_) {
            if (c.rank < rank) continue;
            if (c.rank == rank) { c.offset = p.start + first; c.previous = value; }
            else c.offset = static_cast<uint32_t>(int64_t(c.offset) + delta);
            ++c.rank;
        }
        ++count_; boundSeeks();
    }
    void erase(const_iterator position) {
        const uint32_t rank = static_cast<uint32_t>(position - begin());
        auto p = locate(rank);
        uint8_t encoded[5]; size_t n = 0; uint32_t end = p.end;
        if (rank + 1 < count_) {
            uint32_t next = p.value + decode(end);
            n = encode(next - p.previous, encoded);
        } else last_ = p.previous;
        const int64_t delta = int64_t(n) - (end - p.start);
        replace(p.start, end, encoded, n);
        for (auto& c : checkpoints_) {
            if (c.rank <= rank) continue;
            if (c.rank == rank + 1) { c.offset = p.start; c.previous = p.previous; }
            else c.offset = static_cast<uint32_t>(int64_t(c.offset) + delta);
            --c.rank;
        }
        --count_;
        checkpoints_.erase(std::remove_if(checkpoints_.begin(), checkpoints_.end(), [&](const auto& c) { return c.rank >= count_; }), checkpoints_.end());
        checkpoints_.erase(std::unique(checkpoints_.begin(), checkpoints_.end(), [](const auto& a, const auto& b) { return a.rank == b.rank; }), checkpoints_.end());
    }
};
// Delta insertion reserves the exact encoded growth internally.
inline void reserveForAppend(PackedPostingList&, size_t = 1) {}
}
