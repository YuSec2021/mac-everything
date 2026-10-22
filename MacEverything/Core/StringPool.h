#pragma once
#include "VectorGrowth.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <memory>
#include <utility>

/// Contiguous memory pool for strings.
/// All strings are stored end-to-end in a single char buffer.
/// Each string is referenced by an Entry {offset, length}.
/// Supports append, tombstone (length=0), and compaction.
class StringPool {
public:
    struct Entry {
        uint32_t offset;
        uint16_t length;
    };

    StringPool() = default;
    StringPool(StringPool&& other) noexcept { *this = std::move(other); }
    StringPool& operator=(StringPool&& other) noexcept {
        if (this == &other) return *this;
        buffer_ = std::move(other.buffer_);
        entries_ = std::move(other.entries_);
        backing_ = std::move(other.backing_);
        external_ = std::exchange(other.external_, nullptr);
        externalSize_ = std::exchange(other.externalSize_, 0);
        return *this;
    }
    StringPool(const StringPool&) = default;
    StringPool& operator=(const StringPool&) = default;

    /// Append a string to the pool. Returns the entry index.
    uint32_t append(const char* data, uint16_t len) {
        std::string saved;
        const auto source = reinterpret_cast<uintptr_t>(data);
        const auto begin = reinterpret_cast<uintptr_t>(rawBuffer());
        if (source >= begin && source - begin < rawSize()) {
            saved.assign(data, len);
            data = saved.data();
        }
        ownBuffer();
        uint32_t idx = static_cast<uint32_t>(entries_.size());
        uint32_t off = static_cast<uint32_t>(buffer_.size());
        me::reserveForAppend(buffer_, len);
        me::reserveForAppend(entries_);
        buffer_.insert(buffer_.end(), data, data + len);
        entries_.push_back({off, len});
        return idx;
    }

    /// Append from C string literal.
    uint32_t append(const char* s) {
        size_t len = std::strlen(s);
        return append(s, static_cast<uint16_t>(std::min<size_t>(len, UINT16_MAX)));
    }

    /// Append from std::string.
    uint32_t append(const std::string& s) {
        return append(s.data(), static_cast<uint16_t>(std::min<size_t>(s.size(), UINT16_MAX)));
    }

    /// Append from string_view.
    uint32_t append(std::string_view sv) {
        return append(sv.data(), static_cast<uint16_t>(std::min<size_t>(sv.size(), UINT16_MAX)));
    }

    /// Mark entry as tombstoned. Buffer space is not reclaimed.
    void tombstone(uint32_t idx) {
        if (idx < entries_.size()) {
            entries_[idx].length = 0;
        }
    }

    /// Pointer to the string data for entry idx.
    const char* data(uint32_t idx) const {
        return rawBuffer() + entries_[idx].offset;
    }

    /// Length of entry idx (0 if tombstoned).
    uint16_t length(uint32_t idx) const {
        return entries_[idx].length;
    }

    /// Whether entry idx is live (not tombstoned).
    bool isLive(uint32_t idx) const {
        return idx < entries_.size() && entries_[idx].length > 0;
    }

    /// Get a string_view for entry idx.
    std::string_view view(uint32_t idx) const {
        return {data(idx), entries_[idx].length};
    }

    /// Get a std::string copy for entry idx.
    std::string str(uint32_t idx) const {
        return std::string(view(idx));
    }

    /// Raw contiguous buffer pointer. All strings are packed here.
    const char* rawBuffer() const { return external_ ? external_ : (buffer_.empty() ? "" : buffer_.data()); }

    /// Total size of the raw buffer in bytes.
    size_t rawSize() const { return external_ ? externalSize_ : buffer_.size(); }

    /// Pointer to entries array.
    const Entry* entries() const { return entries_.data(); }

    /// Number of entries (including tombstoned).
    uint32_t entryCount() const { return static_cast<uint32_t>(entries_.size()); }

    /// Pre-allocate buffer and entry storage.
    void reserve(size_t totalBytes, size_t entryCount) {
        ownBuffer();
        buffer_.reserve(totalBytes);
        entries_.reserve(entryCount);
    }

    /// Bulk-load from a vector of strings. Clears existing content.
    void loadBulk(const std::vector<std::string>& strings) {
        clear();
        size_t totalLen = 0;
        for (const auto& s : strings) totalLen += s.size();
        buffer_.reserve(totalLen);
        entries_.reserve(strings.size());
        for (const auto& s : strings) {
            append(s);
        }
    }

    /// Construct pool directly from raw binary data (copy-based bulk load).
    /// Used by v5 persistence to restore a StringPool from on-disk buffer+entries.
    void loadRaw(const char* buffer, size_t bufferSize,
                 const Entry* entries, uint32_t entryCount) {
        std::vector<char> bytes(buffer, buffer + bufferSize);
        std::vector<Entry> offsets(entries, entries + entryCount);
        loadRaw(std::move(bytes), std::move(offsets));
    }

    /// Move-based bulk load (zero-copy from pre-read vectors).
    /// Used by v6 persistence for direct I/O without intermediate copies.
    void loadRaw(std::vector<char>&& buffer, std::vector<Entry>&& entries) {
        clear();
        buffer_ = std::move(buffer);
        entries_ = std::move(entries);
    }

    // The validated v6 input stays file-backed only while converting to compact pools.
    void loadMappedRaw(const char* buffer, size_t size, std::vector<Entry>&& entries,
                       std::shared_ptr<void> backing) {
        clear();
        external_ = buffer;
        externalSize_ = size;
        backing_ = std::move(backing);
        entries_ = std::move(entries);
    }

    /// Clear all entries and buffer.
    void clear() {
        buffer_.clear();
        entries_.clear();
        external_ = nullptr;
        externalSize_ = 0;
        backing_.reset();
    }

    /// Create a compacted copy containing only live entries matching liveMask.
    /// Defined after class body (CompactResult uses StringPool by value).
    struct CompactResult;
    CompactResult compact(const std::vector<bool>& liveMask) const;

private:
    std::vector<char> buffer_;
    std::vector<Entry> entries_;
    const char* external_ = nullptr;
    size_t externalSize_ = 0;
    std::shared_ptr<void> backing_;
    void ownBuffer() {
        if (!external_) return;
        buffer_.assign(external_, external_ + externalSize_);
        external_ = nullptr;
        externalSize_ = 0;
        backing_.reset();
    }
};

/// Result of compaction — defined outside class so StringPool is complete.
struct StringPool::CompactResult {
    StringPool compacted;
    std::unordered_map<uint32_t, uint32_t> remap; // old index -> new index
};

inline StringPool::CompactResult StringPool::compact(const std::vector<bool>& liveMask) const {
    CompactResult result;
    size_t liveBytes = 0;
    size_t liveCount = 0;
    for (uint32_t i = 0; i < entries_.size(); i++) {
        if (i < liveMask.size() && liveMask[i] && entries_[i].length > 0) {
            liveBytes += entries_[i].length;
            liveCount++;
        }
    }
    result.compacted.reserve(liveBytes, liveCount);
    for (uint32_t i = 0; i < entries_.size(); i++) {
        if (i < liveMask.size() && liveMask[i] && entries_[i].length > 0) {
            uint32_t newIdx = result.compacted.append(
                data(i), entries_[i].length);
            result.remap[i] = newIdx;
        }
    }
    return result;
}
