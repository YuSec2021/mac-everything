#pragma once
#include "StringPool.h"

// Most names are already lowercase: both views then address the same bytes.
class FoldedNamePool {
    struct Entry {
        uint32_t original, lower;
        uint16_t originalLength, lowerLength;
    };
    std::vector<char> buffer_;
    std::vector<Entry> entries_;
public:
    void reserve(size_t count, size_t bytes) { entries_.reserve(count); buffer_.reserve(bytes); }
    void clear() { *this = FoldedNamePool{}; }
    uint32_t append(std::string_view original, std::string_view lower) {
        original = original.substr(0, UINT16_MAX);
        lower = lower.substr(0, UINT16_MAX);
        const bool shared = original == lower;
        me::reserveForAppend(buffer_, original.size() + (shared ? 0 : lower.size()));
        const auto offset = static_cast<uint32_t>(buffer_.size());
        buffer_.insert(buffer_.end(), original.begin(), original.end());
        const auto lowerOffset = shared ? offset : static_cast<uint32_t>(buffer_.size());
        if (!shared) buffer_.insert(buffer_.end(), lower.begin(), lower.end());
        const auto id = static_cast<uint32_t>(entries_.size());
        me::reserveForAppend(entries_);
        entries_.push_back({offset, lowerOffset, static_cast<uint16_t>(original.size()), static_cast<uint16_t>(lower.size())});
        return id;
    }
    void tombstone(uint32_t id) { entries_[id].originalLength = entries_[id].lowerLength = 0; }
    void load(const StringPool& original, const StringPool& lower) {
        clear();
        size_t bytes = 0;
        for (uint32_t i = 0; i < original.entryCount(); ++i)
            bytes += original.length(i) + (original.view(i) == lower.view(i) ? 0 : lower.length(i));
        reserve(original.entryCount(), bytes);
        for (uint32_t i = 0; i < original.entryCount(); ++i) append(original.view(i), lower.view(i));
    }
    size_t storageBytes() const { return buffer_.capacity() + entries_.capacity() * sizeof(Entry); }
    class View {
        const FoldedNamePool& pool_;
        bool lower_;
    public:
        View(const FoldedNamePool& pool, bool lower) : pool_(pool), lower_(lower) {}
        uint32_t entryCount() const { return static_cast<uint32_t>(pool_.entries_.size()); }
        uint16_t length(uint32_t id) const {
            const auto& entry = pool_.entries_[id];
            return lower_ ? entry.lowerLength : entry.originalLength;
        }
        const char* data(uint32_t id) const {
            const auto& entry = pool_.entries_[id];
            return pool_.buffer_.empty() ? "" : pool_.buffer_.data() + (lower_ ? entry.lower : entry.original);
        }
        bool isLive(uint32_t id) const { return id < entryCount() && length(id) != 0; }
        std::string_view view(uint32_t id) const { return {data(id), length(id)}; }
        std::string str(uint32_t id) const { return std::string(view(id)); }
        void reconstruct(uint32_t id, std::string& out) const { out.assign(view(id)); }
        const Entry* entries() const { return pool_.entries_.data(); }
        StringPool flatSnapshot() const {
            StringPool flat;
            size_t bytes = 0;
            for (uint32_t i = 0; i < entryCount(); ++i) bytes += length(i);
            flat.reserve(bytes, entryCount());
            for (uint32_t i = 0; i < entryCount(); ++i) flat.append(view(i));
            return flat;
        }
    };
    View original() const { return {*this, false}; }
    View lower() const { return {*this, true}; }
};
