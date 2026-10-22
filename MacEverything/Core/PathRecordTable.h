#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace me {
// Open-addressing table of record IDs with linear probing. Slots hold only the
// 4-byte record ID; callers derive a record's key hash from the record itself,
// so probing and rehashing never build path strings.
//   matches(record) -> bool : record has the key being probed
//   hashOf(record)  -> uint64_t : key hash of a stored record
class PathRecordTable {
public:
    static constexpr uint32_t kNone = UINT32_MAX;
    static constexpr size_t npos = SIZE_MAX;

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    void clear() { std::vector<uint32_t>{}.swap(slots_); count_ = 0; }
    size_t storageBytes() const { return slots_.capacity() * sizeof(uint32_t); }

    // Size for n records with ~10% headroom below the 85% growth threshold;
    // growth adds half the capacity. Compaction rebuilds at the live count.
    template<class HashOf>
    void reserve(size_t n, HashOf hashOf) {
        const size_t want = n + n * 3 / 10 + 16;
        if (want > slots_.size()) rehash(want, hashOf);
    }

    template<class Matches>
    size_t findSlot(uint64_t hash, Matches matches) const {
        if (slots_.empty()) return npos;
        for (size_t i = bucket(hash);; i = next(i)) {
            const uint32_t r = slots_[i];
            if (r == kNone) return npos;
            if (matches(r)) return i;
        }
    }

    template<class Matches>
    uint32_t find(uint64_t hash, Matches matches) const {
        const size_t slot = findSlot(hash, matches);
        return slot == npos ? kNone : slots_[slot];
    }

    size_t slotOf(uint32_t record, uint64_t hash) const {
        return findSlot(hash, [record](uint32_t r) { return r == record; });
    }

    uint32_t at(size_t slot) const { return slots_[slot]; }

    // Insert a record, replacing a stored record with an equal key.
    template<class Matches, class HashOf>
    void assign(uint64_t hash, uint32_t record, Matches matches, HashOf hashOf) {
        if ((count_ + 1) * 20 > slots_.size() * 17) rehash(std::max<size_t>(64, slots_.size() + slots_.size() / 2), hashOf);
        size_t i = bucket(hash);
        for (;; i = next(i)) {
            const uint32_t r = slots_[i];
            if (r == kNone) break;
            if (matches(r)) { slots_[i] = record; return; }
        }
        slots_[i] = record;
        ++count_;
    }

    // Remove the entry at a slot returned by findSlot/slotOf (backward-shift
    // deletion keeps every remaining record reachable from its home bucket).
    template<class HashOf>
    void eraseSlot(size_t slot, HashOf hashOf) {
        size_t hole = slot;
        for (size_t j = next(slot);; j = next(j)) {
            const uint32_t r = slots_[j];
            if (r == kNone) break;
            const size_t home = bucket(hashOf(r));
            const bool reachable = hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
            if (!reachable) {
                slots_[hole] = r;
                hole = j;
            }
        }
        slots_[hole] = kNone;
        --count_;
    }

    template<class Fn>
    void forEach(Fn&& fn) const {
        for (uint32_t r : slots_) if (r != kNone) fn(r);
    }

private:
    size_t bucket(uint64_t hash) const {
        return static_cast<size_t>((static_cast<unsigned __int128>(hash) * slots_.size()) >> 64);
    }
    size_t next(size_t i) const { return ++i == slots_.size() ? 0 : i; }

    template<class HashOf>
    void rehash(size_t capacity, HashOf hashOf) {
        std::vector<uint32_t> old(capacity, kNone);
        old.swap(slots_);
        for (uint32_t r : old) {
            if (r == kNone) continue;
            size_t i = bucket(hashOf(r));
            while (slots_[i] != kNone) i = next(i);
            slots_[i] = r;
        }
    }

    std::vector<uint32_t> slots_;
    size_t count_ = 0;
};
}
