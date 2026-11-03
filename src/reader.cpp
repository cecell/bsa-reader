// Memory-mapped BSA reader. See include/bsa/reader.hpp for the contract.
// SPDX-License-Identifier: GPL-3.0-only
#include "bsa/reader.hpp"

#include "codecs.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bsa {

const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::NotFound: return "not found";
        case Status::CannotOpen: return "cannot open";
        case Status::NotBsa: return "not a BSA archive";
        case Status::UnsupportedVersion: return "unsupported BSA version";
        case Status::Unsupported: return "unsupported archive (Xbox 360)";
        case Status::Corrupt: return "corrupt archive";
        case Status::NoNames: return "archive has no name tables";
        case Status::BufferTooSmall: return "buffer too small";
        case Status::TooLarge: return "file exceeds size limit";
        case Status::DecompressFailed: return "decompression failed";
        case Status::OutOfMemory: return "out of memory";
        case Status::IoError: return "I/O error reading the mapping";
    }
    return "unknown status";
}

namespace {

constexpr uint64_t kHeaderSize = 36;
constexpr uint64_t kFileRecordSize = 16;
constexpr uint32_t kSizeCompressToggle = 0x40000000u;
constexpr uint32_t kSizeMask = 0x3FFFFFFFu;

// Hard ceilings on header counts, so a hostile file cannot make open()
// allocate in proportion to its size. The largest shipped archives have
// 1,700 folders and 75,408 files; these are 38x and 27x that.
constexpr uint32_t kMaxFolders = 65536;
constexpr uint32_t kMaxFiles = 2097152;

inline uint32_t rd32(const uint8_t* p) noexcept {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}
inline uint64_t rd64(const uint8_t* p) noexcept { return uint64_t{rd32(p)} | (uint64_t{rd32(p + 4)} << 32); }

struct Mapping {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
#ifdef _WIN32
    HANDLE mapping = nullptr;
#endif

    Status map(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Status::CannotOpen;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
            CloseHandle(h);
            return sz.QuadPart == 0 ? Status::NotBsa : Status::CannotOpen;
        }
        HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(h);  // the mapping keeps the file alive
        if (!m) return Status::CannotOpen;
        void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        if (!v) {
            CloseHandle(m);
            return Status::CannotOpen;
        }
        mapping = m;
        data = static_cast<const uint8_t*>(v);
        size = static_cast<uint64_t>(sz.QuadPart);
#else
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return Status::CannotOpen;
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size < 0) {
            ::close(fd);
            return Status::CannotOpen;
        }
        if (st.st_size == 0) {
            ::close(fd);
            return Status::NotBsa;
        }
        void* v = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (v == MAP_FAILED) return Status::CannotOpen;
        data = static_cast<const uint8_t*>(v);
        size = static_cast<uint64_t>(st.st_size);
#endif
        return Status::Ok;
    }

    void unmap() noexcept {
#ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        mapping = nullptr;
#else
        if (data) munmap(const_cast<uint8_t*>(data), static_cast<size_t>(size));
#endif
        data = nullptr;
        size = 0;
    }
};

struct FolderInfo {
    uint64_t hash = 0;
    uint64_t nameOffset = 0;     // offset of the length byte; meaningful only with FolderNames
    uint64_t recordsOffset = 0;  // first 16-byte file record
    uint32_t fileCount = 0;
    uint32_t firstFileIndex = 0;
    bool sorted = true;          // file hashes ascending, so binary search is valid
};

// On MSVC, reads from a mapping whose backing file vanished (network drop,
// media removed) raise EXCEPTION_IN_PAGE_ERROR. Catch exactly that one.
template <class F>
Status guarded(F& f) noexcept {
#ifdef _MSC_VER
    __try {
        return f();
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return Status::IoError;
    }
#else
    return f();
#endif
}

}  // namespace

struct Reader::Impl {
    Mapping map;
    Header hdr;
    std::vector<FolderInfo> folders;
    bool foldersSorted = true;
    uint64_t namesOffset = 0;  // file-name block; meaningful only with FileNames

    mutable std::once_flag namesOnce;
    mutable std::vector<uint32_t> nameOffsets;  // relative to namesOffset
    mutable Status namesStatus = Status::Ok;

    ~Impl() { map.unmap(); }

    bool has(uint32_t flag) const noexcept { return (hdr.archiveFlags & flag) != 0; }

    Status parse() noexcept {
        const uint8_t* d = map.data;
        const uint64_t size = map.size;
        if (size < 4 || std::memcmp(d, "BSA\0", 4) != 0) return Status::NotBsa;
        if (size < kHeaderSize) return Status::Corrupt;

        hdr.version = rd32(d + 4);
        if (hdr.version != 103 && hdr.version != 104 && hdr.version != 105) return Status::UnsupportedVersion;
        const uint64_t folderRecordOffset = rd32(d + 8);
        hdr.archiveFlags = rd32(d + 12);
        hdr.folderCount = rd32(d + 16);
        hdr.fileCount = rd32(d + 20);
        hdr.folderNamesLength = rd32(d + 24);
        hdr.fileNamesLength = rd32(d + 28);
        hdr.fileFlags = rd32(d + 32);
        hdr.archiveSize = size;
        if (has(archive_flags::Xbox360)) return Status::Unsupported;
        if (hdr.folderCount > kMaxFolders || hdr.fileCount > kMaxFiles) return Status::Corrupt;

        const uint64_t recSize = hdr.version == 105 ? 24 : 16;
        if (folderRecordOffset > size || hdr.folderCount > (size - folderRecordOffset) / recSize) return Status::Corrupt;

        folders.resize(hdr.folderCount);
        uint64_t totalFiles = 0, tablesEnd = folderRecordOffset + hdr.folderCount * recSize, prevHash = 0;
        for (uint32_t i = 0; i < hdr.folderCount; ++i) {
            const uint8_t* r = d + folderRecordOffset + i * recSize;
            FolderInfo& f = folders[i];
            f.hash = rd64(r);
            f.fileCount = rd32(r + 8);
            const uint64_t recordedOffset = hdr.version == 105 ? rd64(r + 16) : rd32(r + 12);
            // The stored offset includes the total file-name length (engine quirk).
            if (recordedOffset < hdr.fileNamesLength) return Status::Corrupt;
            uint64_t block = recordedOffset - hdr.fileNamesLength;
            if (block > size) return Status::Corrupt;
            if (has(archive_flags::FolderNames)) {
                if (block >= size) return Status::Corrupt;
                const uint8_t len = d[block];
                if (len == 0 || len > size - block - 1) return Status::Corrupt;
                f.nameOffset = block;
                block += 1 + len;
            }
            if (f.fileCount > (size - block) / kFileRecordSize) return Status::Corrupt;
            f.recordsOffset = block;
            f.firstFileIndex = static_cast<uint32_t>(totalFiles);
            totalFiles += f.fileCount;
            if (totalFiles > hdr.fileCount) return Status::Corrupt;

            uint64_t prevFile = 0;
            for (uint32_t j = 0; j < f.fileCount; ++j) {
                const uint8_t* fr = d + block + j * kFileRecordSize;
                const uint64_t fh = rd64(fr);
                if (fh < prevFile) f.sorted = false;
                prevFile = fh;
                const uint64_t dataOff = rd32(fr + 12), stored = rd32(fr + 8) & kSizeMask;
                if (dataOff > size || stored > size - dataOff) return Status::Corrupt;
            }
            tablesEnd = std::max(tablesEnd, block + f.fileCount * kFileRecordSize);
            if (f.hash < prevHash) foldersSorted = false;
            prevHash = f.hash;
        }
        if (totalFiles != hdr.fileCount) return Status::Corrupt;
        if (has(archive_flags::FileNames)) {
            if (hdr.fileNamesLength > size - tablesEnd) return Status::Corrupt;
            namesOffset = tablesEnd;
        }
        return Status::Ok;
    }

    Status fill(uint32_t folderIndex, uint32_t j, Entry& e) const noexcept {
        const FolderInfo& f = folders[folderIndex];
        const uint8_t* fr = map.data + f.recordsOffset + j * kFileRecordSize;
        const uint32_t rawSize = rd32(fr + 8);
        e.folderHash = f.hash;
        e.fileHash = rd64(fr);
        e.folderIndex = folderIndex;
        e.fileIndex = f.firstFileIndex + j;
        e.dataOffset = rd32(fr + 12);
        e.storedSize = rawSize & kSizeMask;
        e.compressed = has(archive_flags::Compressed) != ((rawSize & kSizeCompressToggle) != 0);
        return Status::Ok;
    }

    Status find_in_folder(uint32_t folderIndex, uint64_t fileHash, Entry& e) const noexcept {
        const FolderInfo& f = folders[folderIndex];
        const uint8_t* base = map.data + f.recordsOffset;
        if (f.sorted) {
            uint32_t lo = 0, hi = f.fileCount;
            while (lo < hi) {
                const uint32_t mid = lo + (hi - lo) / 2;
                const uint64_t h = rd64(base + mid * kFileRecordSize);
                if (h < fileHash) lo = mid + 1;
                else hi = mid;
            }
            if (lo < f.fileCount && rd64(base + lo * kFileRecordSize) == fileHash) return fill(folderIndex, lo, e);
            return Status::NotFound;
        }
        for (uint32_t j = 0; j < f.fileCount; ++j)
            if (rd64(base + j * kFileRecordSize) == fileHash) return fill(folderIndex, j, e);
        return Status::NotFound;
    }

    Status find(uint64_t folderHash, uint64_t fileHash, Entry& e) const noexcept {
        if (foldersSorted) {
            auto it = std::lower_bound(folders.begin(), folders.end(), folderHash,
                                       [](const FolderInfo& f, uint64_t h) { return f.hash < h; });
            for (; it != folders.end() && it->hash == folderHash; ++it) {
                const Status s = find_in_folder(static_cast<uint32_t>(it - folders.begin()), fileHash, e);
                if (s != Status::NotFound) return s;
            }
            return Status::NotFound;
        }
        for (uint32_t i = 0; i < hdr.folderCount; ++i) {
            if (folders[i].hash != folderHash) continue;
            const Status s = find_in_folder(i, fileHash, e);
            if (s != Status::NotFound) return s;
        }
        return Status::NotFound;
    }

    Status entry(uint32_t fileIndex, Entry& e) const noexcept {
        if (fileIndex >= hdr.fileCount) return Status::NotFound;
        auto it = std::upper_bound(folders.begin(), folders.end(), fileIndex,
                                   [](uint32_t i, const FolderInfo& f) { return i < f.firstFileIndex; });
        // Skip back over empty folders to the one that owns this index.
        do {
            --it;
        } while (it->fileCount == 0);
        return fill(static_cast<uint32_t>(it - folders.begin()), fileIndex - it->firstFileIndex, e);
    }

    // Locates the stored payload; validates the Entry against the mapping in
    // case the caller forged or reused it across archives.
    Status payload(const Entry& e, const uint8_t*& p, size_t& n, uint32_t& uncompressed) const noexcept {
        if (e.dataOffset > map.size || e.storedSize > map.size - e.dataOffset) return Status::Corrupt;
        p = map.data + e.dataOffset;
        n = e.storedSize;
        if (hdr.version >= 104 && has(archive_flags::EmbeddedNames)) {
            if (n < 1) return Status::Corrupt;
            const uint8_t len = p[0];
            if (n < 1u + len) return Status::Corrupt;
            p += 1 + len;
            n -= 1 + len;
        }
        if (e.compressed) {
            if (n < 4) return Status::Corrupt;
            uncompressed = rd32(p);
            p += 4;
            n -= 4;
        } else {
            uncompressed = static_cast<uint32_t>(n);
        }
        return Status::Ok;
    }

    Status decode(const Entry& e, const uint8_t* p, size_t n, uint8_t* dst, size_t len) const noexcept {
        if (!e.compressed) {
            std::memcpy(dst, p, len);
            return Status::Ok;
        }
        if (len == 0) return Status::Ok;
        const bool ok = hdr.version == 105 ? detail::lz4_frame_decode(p, n, dst, len) : detail::zlib_inflate(p, n, dst, len);
        return ok ? Status::Ok : Status::DecompressFailed;
    }

    void build_name_index() const noexcept {
        try {
            nameOffsets.reserve(hdr.fileCount);
        } catch (...) {
            namesStatus = Status::OutOfMemory;
            return;
        }
        const uint8_t* base = map.data + namesOffset;
        const size_t len = hdr.fileNamesLength;
        size_t pos = 0;
        for (uint32_t i = 0; i < hdr.fileCount; ++i) {
            if (pos >= len) {
                namesStatus = Status::Corrupt;
                return;
            }
            const void* nul = std::memchr(base + pos, 0, len - pos);
            if (!nul) {
                namesStatus = Status::Corrupt;
                return;
            }
            nameOffsets.push_back(static_cast<uint32_t>(pos));
            pos = static_cast<size_t>(static_cast<const uint8_t*>(nul) - base) + 1;
        }
    }
};

Reader::Reader() noexcept = default;
Reader::~Reader() = default;
Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

Status Reader::open(const std::filesystem::path& path) noexcept {
    close();
    try {
        impl_ = std::make_unique<Impl>();
        Status s = impl_->map.map(path);
        if (s != Status::Ok) {
            impl_.reset();
            return s;
        }
        auto body = [this]() noexcept { return impl_->parse(); };
        s = guarded(body);
        if (s != Status::Ok) impl_.reset();
        return s;
    } catch (const std::bad_alloc&) {
        impl_.reset();
        return Status::OutOfMemory;
    } catch (...) {
        impl_.reset();
        return Status::CannotOpen;
    }
}

void Reader::close() noexcept { impl_.reset(); }

bool Reader::is_open() const noexcept { return impl_ != nullptr; }

const Header& Reader::header() const noexcept {
    static const Header empty{};
    return impl_ ? impl_->hdr : empty;
}

Status Reader::find(std::string_view path, Entry& out) const noexcept {
    if (!impl_) return Status::NotFound;
    const size_t sep = path.find_last_of("\\/");
    if (sep == std::string_view::npos || sep == 0 || sep + 1 == path.size()) return Status::NotFound;
    const uint32_t v = impl_->hdr.version;
    return find(hash_folder(path.substr(0, sep), v), hash_file(path.substr(sep + 1), v), out);
}

Status Reader::find(uint64_t folderHash, uint64_t fileHash, Entry& out) const noexcept {
    if (!impl_) return Status::NotFound;
    auto body = [&]() noexcept { return impl_->find(folderHash, fileHash, out); };
    return guarded(body);
}

Status Reader::entry(uint32_t fileIndex, Entry& out) const noexcept {
    if (!impl_) return Status::NotFound;
    auto body = [&]() noexcept { return impl_->entry(fileIndex, out); };
    return guarded(body);
}

Status Reader::folder_name(uint32_t folderIndex, std::string_view& out) const noexcept {
    if (!impl_) return Status::NotFound;
    if (!impl_->has(archive_flags::FolderNames)) return Status::NoNames;
    if (folderIndex >= impl_->hdr.folderCount) return Status::NotFound;
    auto body = [&]() noexcept {
        const uint8_t* p = impl_->map.data + impl_->folders[folderIndex].nameOffset;
        size_t len = p[0];
        if (len > 0 && p[len] == 0) --len;  // drop the stored terminator
        out = std::string_view(reinterpret_cast<const char*>(p + 1), len);
        return Status::Ok;
    };
    return guarded(body);
}

Status Reader::file_name(const Entry& e, std::string_view& out) const noexcept {
    if (!impl_) return Status::NotFound;
    if (!impl_->has(archive_flags::FileNames)) return Status::NoNames;
    if (e.fileIndex >= impl_->hdr.fileCount) return Status::NotFound;
    auto body = [&]() noexcept {
        std::call_once(impl_->namesOnce, [this] { impl_->build_name_index(); });
        if (impl_->namesStatus != Status::Ok) return impl_->namesStatus;
        const char* p = reinterpret_cast<const char*>(impl_->map.data + impl_->namesOffset + impl_->nameOffsets[e.fileIndex]);
        out = std::string_view(p);  // terminator verified by build_name_index
        return Status::Ok;
    };
    return guarded(body);
}

Status Reader::uncompressed_size(const Entry& e, uint32_t& size) const noexcept {
    if (!impl_) return Status::NotFound;
    auto body = [&]() noexcept {
        const uint8_t* p;
        size_t n;
        return impl_->payload(e, p, n, size);
    };
    return guarded(body);
}

Status Reader::read(const Entry& e, void* dst, size_t capacity, size_t& written) const noexcept {
    written = 0;
    if (!impl_) return Status::NotFound;
    auto body = [&]() noexcept {
        const uint8_t* p;
        size_t n;
        uint32_t size;
        const Status s = impl_->payload(e, p, n, size);
        if (s != Status::Ok) return s;
        if (size > capacity) return Status::BufferTooSmall;
        const Status d = impl_->decode(e, p, n, static_cast<uint8_t*>(dst), size);
        if (d == Status::Ok) written = size;
        return d;
    };
    return guarded(body);
}

Status Reader::read(const Entry& e, std::vector<uint8_t>& out, size_t maxBytes) const noexcept {
    if (!impl_) return Status::NotFound;
    auto body = [&]() noexcept {
        const uint8_t* p;
        size_t n;
        uint32_t size;
        const Status s = impl_->payload(e, p, n, size);
        if (s != Status::Ok) return s;
        if (size > maxBytes) return Status::TooLarge;
        try {
            out.resize(size);
        } catch (...) {
            return Status::OutOfMemory;
        }
        return impl_->decode(e, p, n, out.data(), size);
    };
    return guarded(body);
}

Status Reader::raw(const Entry& e, const uint8_t*& data, size_t& size) const noexcept {
    if (!impl_) return Status::NotFound;
    auto body = [&]() noexcept {
        uint32_t unc;
        return impl_->payload(e, data, size, unc);
    };
    return guarded(body);
}

}  // namespace bsa
