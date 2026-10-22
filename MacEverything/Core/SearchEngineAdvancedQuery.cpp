#include "SearchEngine.h"
#include "StringUtils.h"
#include "QueryParser.h"
#include "QueryTokenizer.h"
#include "QueryFilterParser.h"
#include "QueryNeedsAnalysis.h"
#include "ASTStructuredTransform.h"
#include "ASTGlobTransform.h"
#include "CompiledGlob.h"
#include "SIMDSearch.h"
#include "Logger.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <re2/re2.h>
#include <re2/filtered_re2.h>
#include <functional>
#include <thread>
#include <dispatch/dispatch.h>

// ---------------------------------------------------------------------------
// queryAdvanced: evaluate a parsed AST against the record set.
//
// Strategy:
// 1. Parse the query into an AST.
// 2. Extract best TERM for trigram pre-filtering.
// 3. Evaluate AST per-record: TERM=substring/glob, FILTER=structured filters,
//    AND/OR/NOT=boolean logic.
// 4. Score, sort, and return results.
//
// Supported FILTER types: ext:, size:, file:, folder:, type:, path:, nopath:,
// parent:, depth:, len:
// ---------------------------------------------------------------------------

namespace {

/// Compare a numeric value against a filter's parsed operator and thresholds.
static bool compareNumeric(uint64_t val, CompareOp op, uint64_t v1, uint64_t v2) {
    switch (op) {
        case CompareOp::EQ:    return val == v1;
        case CompareOp::LT:    return val < v1;
        case CompareOp::LE:    return val <= v1;
        case CompareOp::GT:    return val > v1;
        case CompareOp::GE:    return val >= v1;
        case CompareOp::RANGE: return val >= v1 && val <= v2;
    }
    return false;
}

/// Extract lowercase file extension from name data.
/// Returns empty string_view if no extension found.
static std::string getExtLower(const char* nameData, uint16_t nameLen) {
    // Find last '.'
    int dotPos = -1;
    for (int i = static_cast<int>(nameLen) - 1; i >= 0; --i) {
        if (nameData[i] == '.') { dotPos = i; break; }
    }
    if (dotPos < 0 || dotPos == static_cast<int>(nameLen) - 1) return {};
    // nameData is already lowercase
    return std::string(nameData + dotPos + 1, nameLen - dotPos - 1);
}

/// Count '/' in path to determine depth.
static uint64_t computeDepth(const char* pathData, uint16_t pathLen) {
    uint64_t depth = 0;
    for (uint16_t i = 0; i < pathLen; ++i) {
        if (pathData[i] == '/') depth++;
    }
    return depth;
}

// globMatchImpl is now in CompiledGlob.h

/// Check if a character is a word boundary character (not alphanumeric).
/// Note: underscore '_' IS a boundary (consistent with Everything behavior).
static bool isWordBoundaryChar(char c) {
    return !std::isalnum(static_cast<unsigned char>(c));
}

/// Check if a substring match at position `pos` of length `len` within `text`
/// (total length `textLen`) is a whole-word match.
static bool isWholeWordMatch(const char* text, size_t textLen,
                             size_t pos, size_t len) {
    // Check left boundary: must be start of string or preceded by non-word char
    if (pos > 0 && !isWordBoundaryChar(text[pos - 1])) return false;
    // Check right boundary: must be end of string or followed by non-word char
    size_t end = pos + len;
    if (end < textLen && !isWordBoundaryChar(text[end])) return false;
    return true;
}

/// Pre-compiled regex cache, keyed by QueryNode pointer.
/// Uses RE2 for guaranteed linear-time matching (no backtracking).
using RegexCache = std::unordered_map<const QueryNode*, std::unique_ptr<re2::RE2>>;

/// Collect all REGEX TERM nodes from the AST for pre-compilation.
static void collectRegexNodes(const QueryNode& node, std::vector<const QueryNode*>& out) {
    if (node.type == QueryNodeType::TERM && node.mode == MatchMode::REGEX) {
        out.push_back(&node);
    }
    for (auto& child : node.children) {
        collectRegexNodes(*child, out);
    }
}

/// Clone RegexCache for each thread to avoid RE2 DFA mutex contention.
/// RE2's lazy DFA construction uses internal mutexes; sharing one RE2 object
/// across threads causes severe contention (measured: 12T 6x slower than 1T).
/// Per-thread clones eliminate this, yielding ~3.8x MT speedup.
static std::vector<RegexCache> cloneRegexCachePerThread(
    const RegexCache& src, unsigned numThreads) {
    std::vector<RegexCache> perThread(numThreads);
    if (src.empty()) return perThread;
    for (unsigned t = 0; t < numThreads; t++) {
        for (const auto& [nodePtr, re] : src) {
            re2::RE2::Options opts;
            opts.set_case_sensitive(re->options().case_sensitive());
            opts.set_log_errors(false);
            perThread[t].emplace(nodePtr,
                std::make_unique<re2::RE2>(re->pattern(), opts));
        }
    }
    return perThread;
}

/// Evaluate a FILTER node against a single record (SoA fields).
/// When called from the pure-filter SoA fast path, nameData and pathData are nullptr —
/// string-based filters short-circuit (return true) since isPureFilter() guarantees
/// the query contains no string filters.
static bool evalFilter(const QueryNode& node,
                       uint8_t recType, uint64_t recSize, time_t recModTime,
                       const char* nameData, uint16_t nameLen,
                       const char* pathData, uint16_t pathLen,
                       std::vector<char>& pathBuf) {
    const auto& name = node.filterName;

    // __pathseg: internal filter — structured path segment matching
    if (name == "__pathseg") {
        if (!nameData || !pathData) return true;
        // Directory constraints must not match the filename component itself.
        return SearchEngine::pathSegmentsMatch(std::string_view(pathData, pathLen), node.pathSegments);
    }

    // ext: — match file extension (requires name data)
    if (name == "ext") {
        if (!nameData) return true;
        std::string ext = getExtLower(nameData, nameLen);
        if (ext.empty()) return false;
        for (auto& e : node.extList) {
            if (ext == e) return true;
        }
        return false;
    }

    // size: — compare file size
    if (name == "size") {
        return compareNumeric(recSize, node.op, node.numVal1, node.numVal2);
    }

    // file: — match files only
    if (name == "file") {
        return recType == 1; // 1=file
    }

    // folder: — match directories only
    if (name == "folder") {
        return recType == 2; // 2=dir
    }

    // type: — shorthand for file/folder
    if (name == "type") {
        if (node.filterArg == "file") return recType == 1;
        if (node.filterArg == "folder" || node.filterArg == "dir") return recType == 2;
        return false;
    }

    // path: — path must contain substring (case insensitive, requires path data)
    if (name == "path") {
        if (!nameData || !pathData) return true;
        // Build full path: pathData + '/' + nameData
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        return me::simdContains(pathBuf.data(), fullLen,
                                node.filterArg.data(), node.filterArg.size());
    }

    // nopath: — path must NOT contain substring (requires path data)
    if (name == "nopath") {
        if (!nameData || !pathData) return true;
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        return !me::simdContains(pathBuf.data(), fullLen,
                                 node.filterArg.data(), node.filterArg.size());
    }

    // parent: — direct parent path must match exactly (requires path data)
    if (name == "parent") {
        if (!pathData) return true;
        // filterArg is lowercased; pathData is already lowercased
        std::string pathStr(pathData, pathLen);
        return pathStr == node.filterArg;
    }

    // depth: — directory depth (count of '/' in full path, requires path data)
    if (name == "depth") {
        if (!pathData) return true;
        uint64_t depth = computeDepth(pathData, pathLen);
        return compareNumeric(depth, node.op, node.numVal1, node.numVal2);
    }

    // len: — filename length (requires name data)
    if (name == "len") {
        if (!nameData) return true;
        return compareNumeric(static_cast<uint64_t>(nameLen), node.op,
                              node.numVal1, node.numVal2);
    }

    // dm: / datemodified: — modification date filter
    if (name == "dm" || name == "datemodified") {
        return compareNumeric(static_cast<uint64_t>(recModTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // dc: / datecreated: — creation date (uses modTime as fallback since
    // FileRecord doesn't store birthtime yet)
    if (name == "dc" || name == "datecreated") {
        return compareNumeric(static_cast<uint64_t>(recModTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // da: / dateaccessed: — access date (uses modTime as fallback)
    if (name == "da" || name == "dateaccessed") {
        return compareNumeric(static_cast<uint64_t>(recModTime), node.op,
                              node.numVal1, node.numVal2);
    }

    // Unknown filter — pass through
    return true;
}

/// Evaluate a TERM node against a single record.
/// Returns true if the record's name or full path matches the term text.
/// nameData is lowercase (from names_.lower()), origNameData has original case (from names_.original()).
/// pathData is lowercase, origPathData has original case (from pathPool_).
/// Returns false when nameData is nullptr (pure-filter SoA fast path).
static bool evalTerm(const QueryNode& node,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     const char* origNameData, uint16_t origNameLen,
                     const char* origPathData, uint16_t origPathLen,
                     std::vector<char>& pathBuf,
                     const RegexCache& regexCache) {
    if (!nameData) return false; // SoA pure-filter path — no string data available
    switch (node.mode) {

    case MatchMode::SUBSTRING: {
        if (node.caseSensitive) {
            // Case-sensitive: compare against original-case name
            const auto& term = node.text;
            // Check name
            if (origNameLen >= term.size()) {
                for (size_t i = 0; i + term.size() <= origNameLen; ++i) {
                    if (memcmp(origNameData + i, term.data(), term.size()) == 0)
                        return true;
                }
            }
            // Check full path (original case: origPath + "/" + origName)
            if (node.nameOnly) return false;
            size_t fullLen = static_cast<size_t>(origPathLen) + 1 + origNameLen;
            if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
            memcpy(pathBuf.data(), origPathData, origPathLen);
            pathBuf[origPathLen] = '/';
            memcpy(pathBuf.data() + origPathLen + 1, origNameData, origNameLen);
            if (fullLen >= term.size()) {
                for (size_t i = 0; i + term.size() <= fullLen; ++i) {
                    if (memcmp(pathBuf.data() + i, term.data(), term.size()) == 0)
                        return true;
                }
            }
            return false;
        }
        // Case-insensitive (default): use lowercase nameData
        const auto& lower = node.textLower;
        if (me::simdContains(nameData, nameLen, lower.data(), lower.size())) {
            return true;
        }
        // Skip full-path matching when nameOnly is set
        // (transformSlashTerms uses this for name-component terms)
        if (node.nameOnly) return false;
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        return me::simdContains(pathBuf.data(), fullLen, lower.data(), lower.size());
    }

    case MatchMode::GLOB: {
        const auto& pattern = node.textLower;
        // If pattern contains '/', match against full path; otherwise name only
        if (pattern.find('/') != std::string::npos) {
            size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
            if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
            memcpy(pathBuf.data(), pathData, pathLen);
            pathBuf[pathLen] = '/';
            memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
            if (node.compiledGlob) {
                return compiledGlobMatch(*node.compiledGlob, pathBuf.data(), fullLen);
            }
            return globMatchImpl(pattern, pathBuf.data(), fullLen);
        }
        // Name-only glob: use pre-compiled fast path (zero-alloc)
        if (node.compiledGlob) {
            return compiledGlobMatch(*node.compiledGlob, nameData, nameLen);
        }
        return globMatchImpl(pattern, nameData, nameLen);
    }

    case MatchMode::REGEX: {
        auto it = regexCache.find(&node);
        if (it == regexCache.end()) return false;
        // Match against name using RE2 zero-copy StringPiece (no per-record allocation)
        re2::StringPiece sp(nameData, nameLen);
        if (RE2::PartialMatch(sp, *it->second)) return true;
        // If not nameOnly, also try full path match
        if (node.nameOnly) return false;
        size_t fullLen = static_cast<size_t>(pathLen) + 1 + nameLen;
        if (pathBuf.size() < fullLen) pathBuf.resize(fullLen * 2);
        memcpy(pathBuf.data(), pathData, pathLen);
        pathBuf[pathLen] = '/';
        memcpy(pathBuf.data() + pathLen + 1, nameData, nameLen);
        re2::StringPiece fullSp(pathBuf.data(), fullLen);
        return RE2::PartialMatch(fullSp, *it->second);
    }

    case MatchMode::WHOLEWORD: {
        const auto& lower = node.textLower;
        // Check in lowercase name
        std::string name(nameData, nameLen);
        for (size_t i = 0; i + lower.size() <= name.size(); ++i) {
            if (memcmp(name.data() + i, lower.data(), lower.size()) == 0) {
                if (isWholeWordMatch(name.data(), name.size(), i, lower.size()))
                    return true;
            }
        }
        return false;
    }

    case MatchMode::WHOLEFILENAME: {
        const auto& lower = node.textLower;
        // Entire filename must match
        if (nameLen == lower.size() &&
            memcmp(nameData, lower.data(), nameLen) == 0)
            return true;
        return false;
    }

    } // switch
    return false;
}

/// Recursively evaluate AST node against a single record.
static bool evalNode(const QueryNode& node,
                     uint8_t recType, uint64_t recSize, time_t recModTime,
                     const char* nameData, uint16_t nameLen,
                     const char* pathData, uint16_t pathLen,
                     const char* origNameData, uint16_t origNameLen,
                     const char* origPathData, uint16_t origPathLen,
                     std::vector<char>& pathBuf,
                     const RegexCache& regexCache) {
    switch (node.type) {
        case QueryNodeType::TERM:
            return evalTerm(node, nameData, nameLen, pathData, pathLen,
                            origNameData, origNameLen, origPathData, origPathLen,
                            pathBuf, regexCache);

        case QueryNodeType::AND:
            for (auto& child : node.children) {
                if (!evalNode(*child, recType, recSize, recModTime,
                              nameData, nameLen, pathData, pathLen,
                              origNameData, origNameLen, origPathData, origPathLen,
                              pathBuf, regexCache))
                    return false;
            }
            return true;

        case QueryNodeType::OR:
            for (auto& child : node.children) {
                if (evalNode(*child, recType, recSize, recModTime,
                             nameData, nameLen, pathData, pathLen,
                             origNameData, origNameLen, origPathData, origPathLen,
                             pathBuf, regexCache))
                    return true;
            }
            return false;

        case QueryNodeType::NOT:
            if (node.children.empty()) return true;
            return !evalNode(*node.children[0], recType, recSize, recModTime,
                             nameData, nameLen, pathData, pathLen,
                             origNameData, origNameLen, origPathData, origPathLen,
                             pathBuf, regexCache);

        case QueryNodeType::FILTER:
            return evalFilter(node, recType, recSize, recModTime,
                              nameData, nameLen, pathData, pathLen, pathBuf);
    }
    return false;
}

/// Extract all TERM texts from the AST (for trigram pre-filtering).
static void collectTerms(const QueryNode& node, std::vector<std::string>& terms) {
    switch (node.type) {
        case QueryNodeType::TERM:
            terms.push_back(node.textLower);
            break;
        case QueryNodeType::AND:
        case QueryNodeType::OR:
            for (auto& child : node.children) collectTerms(*child, terms);
            break;
        case QueryNodeType::NOT:
            if (!node.children.empty()) collectTerms(*node.children[0], terms);
            break;
        case QueryNodeType::FILTER:
            break;
    }
}

/// Find the single best (longest) SUBSTRING TERM from the AND-level for trigram pre-filtering.
/// For OR queries we can't pre-filter (any branch might match), so return empty.
/// Skips __pathseg filter nodes and recurses into AND children.
/// Extract the best trigram key from a TERM node (SUBSTRING or GLOB mode).
static std::string termTrigramKey(const QueryNode& node) {
    if (node.mode == MatchMode::SUBSTRING) return node.textLower;
    if (node.mode == MatchMode::GLOB) {
        auto segs = extractLiteralSegments(node.textLower);
        std::string best;
        for (const auto& s : segs)
            if (s.size() >= 3 && s.size() > best.size()) best = s;
        return best;
    }
    return {};
}

static std::string bestTrigramTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM) {
        return termTrigramKey(node);
    }
    if (node.type == QueryNodeType::AND) {
        std::string best;
        for (auto& child : node.children) {
            if (child->type == QueryNodeType::TERM) {
                std::string t = termTrigramKey(*child);
                if (t.size() > best.size()) best = t;
            } else if (child->type == QueryNodeType::AND) {
                std::string t = bestTrigramTerm(*child);
                if (t.size() > best.size()) best = t;
            }
        }
        return best;
    }
    // OR, NOT, FILTER — can't reliably pre-filter
    return {};
}

/// Extract literal atoms from a regex pattern using RE2's FilteredRE2.
/// Returns lowercased, deduplicated atoms (min length 3) that RE2's own
/// AST parser determines are useful for pre-filtering.
/// Correctly handles alternation, character classes, and all regex constructs.
static std::vector<std::string> extractRegexLiteralsImpl(const std::string& pattern) {
    re2::FilteredRE2 fre2(3);  // min_atom_len = 3 (matches trigram minimum)
    re2::RE2::Options opts;
    opts.set_case_sensitive(false);
    opts.set_log_errors(false);
    int id;
    if (fre2.Add(pattern, opts, &id) != re2::RE2::NoError) {
        return {};
    }
    std::vector<std::string> atoms;
    fre2.Compile(&atoms);
    return atoms;
}

/// Collect regex literal substrings from all REGEX TERM nodes in the AST.
static std::vector<std::string> extractRegexLiteralsFromAST(const QueryNode& node) {
    std::vector<std::string> allLiterals;
    std::function<void(const QueryNode&)> collect = [&](const QueryNode& n) {
        if (n.type == QueryNodeType::TERM && n.mode == MatchMode::REGEX) {
            auto lits = extractRegexLiteralsImpl(n.text);
            allLiterals.insert(allLiterals.end(), lits.begin(), lits.end());
        }
        for (auto& child : n.children) collect(*child);
    };
    collect(node);
    return allLiterals;
}


/// Extract the first SUBSTRING TERM text from the AST for result scoring.
/// Returns empty string if no suitable term is found (pure filter queries).
/// Skips __pathseg filter nodes (they are path constraints, not scoring terms).
static std::string extractScoringTerm(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM && node.mode == MatchMode::SUBSTRING) {
        return node.textLower;
    }
    if (node.type == QueryNodeType::FILTER && node.filterName == "__pathseg") {
        return {}; // path segment constraint, not a scoring term
    }
    if (node.type == QueryNodeType::AND || node.type == QueryNodeType::OR) {
        for (auto& child : node.children) {
            auto t = extractScoringTerm(*child);
            if (!t.empty()) return t;
        }
    }
    return {};
}

static bool needsOriginalPath(const QueryNode& node) {
    if (node.type == QueryNodeType::TERM && node.mode == MatchMode::SUBSTRING && node.caseSensitive && !node.nameOnly) return true;
    for (const auto& child : node.children) if (needsOriginalPath(*child)) return true;
    return false;
}

// Only positive AND-level constraints can safely narrow the whole query.
static void requiredPathConstraints(const QueryNode& node, std::vector<const QueryNode*>& out) {
    if (node.type == QueryNodeType::FILTER && node.filterName == "__pathseg") {
        out.push_back(&node);
    } else if (node.type == QueryNodeType::AND) {
        for (const auto& child : node.children) requiredPathConstraints(*child, out);
    }
}

/// Extract extension filter values from AND-level ext: FILTER nodes in the AST.
/// Returns the collected extList values for the first ext: filter found at AND level.
/// For OR queries or deeply nested ext:, returns empty (can't pre-filter safely).
static std::vector<std::string> extractExtFilterValues(const QueryNode& node) {
    if (node.type == QueryNodeType::FILTER && node.filterName == "ext") {
        return node.extList;
    }
    if (node.type == QueryNodeType::AND) {
        for (auto& child : node.children) {
            if (child->type == QueryNodeType::FILTER && child->filterName == "ext") {
                return child->extList;
            }
        }
    }
    return {};
}

} // anonymous namespace

// Expose extractRegexLiterals for unit testing
namespace me_test {
    std::vector<std::string> extractRegexLiterals(const std::string& pattern) {
        return extractRegexLiteralsImpl(pattern);
    }
}

std::vector<uint32_t> SearchEngine::queryAdvanced(const std::string& input,
                                                   uint32_t maxResults,
                                                   bool useTrigram,
                                                   QueryTimingInfo& timing,
                                                   uint64_t myGen,
                                                   const std::atomic<uint64_t>* genPtr) const {
    // If no external generation pointer, create a local dummy (no cancellation)
    std::atomic<uint64_t> dummyGen{myGen};
    if (!genPtr) genPtr = &dummyGen;

    auto queryStart = std::chrono::steady_clock::now();

    // Parse the AST
    auto ast = QueryParser::parse(input);
    if (!ast) return {};

    // Transform TERM nodes containing '/' into structured path-segment constraints.
    // e.g. TERM("/usr/local/test") → AND(FILTER("__pathseg",[usr,local]), TERM("test"))
    ast = transformSlashTerms(std::move(ast));

    // Transform TERM nodes containing '*' or '?' from SUBSTRING to GLOB mode.
    ast = transformGlobTerms(std::move(ast));

    // Analyze which record fields the query actually needs
    QueryNeeds needs = analyzeQueryNeeds(*ast);
    const bool originalPathNeeded = needsOriginalPath(*ast);

    // Pre-compile all regex patterns before acquiring lock
    RegexCache regexCache;
    {
        std::vector<const QueryNode*> regexNodes;
        collectRegexNodes(*ast, regexNodes);
        for (auto* rn : regexNodes) {
            re2::RE2::Options opts;
            opts.set_case_sensitive(rn->caseSensitive);
            opts.set_log_errors(false);
            auto re = std::make_unique<re2::RE2>(rn->text, opts);
            if (re->ok()) {
                regexCache.emplace(rn, std::move(re));
            }
            // Invalid regex — evalTerm will return false for this node
        }
    }

    auto beforeLock = std::chrono::steady_clock::now();
    std::shared_lock lock(mutex_);
    auto afterLock = std::chrono::steady_clock::now();

    if (types_.empty()) return {};
    size_t totalSize = types_.size();
    auto beforeTrigram = afterLock, afterTrigram = afterLock;

    // --- Trigram pre-filtering: competitive selection among name, regex, and path trigrams ---
    std::string trigramKey = bestTrigramTerm(*ast);
    std::vector<uint32_t> candidates;
    bool useTrigramIndex = false;

    // Stage 1: Name trigram — stash result, don't commit yet
    std::vector<uint32_t> nameCands;
    bool nameOk = false;
    bool stage1AllFound = false;
    size_t stage1RawCandCount = 0;
    if (useTrigram && !trigramKey.empty() && trigramKey.size() >= 3 && !nameTrigramIndex_.empty()) {
        beforeTrigram = std::chrono::steady_clock::now();
        nameCands = intersectPostingLists(nameTrigramIndex_, trigramKey, stage1AllFound);
        stage1RawCandCount = nameCands.size();
        nameOk = stage1AllFound && nameCands.size() <= totalSize / 10;
        if (!nameOk) nameCands.clear();
        afterTrigram = std::chrono::steady_clock::now();
    }

    // Diagnostic: log why trigram Stage 1 was skipped or failed (for 3+ char queries)
    if (trigramKey.size() >= 3 && !nameOk) {
        bool p2 = phase2Pending_.load(std::memory_order_acquire);
        bool idxEmpty = nameTrigramIndex_.empty();
        LOG_INFO("SearchEngine", "DIAG-TRIGRAM key=\"" << trigramKey
            << "\" SKIP reason="
            << (!useTrigram ? "useTrigram=false" :
                idxEmpty ? "indexEmpty" :
                !stage1AllFound ? "allFound=false" :
                stage1RawCandCount > totalSize / 10 ? "tooManyCands" : "unknown")
            << " phase2Pending=" << p2
            << " indexEmpty=" << idxEmpty
            << " indexBuckets=" << nameTrigramIndex_.size()
            << " allFound=" << stage1AllFound
            << " rawCands=" << stage1RawCandCount
            << " threshold=" << totalSize / 10
            << " totalSize=" << totalSize);
    }

    // Stage 2: Regex trigram (only if name failed)
    // Uses UNION (not AND) across atoms because FilteredRE2 atoms may have
    // OR semantics (e.g. alternation `foo|bar` → atoms are OR-related).
    if (useTrigram && !nameOk && !nameTrigramIndex_.empty()) {
        auto regexLiterals = extractRegexLiteralsFromAST(*ast);
        if (!regexLiterals.empty()) {
            beforeTrigram = std::chrono::steady_clock::now();
            nameCands = unionPostingListsMulti(nameTrigramIndex_, regexLiterals);
            nameOk = !nameCands.empty() && nameCands.size() <= totalSize / 10;
            if (!nameOk) nameCands.clear();
            afterTrigram = std::chrono::steady_clock::now();
        }
    }

    // Stage 4: Extension index pre-filtering
    std::vector<uint32_t> extCands;
    bool extOk = false;
    {
        auto extValues = extractExtFilterValues(*ast);
        if (!extValues.empty()) {
            // Union posting lists for all requested extensions
            for (const auto& ext : extValues) {
                auto it = extensionIndex_.find(ext);
                if (it != extensionIndex_.end()) {
                    extCands.insert(extCands.end(), it->second.begin(), it->second.end());
                }
            }
            if (extValues.size() > 1) {
                std::sort(extCands.begin(), extCands.end());
                extCands.erase(std::unique(extCands.begin(), extCands.end()), extCands.end());
            }
            extOk = true; // Extension index is precise — empty means no matches
        }
    }

    // Without a selective name/extension index, scan directories once instead of
    // matching the same parent path separately for millions of records.
    std::vector<uint32_t> pathCands;
    bool pathOk = false;
    bool usedPathScan = false;
    if (useTrigram && !nameOk && !extOk && !phase2Pending_.load(std::memory_order_acquire)
        && !pathIdxToRecords_.empty()) {
        std::vector<const QueryNode*> constraints;
        requiredPathConstraints(*ast, constraints);
        if (!constraints.empty()) {
            beforeTrigram = std::chrono::steady_clock::now();
            pathOk = true;
            for (uint32_t pi = 0; pi < pathPool_.lower().entryCount(); ++pi) {
                if (genPtr->load(std::memory_order_relaxed) != myGen) return {};
                if (!pathPool_.lower().isLive(pi)) continue;
                bool matches = true;
                for (const auto* constraint : constraints) {
                    if (!pathPool_.matchesSegments(pi, constraint->pathSegments)) { matches = false; break; }
                }
                if (!matches) continue;
                pathIdxToRecords_.forEach(pi, types_, [&](uint32_t idx) { pathCands.push_back(idx); });
                // Broad paths are cheaper to evaluate directly and need no candidate copy.
                if (pathCands.size() > totalSize / 4) {
                    pathOk = false;
                    pathCands.clear();
                    break;
                }
            }
            afterTrigram = std::chrono::steady_clock::now();
        }
    }

    // Stage 5: Pick best — fewer candidates wins
    // Collect all viable candidate sets and pick the smallest
    struct CandidateSet {
        std::vector<uint32_t>* cands;
        const char* label;
    };
    std::vector<CandidateSet> viable;
    if (nameOk) viable.push_back({&nameCands, "name"});
    if (pathOk) viable.push_back({&pathCands, "path"});
    if (extOk)  viable.push_back({&extCands,  "ext"});

    if (!viable.empty()) {
        // Find smallest
        auto best = &viable[0];
        for (size_t i = 1; i < viable.size(); i++) {
            if (viable[i].cands->size() < best->cands->size()) {
                best = &viable[i];
            }
        }
        usedPathScan = std::string(best->label) == "path";
        candidates = std::move(*best->cands);
        // If the winner is not name trigram, clear trigramKey for label purposes
        if (std::string(best->label) != "name") {
            trigramKey.clear();
        }
        useTrigramIndex = true;
    }

    // Evaluate AST against each candidate (or all records for linear scan)
    // Uses SearchEngine::Match (declared in SearchEngine.h)
    std::vector<Match> merged;

    // Extract scoring term from AST (not raw input) — hoisted out of per-record loop
    std::string scoringTerm = extractScoringTerm(*ast);

    // Only retain each worker's best K references. Unlimited queries keep every match.
    struct RankedMatches {
        std::vector<Match>& values;
        size_t limit;
        bool ordered = false;
        static bool less(const Match& a, const Match& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            if (a.pathLen != b.pathLen) return a.pathLen < b.pathLen;
            return a.idx < b.idx;
        }
        bool full() const { return limit && values.size() >= limit; }
        void push_back(Match match) {
            if (!limit || ordered) { values.push_back(match); return; }
            if (values.size() < limit) {
                values.push_back(match);
                std::push_heap(values.begin(), values.end(), less);
            } else if (less(match, values.front())) {
                std::pop_heap(values.begin(), values.end(), less);
                values.back() = match;
                std::push_heap(values.begin(), values.end(), less);
            }
        }
    };

    const auto* astPtr = ast.get();
    const auto lowerNames = names_.lower();
    const auto originalNames = names_.original();
    const auto lowerPaths = pathPool_.lower();
    const bool singleSubstring = astPtr->type == QueryNodeType::TERM && astPtr->mode == MatchMode::SUBSTRING;
    const bool pureFilter = !useTrigramIndex && needs.isPureFilter();
    const uint32_t* ids = useTrigramIndex ? candidates.data() : nullptr;
    const size_t workCount = useTrigramIndex ? candidates.size() : totalSize;
    const unsigned numThreads = workCount < 10000 ? 1 : std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
    const size_t chunkSize = (workCount + numThreads - 1) / numThreads;
    __block std::vector<std::vector<Match>> threadResults(numThreads);
    auto perThreadCaches = numThreads > 1 ? cloneRegexCachePerThread(regexCache, numThreads) : std::vector<RegexCache>{};
    const RegexCache* caches = numThreads > 1 ? perThreadCaches.data() : &regexCache;

    // Select the single-condition loop once per worker, outside its record loop.
    auto scanRecords = [&](size_t start, size_t end, RankedMatches& out, const RegexCache& cache) {
        DirectoryPool::Cursor lowerParent(lowerPaths), originalParent(pathPool_);
        std::vector<char> buffer;
        auto scan = [&]<bool Simple, bool CaseSensitive>() {
            for (size_t i = start; i < end; ++i) {
                if (((i - start) & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != myGen) return;
                if (i + 8 < end) {
                    uint32_t future = ids ? ids[i + 8] : static_cast<uint32_t>(i + 8);
                    __builtin_prefetch(types_.data() + future, 0, 0);
                    __builtin_prefetch(lowerNames.entries() + future, 0, 0);
                }
                const uint32_t idx = ids ? ids[i] : static_cast<uint32_t>(i);
                if (types_[idx] == 0) continue;
                const auto name = lowerNames.view(idx);
                const uint32_t parent = pathIndices_[idx];
                const uint16_t pathLength = lowerPaths.length(parent);
                if constexpr (Simple) {
                    const auto comparedName = CaseSensitive ? originalNames.view(idx) : name;
                    const auto& term = CaseSensitive ? astPtr->text : astPtr->textLower;
                    bool matched = (CaseSensitive && term.empty()) ||
                        me::simdContains(comparedName.data(), comparedName.size(), term.data(), term.size());
                    if (!matched && !astPtr->nameOnly) {
                        const char* path = CaseSensitive ? originalParent.data(parent) : lowerParent.data(parent);
                        const uint16_t length = CaseSensitive ? pathPool_.length(parent) : pathLength;
                        const size_t fullLength = buildFullPathBuf(buffer, path, length, comparedName.data(), comparedName.size());
                        matched = me::simdContains(buffer.data(), fullLength, term.data(), term.size());
                    }
                    if (!matched) continue;
                } else {
                    const char* path = needs.needsPath ? lowerParent.data(parent) : "";
                    const char* originalPath = originalPathNeeded ? originalParent.data(parent) : "";
                    if (!evalNode(*astPtr, types_[idx], sizes_[idx], static_cast<time_t>(modTimes_[idx]),
                                  name.data(), name.size(), path, pathLength,
                                  originalNames.data(idx), originalNames.length(idx), originalPath, pathPool_.length(parent),
                                  buffer, cache)) continue;
                }
                uint8_t priority = 2;
                if (!scoringTerm.empty()) {
                    priority = me::simdContains(name.data(), name.size(), scoringTerm.data(), scoringTerm.size())
                        ? namePriority(name.data(), name.size(), scoringTerm.data(), scoringTerm.size()) : 3;
                }
                out.push_back({idx, priority, static_cast<uint32_t>(pathLength + 1 + name.size())});
            }
        };
        if (!singleSubstring) scan.template operator()<false, false>();
        else if (astPtr->caseSensitive) scan.template operator()<true, true>();
        else scan.template operator()<true, false>();
    };

    auto beforePhase = std::chrono::steady_clock::now();
    auto worker = ^(size_t t) {
        const size_t start = t * chunkSize, end = std::min(start + chunkSize, workCount);
        RankedMatches local{threadResults[t], maxResults, pureFilter};
        if (!pureFilter) { scanRecords(start, end, local, caches[t]); return; }
        std::vector<char> buffer;
        // Pure metadata matches rank by record ID, so later hits cannot beat the first K.
        for (size_t i = start; i < end;) {
            if (((i - start) & 1023) == 0 && genPtr->load(std::memory_order_relaxed) != myGen) return;
            uint16_t live = i + 16 <= end ? me::simdTypeLive16(types_.data() + i) : (types_[i] ? 1 : 0);
            const size_t width = i + 16 <= end ? 16 : 1;
            while (live) {
                const size_t idx = i + __builtin_ctz(live);
                if (evalNode(*astPtr, types_[idx], sizes_[idx], static_cast<time_t>(modTimes_[idx]),
                             nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, buffer, caches[t])) {
                    local.push_back({static_cast<uint32_t>(idx), 2, 0});
                    if (local.full()) return;
                }
                live &= live - 1;
            }
            i += width;
        }
    };
    if (numThreads == 1) worker(0);
    else dispatch_apply(numThreads, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), worker);
    if (genPtr->load(std::memory_order_relaxed) != myGen) return {};
    size_t retained = 0;
    for (const auto& local : threadResults) retained += local.size();
    merged.reserve(retained);
    for (const auto& local : threadResults) merged.insert(merged.end(), local.begin(), local.end());

    auto afterPhase = std::chrono::steady_clock::now();

    // Release lock before sorting
    auto beforeUnlock = std::chrono::steady_clock::now();
    lock.unlock();

    // Sort by priority, then path length
    auto beforeSort = std::chrono::steady_clock::now();
    auto cmp = RankedMatches::less;

    size_t resultCount = merged.size();
    if (maxResults > 0 && resultCount > maxResults) resultCount = maxResults;

    if (resultCount < merged.size()) {
        std::partial_sort(merged.begin(), merged.begin() + resultCount, merged.end(), cmp);
    } else {
        std::sort(merged.begin(), merged.end(), cmp);
    }
    auto afterSort = std::chrono::steady_clock::now();

    std::vector<uint32_t> result;
    result.reserve(resultCount);
    for (size_t i = 0; i < resultCount; i++) {
        result.push_back(merged[i].idx);
    }

    // Populate timing
    auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
    timing.totalMs = toMs(std::chrono::steady_clock::now() - queryStart);
    timing.lockWaitMs = toMs(afterLock - beforeLock);
    timing.lockHeldMs = toMs(beforeUnlock - afterLock);
    timing.sortMs = toMs(afterSort - beforeSort);
    timing.trigramMs = toMs(afterTrigram - beforeTrigram);
    timing.phase1Ms = 0;
    timing.phase2Ms = toMs(afterPhase - beforePhase);
    timing.totalRecords = totalSize;
    timing.candidates = candidates.size();
    timing.nameMatches = 0;
    timing.pathMatches = 0;
    timing.resultCount = result.size();
    timing.usedTrigram = useTrigramIndex && !usedPathScan;
    if (useTrigramIndex) {
        if (usedPathScan) {
            timing.searchPath = "advanced-path-scan";
        } else if (!trigramKey.empty()) {
            timing.searchPath = "advanced-trigram";
        } else if (extOk && !extractExtFilterValues(*ast).empty()) {
            timing.searchPath = "advanced-ext-index";
        } else {
            timing.searchPath = "advanced-regex-trigram";
        }
    } else if (needs.isPureFilter()) {
        timing.searchPath = "advanced-pure-filter-soa-gcd";
    } else {
        timing.searchPath = "advanced-linear-gcd";
    }

    auto ms = static_cast<long long>(timing.totalMs);
    if (ms > 100) {
        LOG_INFO("SearchEngine", "QueryAdvanced \"" << input << "\" total=" << ms
            << "ms | path=" << timing.searchPath
            << " candidates=" << timing.candidates
            << " results=" << timing.resultCount
            << " totalRecords=" << timing.totalRecords);
    }

    return result;
}
