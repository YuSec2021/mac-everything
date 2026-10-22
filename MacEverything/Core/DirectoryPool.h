#pragma once
#include "CompactPathIndex.h"
#include "StringPool.h"
#include "StringUtils.h"
#include "StructuredQueryParser.h"
#include <limits>

// Keep shared directory prefixes as parent links. External path IDs remain stable.
class DirectoryPool {
    struct Node {
        uint32_t parent, offset, lowerOffset, lowerLength, fullLowerLength;
        uint16_t length, fullLength;
    };
    std::vector<Node> nodes_{{0, 0, 0, 0, 0, 0, 0}};
    std::vector<char> components_;
    std::vector<uint32_t> paths_;
    me::CompactPathIndex edges_;
    std::unordered_map<uint32_t, std::string> lowerOverrides_;

    uint32_t child(uint32_t parent, std::string_view component) {
        const size_t key = std::hash<std::string_view>{}(component) ^ (size_t(parent) * 0x9e3779b97f4a7c15ULL);
        auto [first, last] = edges_.equal_range(key);
        for (auto it = first; it != last; ++it) {
            const auto& node = nodes_[it->second];
            if (node.parent == parent && std::string_view(components_.data() + node.offset, node.length) == component)
                return it->second;
        }
        const uint32_t id = static_cast<uint32_t>(nodes_.size());
        const uint32_t offset = static_cast<uint32_t>(components_.size());
        const auto fullLength = static_cast<uint16_t>(nodes_[parent].fullLength + component.size());
        const auto lower = me::toLower(std::string(component));
        const bool same = component == lower;
        me::reserveForAppend(components_, component.size() + (same ? 0 : lower.size()));
        components_.insert(components_.end(), component.begin(), component.end());
        const uint32_t lowerOffset = same ? offset : static_cast<uint32_t>(components_.size());
        if (!same) components_.insert(components_.end(), lower.begin(), lower.end());
        const uint32_t fullLowerLength = nodes_[parent].fullLowerLength + lower.size();
        me::reserveForAppend(nodes_);
        nodes_.push_back({parent, offset, lowerOffset, static_cast<uint32_t>(lower.size()), fullLowerLength,
                          static_cast<uint16_t>(component.size()), fullLength});
        edges_.emplace(key, id);
        return id;
    }
public:
    DirectoryPool() = default;
    explicit DirectoryPool(const StringPool& flat) {
        paths_.reserve(flat.entryCount());
        for (uint32_t i = 0; i < flat.entryCount(); ++i) append(flat.view(i));
    }
    uint32_t append(std::string_view path) {
        path = path.substr(0, UINT16_MAX);
        uint32_t node = 0;
        size_t start = 0;
        if (!path.empty() && path.front() == '/') { node = child(0, "/"); start = 1; }
        while (start < path.size()) {
            size_t end = path.find('/', start + (path[start] == '/'));
            if (end == std::string_view::npos) end = path.size();
            node = child(node, path.substr(start, end - start));
            start = end;
        }
        const auto id = static_cast<uint32_t>(paths_.size());
        me::reserveForAppend(paths_);
        paths_.push_back(node);
        auto lower = me::toLower(std::string(path));
        if (lower.size() > UINT16_MAX) lower.resize(UINT16_MAX);
        std::string derived;
        reconstruct(id, derived, true);
        // Invalid UTF-8 and contextual Unicode casing must retain whole-path semantics.
        if (derived != lower) lowerOverrides_.emplace(id, std::move(lower));
        return id;
    }
    uint32_t entryCount() const { return static_cast<uint32_t>(paths_.size()); }
    uint16_t length(uint32_t id) const { return nodes_[paths_[id]].fullLength; }
    bool isLive(uint32_t id) const { return id < paths_.size() && length(id) != 0; }
    void reconstruct(uint32_t id, std::string& out, bool lower = false) const {
        if (lower && !lowerOverrides_.empty()) {
            auto it = lowerOverrides_.find(id);
            if (it != lowerOverrides_.end()) { out = it->second; return; }
        }
        uint32_t node = paths_[id];
        out.resize(lower ? nodes_[node].fullLowerLength : nodes_[node].fullLength);
        size_t end = out.size();
        while (node) {
            const auto& n = nodes_[node];
            const size_t length = lower ? n.lowerLength : n.length;
            end -= length;
            std::memcpy(out.data() + end, components_.data() + (lower ? n.lowerOffset : n.offset), length);
            node = n.parent;
        }
        if (lower && out.size() > UINT16_MAX) out.resize(UINT16_MAX);
    }
    std::string str(uint32_t id) const { std::string out; reconstruct(id, out); return out; }
    // Owning result: reconstructing another path must never invalidate a caller's value.
    std::string view(uint32_t id) const { return str(id); }
    void clear() { *this = DirectoryPool{}; }
    StringPool flatSnapshot() const {
        StringPool flat;
        for (uint32_t i = 0; i < entryCount(); ++i) flat.append(str(i));
        return flat;
    }
    size_t storageBytes() const {
        return nodes_.capacity() * sizeof(Node) + components_.capacity() + paths_.capacity() * sizeof(uint32_t);
    }
    static bool matchFlat(std::string_view dirPath, const std::vector<PathSegment>& segments) {
        if (segments.empty()) return true;
    
        // Walk backwards without allocating a component vector for every directory.
        int segIdx = static_cast<int>(segments.size()) - 1;
        const int lastSegment = segIdx;
        size_t end = dirPath.size();
        while (segIdx >= 0 && end > 0) {
            while (end > 0 && dirPath[end - 1] == '/') --end;
            if (end == 0) break;
            const auto slash = dirPath.rfind('/', end - 1);
            const size_t start = slash == std::string_view::npos ? 0 : slash + 1;
            const auto component = dirPath.substr(start, end - start);
            if (component.find(segments[segIdx].text) != std::string_view::npos) {
                --segIdx;
            } else if (segIdx < lastSegment && segments[segIdx + 1].adjacentToNext) {
                return false;
            }
            end = start;
        }
    
        return segIdx < 0;
    }
    bool matchesSegments(uint32_t id, const std::vector<PathSegment>& segments) const {
        // Truncated and context-sensitive lowercasing need the exact flattened value.
        if (nodes_[paths_[id]].fullLowerLength > UINT16_MAX || lowerOverrides_.contains(id))
            return matchFlat(lower().str(id), segments);
        int segment = static_cast<int>(segments.size()) - 1;
        const int last = segment;
        for (uint32_t node = paths_[id]; node && segment >= 0; node = nodes_[node].parent) {
            const auto& n = nodes_[node];
            std::string_view component(components_.data() + n.lowerOffset, n.lowerLength);
            if (!component.empty() && component.front() == '/') component.remove_prefix(1);
            if (component.empty()) continue;
            if (component.find(segments[segment].text) != std::string_view::npos) --segment;
            else if (segment < last && segments[segment + 1].adjacentToNext) return false;
        }
        return segment < 0;
    }
    class LowerView {
    public:
        const DirectoryPool& pool;
        explicit LowerView(const DirectoryPool& source) : pool(source) {}
        uint32_t entryCount() const { return pool.entryCount(); }
        uint16_t length(uint32_t id) const {
            if (!pool.lowerOverrides_.empty()) {
                auto it = pool.lowerOverrides_.find(id);
                if (it != pool.lowerOverrides_.end()) return it->second.size();
            }
            return std::min<uint32_t>(UINT16_MAX, pool.nodes_[pool.paths_[id]].fullLowerLength);
        }
        bool isLive(uint32_t id) const { return id < entryCount() && length(id) != 0; }
        void reconstruct(uint32_t id, std::string& out) const { pool.reconstruct(id, out, true); }
        std::string str(uint32_t id) const { std::string out; reconstruct(id, out); return out; }
        std::string view(uint32_t id) const { return str(id); }
        StringPool flatSnapshot() const {
            StringPool flat;
            for (uint32_t i = 0; i < entryCount(); ++i) flat.append(str(i));
            return flat;
        }
    };
    LowerView lower() const { return LowerView(*this); }
    // Each query worker keeps only its last reconstructed parent, never a global path cache.
    class Cursor {
        const DirectoryPool& pool_;
        bool lower_ = false;
        uint32_t previous_ = UINT32_MAX;
        std::string text_;
    public:
        explicit Cursor(const DirectoryPool& pool) : pool_(pool) {}
        explicit Cursor(LowerView view) : pool_(view.pool), lower_(true) {}
        const char* data(uint32_t id) {
            if (previous_ != id) { pool_.reconstruct(id, text_, lower_); previous_ = id; }
            return text_.data();
        }
    };
};
