// bsa-reader: read-only access to Bethesda BSA archives, header versions
// 103 (Oblivion), 104 (Fallout 3, New Vegas, Skyrim LE) and 105 (Skyrim SE/AE).
//
// Design: the archive is memory-mapped and validated once at open. Lookups use
// the archive's own sorted hash tables, the way the game engine does, so they
// never touch the name tables and never allocate. Names are decoded only when
// asked for. Nothing here throws; every call returns a Status.
//
// Threading: after open() returns, every const method may be called from any
// number of threads at once (the mapping is immutable and has no cursor).
// open() and close() are not safe to overlap with other calls on the same
// Reader.
//
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace bsa {

enum class Status : int {
    Ok = 0,
    NotFound,            // no such file in the archive
    CannotOpen,          // the OS refused to open or map the file
    NotBsa,              // magic mismatch (Morrowind, BA2 and non-archives land here)
    UnsupportedVersion,  // header version is not 103, 104 or 105
    Unsupported,         // Xbox 360 (big-endian) archive
    Corrupt,             // a count, length or offset points outside the file
    NoNames,             // the archive was built without name tables
    BufferTooSmall,      // caller's buffer cannot hold the uncompressed file
    TooLarge,            // uncompressed size exceeds the caller's limit
    DecompressFailed,    // zlib / LZ4 stream is malformed or has the wrong size
    OutOfMemory,
    IoError,             // the mapping became unreadable (media removed, network drop)
};

const char* to_string(Status s) noexcept;

// Archive flags (header field "archiveFlags").
namespace archive_flags {
constexpr uint32_t FolderNames = 0x001;
constexpr uint32_t FileNames = 0x002;
constexpr uint32_t Compressed = 0x004;  // default for files; per-file bit 0x40000000 in size toggles it
constexpr uint32_t RetainFolderNames = 0x008;
constexpr uint32_t RetainFileNames = 0x010;
constexpr uint32_t RetainFileNameOffsets = 0x020;
constexpr uint32_t Xbox360 = 0x040;
constexpr uint32_t RetainStrings = 0x080;
constexpr uint32_t EmbeddedNames = 0x100;  // v104/v105: each data block starts with its own path
constexpr uint32_t XMemCodec = 0x200;
}  // namespace archive_flags

// Content flags (header field "fileFlags").
namespace file_flags {
constexpr uint32_t Meshes = 0x001;
constexpr uint32_t Textures = 0x002;
constexpr uint32_t Menus = 0x004;
constexpr uint32_t Sounds = 0x008;
constexpr uint32_t Voices = 0x010;
constexpr uint32_t Shaders = 0x020;
constexpr uint32_t Trees = 0x040;
constexpr uint32_t Fonts = 0x080;
constexpr uint32_t Misc = 0x100;
}  // namespace file_flags

struct Header {
    uint32_t version = 0;        // 103, 104 or 105
    uint32_t archiveFlags = 0;
    uint32_t fileFlags = 0;
    uint32_t folderCount = 0;
    uint32_t fileCount = 0;
    uint32_t folderNamesLength = 0;
    uint32_t fileNamesLength = 0;
    uint64_t archiveSize = 0;    // size of the file on disk
};

struct Entry {
    uint64_t folderHash = 0;
    uint64_t fileHash = 0;
    uint32_t folderIndex = 0;   // index into the folder table
    uint32_t fileIndex = 0;     // global index, same order as the file-name table
    uint64_t dataOffset = 0;    // absolute offset of the stored block
    uint32_t storedSize = 0;    // size of the stored block (prefixes included)
    bool compressed = false;
};

// Engine hashes. Input is case-insensitive; '/' and '\' are equivalent; a
// trailing separator on a folder is ignored. Pass the archive's header version
// because v103 hashes bytes above 0x7F differently.
uint64_t hash_folder(std::string_view folder, uint32_t version = 105) noexcept;
uint64_t hash_file(std::string_view fileName, uint32_t version = 105) noexcept;

constexpr size_t kDefaultMaxRead = size_t{256} << 20;  // 256 MiB

class Reader {
public:
    Reader() noexcept;
    ~Reader();
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Maps the file with share-read/write/delete and validates every table.
    // Keep readers short-lived: an open mapping prevents in-place truncation
    // of the archive by other processes (renames and deletes still work).
    Status open(const std::filesystem::path& path) noexcept;
    void close() noexcept;
    bool is_open() const noexcept;
    const Header& header() const noexcept;

    // Hash lookup. `path` is "folder\\file.ext" (either separator). O(log n),
    // no allocation, name tables untouched.
    Status find(std::string_view path, Entry& out) const noexcept;
    Status find(uint64_t folderHash, uint64_t fileHash, Entry& out) const noexcept;

    // Entry by global index, 0 <= fileIndex < header().fileCount.
    Status entry(uint32_t fileIndex, Entry& out) const noexcept;

    // Names. Views point into the mapping and die with the Reader.
    Status folder_name(uint32_t folderIndex, std::string_view& out) const noexcept;
    Status file_name(const Entry& e, std::string_view& out) const noexcept;  // name only, no folder

    // Size after decompression (reads the 4-byte prefix for compressed entries).
    Status uncompressed_size(const Entry& e, uint32_t& size) const noexcept;

    // Decompresses (or copies) into the caller's buffer.
    Status read(const Entry& e, void* dst, size_t capacity, size_t& written) const noexcept;
    // Same, growing `out` to the exact size; refuses anything above maxBytes.
    Status read(const Entry& e, std::vector<uint8_t>& out, size_t maxBytes = kDefaultMaxRead) const noexcept;

    // The stored payload without decoding: the compressed stream for
    // compressed entries, the file bytes otherwise. Embedded-name and size
    // prefixes are skipped.
    Status raw(const Entry& e, const uint8_t*& data, size_t& size) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bsa
