#include "FlatIndexWriter.h"
#include "Logger.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

FlatIndexWriter::FlatIndexWriter(const std::string& path) : path_(path) {}

bool FlatIndexWriter::exists() const {
    struct stat st;
    return stat(path_.c_str(), &st) == 0 && st.st_size > 0;
}

// ---------------------------------------------------------------------------
// StringPool section helpers
// ---------------------------------------------------------------------------

bool FlatIndexWriter::writeStringPoolSection(FILE* f, const StringPool& pool,
                                              uint32_t& outSize, uint32_t& outCRC) {
    uint32_t bufSize = static_cast<uint32_t>(pool.rawSize());
    uint32_t entryCount = pool.entryCount();
    outCRC = 0;
    auto write = [&](const void* data, size_t bytes) {
        if (bytes && fwrite(data, 1, bytes, f) != bytes) return false;
        outCRC = IndexWAL::crc32(data, bytes, outCRC);
        return true;
    };
    const size_t entryBytes = size_t(entryCount) * sizeof(StringPool::Entry);
    if (!write(&bufSize, sizeof(bufSize)) || !write(pool.rawBuffer(), bufSize) ||
        !write(&entryCount, sizeof(entryCount)) || !write(pool.entries(), entryBytes)) return false;
    outSize = static_cast<uint32_t>(sizeof(bufSize) + bufSize + sizeof(entryCount) + entryBytes);

    return true;
}

template<class Pool>
static bool writeFlattenedPool(FILE* f, const Pool& pool, uint32_t& outSize, uint32_t& outCRC) {
    uint64_t bytes = 0;
    const uint32_t count = pool.entryCount();
    for (uint32_t i = 0; i < count; ++i) bytes += pool.length(i);
    const uint64_t total = 8 + bytes + uint64_t(count) * sizeof(StringPool::Entry);
    if (total > UINT32_MAX) return false;
    const auto bufSize = static_cast<uint32_t>(bytes);
    outCRC = 0;
    auto write = [&](const void* data, size_t n) {
        if (n && fwrite(data, 1, n, f) != n) return false;
        outCRC = IndexWAL::crc32(data, n, outCRC);
        return true;
    };
    if (!write(&bufSize, sizeof(bufSize))) return false;
    // Preserve the v6 flat format without materializing a second full path pool.
    std::string chunk, path;
    for (uint32_t i = 0; i < count; ++i) {
        pool.reconstruct(i, path);
        chunk.append(path);
        if (chunk.size() >= 65536) {
            if (!write(chunk.data(), chunk.size())) return false;
            chunk.clear();
        }
    }
    if (!write(chunk.data(), chunk.size()) || !write(&count, sizeof(count))) return false;
    std::vector<StringPool::Entry> entries;
    entries.reserve(4096);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < count; ++i) {
        StringPool::Entry entry{};
        entry.offset = offset;
        entry.length = pool.length(i);
        offset += entry.length;
        entries.push_back(entry);
        if (entries.size() == 4096) {
            if (!write(entries.data(), entries.size() * sizeof(StringPool::Entry))) return false;
            entries.clear();
        }
    }
    if (!write(entries.data(), entries.size() * sizeof(StringPool::Entry))) return false;
    outSize = static_cast<uint32_t>(total);
    return true;
}

bool FlatIndexWriter::writeStringPoolSection(FILE* f, const DirectoryPool& pool, uint32_t& size, uint32_t& crc) {
    return writeFlattenedPool(f, pool, size, crc);
}
bool FlatIndexWriter::writeStringPoolSection(FILE* f, FoldedNamePool::View pool, uint32_t& size, uint32_t& crc) {
    return writeFlattenedPool(f, pool, size, crc);
}

bool FlatIndexWriter::writeStringPoolSection(FILE* f, DirectoryPool::LowerView pool, uint32_t& size, uint32_t& crc) {
    return writeFlattenedPool(f, pool, size, crc);
}

bool FlatIndexWriter::readStringPoolSection(const uint8_t* data, size_t len, StringPool& pool) {
    if (len < 8) return false;  // minimum: bufSize(4) + entryCount(4)

    size_t pos = 0;
    uint32_t bufSize;
    memcpy(&bufSize, data + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    if (pos + bufSize > len) return false;
    const char* buffer = reinterpret_cast<const char*>(data + pos);
    pos += bufSize;

    if (pos + sizeof(uint32_t) > len) return false;
    uint32_t entryCount;
    memcpy(&entryCount, data + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    size_t entryBytes = entryCount * sizeof(StringPool::Entry);
    if (pos + entryBytes > len) return false;
    const StringPool::Entry* entries = reinterpret_cast<const StringPool::Entry*>(data + pos);

    pool.loadRaw(buffer, bufSize, entries, entryCount);
    return true;
}

// ---------------------------------------------------------------------------
// Array section helpers
// ---------------------------------------------------------------------------

template<typename T>
bool FlatIndexWriter::writeArraySection(FILE* f, const std::vector<T>& vec,
                                         uint32_t& outSize, uint32_t& outCRC) {
    size_t bytes = vec.size() * sizeof(T);
    outSize = static_cast<uint32_t>(bytes);
    if (bytes > 0 && fwrite(vec.data(), 1, bytes, f) != bytes) return false;
    outCRC = (bytes > 0)
        ? IndexWAL::crc32(reinterpret_cast<const uint8_t*>(vec.data()), bytes)
        : IndexWAL::crc32(nullptr, 0);
    return true;
}

template<typename Column>
bool FlatIndexWriter::writeColumnSection(FILE* f, const Column& column,
                                          uint32_t& outSize, uint32_t& outCRC) {
    using T = typename Column::value_type;
    std::vector<T> chunk;
    chunk.reserve(std::min<size_t>(column.size(), 8192));
    outSize = static_cast<uint32_t>(column.size() * sizeof(T));
    outCRC = 0;
    for (size_t start = 0; start < column.size(); start += chunk.capacity()) {
        const size_t end = std::min(column.size(), start + chunk.capacity());
        chunk.clear();
        for (size_t i = start; i < end; ++i) chunk.push_back(column[i]);
        const size_t bytes = chunk.size() * sizeof(T);
        if (fwrite(chunk.data(), 1, bytes, f) != bytes) return false;
        outCRC = IndexWAL::crc32(chunk.data(), bytes, outCRC);
    }
    if (column.size() == 0) outCRC = IndexWAL::crc32(nullptr, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Metadata section helpers
// ---------------------------------------------------------------------------

bool FlatIndexWriter::writeMetadataSection(FILE* f, const IndexMetadata& meta,
                                            uint32_t& outSize, uint32_t& outCRC) {
    outSize = 0;
    outCRC = 0;
    auto write = [&](const void* data, size_t bytes) {
        if (bytes && fwrite(data, 1, bytes, f) != bytes) return false;
        outCRC = IndexWAL::crc32(data, bytes, outCRC);
        outSize += static_cast<uint32_t>(bytes);
        return true;
    };
    uint32_t count = static_cast<uint32_t>(meta.extra.size());
    if (!write(&count, sizeof(count))) return false;
    for (const auto& [key, value] : meta.extra) {
        uint32_t kLen = static_cast<uint32_t>(key.size());
        uint32_t vLen = static_cast<uint32_t>(value.size());
        if (!write(&kLen, sizeof(kLen)) || !write(key.data(), kLen) ||
            !write(&vLen, sizeof(vLen)) || !write(value.data(), vLen)) return false;
    }

    return true;
}

bool FlatIndexWriter::readMetadataSection(const uint8_t* data, size_t len, IndexMetadata& meta) {
    if (len < sizeof(uint32_t)) return false;
    size_t pos = 0;

    uint32_t count;
    memcpy(&count, data + pos, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    for (uint32_t i = 0; i < count; i++) {
        if (pos + sizeof(uint32_t) > len) return false;
        uint32_t kLen;
        memcpy(&kLen, data + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        if (pos + kLen > len) return false;
        std::string key(reinterpret_cast<const char*>(data + pos), kLen);
        pos += kLen;

        if (pos + sizeof(uint32_t) > len) return false;
        uint32_t vLen;
        memcpy(&vLen, data + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        if (pos + vLen > len) return false;
        std::string value(reinterpret_cast<const char*>(data + pos), vLen);
        pos += vLen;

        meta.extra[std::move(key)] = std::move(value);
    }
    return true;
}

// ---------------------------------------------------------------------------
// fullRewrite — serialize all SoA columns to v6 file
// ---------------------------------------------------------------------------

bool FlatIndexWriter::fullRewrite(SearchEngine& engine, const IndexMetadata& meta) {
    auto snap = engine.readViewForV6();

    std::string tmpPath = path_ + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "w+b");
    if (!f) {
        LOG_ERROR("FlatIndexWriter", "Cannot open " << tmpPath << " for writing");
        return false;
    }

    // Bypass page cache to avoid evicting hot search data
    fcntl(fileno(f), F_NOCACHE, 1);

    // Write placeholder header + section table (will be filled later)
    Header header{};
    memcpy(header.magic, kMagic, 4);
    header.version = kVersion;
    header.recordCount = snap.origNamePool.entryCount();
    header.liveCount = snap.liveCount;
    header.timestamp = meta.timestamp > 0 ? meta.timestamp : static_cast<int64_t>(time(nullptr));
    header.lastEventId = meta.lastEventId;
    header.sectionCount = kSectionCount;

    if (fwrite(&header, sizeof(Header), 1, f) != 1) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    // Reserve space for section table
    SectionEntry sections[kSectionCount] = {};
    long sectionTablePos = ftell(f);
    if (fwrite(sections, sizeof(SectionEntry), kSectionCount, f) != kSectionCount) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    // Helper to fill a section entry
    auto fillSection = [&](uint32_t idx, uint32_t sectionId, uint64_t offset,
                           uint32_t byteLen, uint32_t crc) {
        sections[idx].sectionId = sectionId;
        sections[idx].reserved = 0;
        sections[idx].offset = offset;
        sections[idx].byteLength = byteLen;
        sections[idx].crc32 = crc;
    };

    bool ok = true;

    // Section 1: NAMES_ORIG
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeStringPoolSection(f, snap.origNamePool, size, crc);
        fillSection(0, kSectionNamesOrig, offset, size, crc);
    }

    // Section 2: NAMES_LOWER
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeStringPoolSection(f, snap.namePool, size, crc);
        fillSection(1, kSectionNamesLower, offset, size, crc);
    }

    // Section 3: PATH_POOL
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeStringPoolSection(f, snap.pathPool, size, crc);
        fillSection(2, kSectionPathPool, offset, size, crc);
    }

    // Section 4: LOWER_PATH_POOL
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeStringPoolSection(f, snap.lowerPathPool, size, crc);
        fillSection(3, kSectionLowerPathPool, offset, size, crc);
    }

    // Section 5: PATH_INDICES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeArraySection(f, snap.pathIndices, size, crc);
        fillSection(4, kSectionPathIndices, offset, size, crc);
    }

    // Section 6: TYPES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeArraySection(f, snap.types, size, crc);
        fillSection(5, kSectionTypes, offset, size, crc);
    }

    // Section 7: SIZES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeColumnSection(f, snap.sizes, size, crc);
        fillSection(6, kSectionSizes, offset, size, crc);
    }

    // Section 8: MOD_TIMES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeColumnSection(f, snap.modTimes, size, crc);
        fillSection(7, kSectionModTimes, offset, size, crc);
    }

    // Section 9: INODES
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeArraySection(f, snap.inodes, size, crc);
        fillSection(8, kSectionInodes, offset, size, crc);
    }

    // Section 10: DEV_IDS
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeColumnSection(f, snap.devIds, size, crc);
        fillSection(9, kSectionDevIds, offset, size, crc);
    }

    // No engine data is read after this point; let queued file events proceed.
    snap.lock.unlock();
    snap.mutationGate.unlock();

    // Section 11: METADATA_KV
    {
        uint64_t offset = static_cast<uint64_t>(ftell(f));
        uint32_t size, crc;
        ok = ok && writeMetadataSection(f, meta, size, crc);
        fillSection(10, kSectionMetadataKV, offset, size, crc);
    }

    if (!ok) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    // Write section table back
    fseek(f, sectionTablePos, SEEK_SET);
    if (fwrite(sections, sizeof(SectionEntry), kSectionCount, f) != kSectionCount) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    // Compute and write header CRC (over first 60 bytes, excluding headerCRC field)
    fseek(f, 0, SEEK_SET);
    uint8_t headerBytes[kHeaderSize];
    if (fread(headerBytes, 1, kHeaderSize, f) != kHeaderSize) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }
    // headerCRC is at offset 44 (after magic(4)+version(4)+recordCount(4)+liveCount(4)
    //   +timestamp(8)+lastEventId(8)+sectionCount(4) = 36, then headerCRC at 40)
    // Actually: 4+4+4+4+8+8+4 = 36, headerCRC at offset 36, reserved at 40
    uint32_t hdrCRC = IndexWAL::crc32(headerBytes, 36);
    header.headerCRC = hdrCRC;
    fseek(f, 0, SEEK_SET);
    if (fwrite(&header, sizeof(Header), 1, f) != 1) {
        fclose(f); remove(tmpPath.c_str()); return false;
    }

    const bool synced = fsync(fileno(f)) == 0;
    const int closeResult = fclose(f);
    if (!synced || closeResult != 0) {
        remove(tmpPath.c_str());
        return false;
    }

    if (rename(tmpPath.c_str(), path_.c_str()) != 0) {
        remove(tmpPath.c_str());
        return false;
    }

    LOG_INFO("FlatIndexWriter", "v6 written: " << header.recordCount << " records ("
             << header.liveCount << " live), " << kSectionCount << " sections");
    return true;
}

// ---------------------------------------------------------------------------
// load — deserialize v6 file into SearchEngine via loadRecordsV6()
// ---------------------------------------------------------------------------

bool FlatIndexWriter::load(SearchEngine& engine, IndexMetadata* outMeta) {
    FILE* f = fopen(path_.c_str(), "rb");
    if (!f) return false;

    // Read header
    Header header{};
    if (fread(&header, sizeof(Header), 1, f) != 1) {
        fclose(f); return false;
    }

    // Verify magic
    if (memcmp(header.magic, kMagic, 4) != 0) {
        fclose(f); return false;
    }

    // Verify version
    if (header.version != kVersion) {
        LOG_ERROR("FlatIndexWriter", "Unknown v6 version: " << header.version);
        fclose(f); return false;
    }

    // Verify header CRC
    {
        uint8_t headerBytes[kHeaderSize];
        fseek(f, 0, SEEK_SET);
        if (fread(headerBytes, 1, kHeaderSize, f) != kHeaderSize) {
            fclose(f); return false;
        }
        uint32_t expectedCRC = IndexWAL::crc32(headerBytes, 36);
        if (header.headerCRC != expectedCRC) {
            LOG_ERROR("FlatIndexWriter", "Header CRC mismatch");
            fclose(f); return false;
        }
    }

    // Reject impossible section tables before allocating buffers from disk lengths.
    struct stat fileInfo{};
    if (fstat(fileno(f), &fileInfo) != 0 || header.sectionCount > kSectionCount ||
        fileInfo.st_size < static_cast<off_t>(kHeaderSize + header.sectionCount * sizeof(SectionEntry))) {
        fclose(f); return false;
    }

    // Read section table
    fseek(f, kHeaderSize, SEEK_SET);
    std::vector<SectionEntry> sections(header.sectionCount);
    if (fread(sections.data(), sizeof(SectionEntry), header.sectionCount, f)
        != header.sectionCount) {
        fclose(f); return false;
    }

    for (const auto& section : sections) {
        if (section.offset > static_cast<uint64_t>(fileInfo.st_size) ||
            section.byteLength > static_cast<uint64_t>(fileInfo.st_size) - section.offset) {
            fclose(f); return false;
        }
    }

    // Build section lookup (sectionId -> index in sections array)
    uint32_t sectionIdx[kSectionCount + 1];
    memset(sectionIdx, 0xFF, sizeof(sectionIdx));
    for (uint32_t i = 0; i < header.sectionCount; i++) {
        uint32_t id = sections[i].sectionId;
        if (id <= kSectionCount) sectionIdx[id] = i;
    }

    // Helper: read a section from file, verify CRC, return raw bytes
    auto readSectionRaw = [&](uint32_t sectionId, std::vector<uint8_t>& out) -> bool {
        if (sectionIdx[sectionId] == 0xFFFFFFFF) return false;
        const auto& entry = sections[sectionIdx[sectionId]];
        out.resize(entry.byteLength);
        fseek(f, static_cast<long>(entry.offset), SEEK_SET);
        if (fread(out.data(), 1, entry.byteLength, f) != entry.byteLength) {
            LOG_ERROR("FlatIndexWriter", "Failed to read section " << sectionId);
            return false;
        }
        uint32_t actualCRC = IndexWAL::crc32(out.data(), out.size());
        if (actualCRC != entry.crc32) {
            LOG_ERROR("FlatIndexWriter", "CRC mismatch for section " << sectionId);
            return false;
        }
        return true;
    };

    // File-backed input avoids allocating four temporary string buffers during conversion.
    const size_t mappingSize = static_cast<size_t>(fileInfo.st_size);
    void* mapped = mmap(nullptr, mappingSize, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    if (mapped == MAP_FAILED) { fclose(f); return false; }
    const std::shared_ptr<void> mapping(mapped, [mappingSize](void* data) { munmap(data, mappingSize); });
    auto readStringPoolDirect = [&](uint32_t sectionId, StringPool& pool, const char* name) -> bool {
        if (sectionIdx[sectionId] == 0xFFFFFFFF) return false;
        const auto& entry = sections[sectionIdx[sectionId]];
        if (entry.byteLength < 8) return false;
        const auto* data = static_cast<const uint8_t*>(mapping.get()) + entry.offset;
        if (IndexWAL::crc32(data, entry.byteLength) != entry.crc32) {
            LOG_ERROR("FlatIndexWriter", "CRC mismatch for section " << name);
            return false;
        }
        uint32_t bufSize, count;
        memcpy(&bufSize, data, sizeof(bufSize));
        if (bufSize > entry.byteLength - 8) return false;
        memcpy(&count, data + 4 + bufSize, sizeof(count));
        const size_t entryBytes = size_t(count) * sizeof(StringPool::Entry);
        if (entryBytes != entry.byteLength - 8 - bufSize) return false;
        std::vector<StringPool::Entry> entries(count);
        if (entryBytes) memcpy(entries.data(), data + 8 + bufSize, entryBytes);
        for (const auto& item : entries)
            if (item.offset > bufSize || item.length > bufSize - item.offset) return false;
        pool.loadMappedRaw(reinterpret_cast<const char*>(data + 4), bufSize, std::move(entries), mapping);
        return true;
    };

    // Helper: read array section directly into target vector (no intermediate buffer)
    auto readArrayDirect = [&]<typename T>(uint32_t sectionId, std::vector<T>& vec,
                                            uint32_t expectedCount, const char* name) -> bool {
        if (sectionIdx[sectionId] == 0xFFFFFFFF) {
            LOG_ERROR("FlatIndexWriter", "Missing section: " << name);
            return false;
        }
        const auto& entry = sections[sectionIdx[sectionId]];
        size_t expectedBytes = expectedCount * sizeof(T);
        if (entry.byteLength != expectedBytes) {
            LOG_ERROR("FlatIndexWriter", "Section " << name << " size mismatch: "
                      << entry.byteLength << " vs expected " << expectedBytes);
            return false;
        }
        vec.resize(expectedCount);
        fseek(f, static_cast<long>(entry.offset), SEEK_SET);
        if (fread(vec.data(), 1, expectedBytes, f) != expectedBytes) {
            LOG_ERROR("FlatIndexWriter", "Failed to read section " << name);
            return false;
        }
        // CRC verification in-place (no extra copy)
        uint32_t actualCRC = IndexWAL::crc32(reinterpret_cast<const uint8_t*>(vec.data()), expectedBytes);
        if (actualCRC != entry.crc32) {
            LOG_ERROR("FlatIndexWriter", "CRC mismatch for section " << name);
            return false;
        }
        return true;
    };

    // Read StringPool sections (with move semantics)
    StringPool origNamePool, namePool, pathPool, lowerPathPool;
    if (!readStringPoolDirect(kSectionNamesOrig, origNamePool, "NAMES_ORIG")) { fclose(f); return false; }
    if (!readStringPoolDirect(kSectionNamesLower, namePool, "NAMES_LOWER")) { fclose(f); return false; }
    if (!readStringPoolDirect(kSectionPathPool, pathPool, "PATH_POOL")) { fclose(f); return false; }
    if (!readStringPoolDirect(kSectionLowerPathPool, lowerPathPool, "LOWER_PATH_POOL")) { fclose(f); return false; }

    if (namePool.entryCount() != origNamePool.entryCount() ||
        lowerPathPool.entryCount() != pathPool.entryCount()) { fclose(f); return false; }

    uint32_t n = origNamePool.entryCount();

    // Read array sections directly into target vectors (in-place CRC)
    std::vector<uint32_t> pathIndices;
    std::vector<uint8_t> types;
    std::vector<uint64_t> sizes;
    std::vector<int64_t> modTimes;
    std::vector<uint64_t> inodes;
    std::vector<int32_t> devIds;

    if (!readArrayDirect(kSectionPathIndices, pathIndices, n, "PATH_INDICES")) { fclose(f); return false; }
    if (!readArrayDirect(kSectionTypes, types, n, "TYPES")) { fclose(f); return false; }
    if (!readArrayDirect(kSectionSizes, sizes, n, "SIZES")) { fclose(f); return false; }
    if (!readArrayDirect(kSectionModTimes, modTimes, n, "MOD_TIMES")) { fclose(f); return false; }
    if (!readArrayDirect(kSectionInodes, inodes, n, "INODES")) { fclose(f); return false; }
    if (!readArrayDirect(kSectionDevIds, devIds, n, "DEV_IDS")) { fclose(f); return false; }

    for (uint32_t path : pathIndices) {
        if (path >= pathPool.entryCount()) { fclose(f); return false; }
    }

    // Metadata (optional)
    IndexMetadata meta;
    meta.formatVersion = 6;
    meta.timestamp = header.timestamp;
    meta.lastEventId = header.lastEventId;

    if (sectionIdx[kSectionMetadataKV] != 0xFFFFFFFF) {
        std::vector<uint8_t> metaRaw;
        if (readSectionRaw(kSectionMetadataKV, metaRaw)) {
            readMetadataSection(metaRaw.data(), metaRaw.size(), meta);
        }
    }

    fclose(f);

    if (outMeta) *outMeta = std::move(meta);

    // Install into engine
    engine.loadRecordsV6(
        std::move(origNamePool),
        std::move(namePool),
        std::move(pathIndices),
        std::move(pathPool),
        std::move(lowerPathPool),
        std::move(types),
        std::move(sizes),
        std::move(modTimes),
        std::move(inodes),
        std::move(devIds)
    );

    LOG_INFO("FlatIndexWriter", "v6 loaded: " << n << " records, lastEventId="
             << header.lastEventId);
    return true;
}
