// Unit tests: synthetic archives (no game data needed), real hash vectors,
// decoder checks and hostile-input fuzzing.
// SPDX-License-Identifier: GPL-3.0-only
#include "bsa/reader.hpp"
#include "codecs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using bsa::Status;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                           \
    } while (0)
#define CHECK_STATUS(expr, want)                                                                              \
    do {                                                                                                      \
        const Status got_ = (expr);                                                                           \
        if (got_ != (want)) {                                                                                 \
            ++g_failures;                                                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s -> %s, wanted %s\n", __FILE__, __LINE__, #expr,              \
                         bsa::to_string(got_), bsa::to_string(want));                                         \
        }                                                                                                     \
    } while (0)

// ---------------------------------------------------------------- builder

struct TFile {
    std::string folder, name;
    std::vector<uint8_t> data;
    bool compress = false;
};

struct Opts {
    uint32_t version = 105;
    uint32_t flags = 0x3;
    bool sortTables = true;
};

static void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
static void put64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, static_cast<uint32_t>(x));
    put32(v, static_cast<uint32_t>(x >> 32));
}
static void patch32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<uint8_t>(x >> (8 * i));
}
static uint32_t get32(const std::vector<uint8_t>& v, size_t at) {
    return uint32_t{v[at]} | (uint32_t{v[at + 1]} << 8) | (uint32_t{v[at + 2]} << 16) | (uint32_t{v[at + 3]} << 24);
}

static uint32_t adler32(const std::vector<uint8_t>& d) {
    uint32_t a = 1, b = 0;
    for (uint8_t c : d) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// LZ4 frame made only of uncompressed blocks: valid, decodable, no encoder needed.
static std::vector<uint8_t> fake_lz4(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> s = {0x04, 0x22, 0x4D, 0x18, 0x60, 0x40, 0x00};
    for (size_t pos = 0; pos < d.size();) {
        const size_t n = std::min<size_t>(65536, d.size() - pos);
        put32(s, 0x80000000u | static_cast<uint32_t>(n));
        s.insert(s.end(), d.begin() + pos, d.begin() + pos + n);
        pos += n;
    }
    put32(s, 0);
    return s;
}

// zlib stream made of stored deflate blocks.
static std::vector<uint8_t> fake_zlib(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> s = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, d.size() - pos);
        const bool last = pos + n == d.size();
        s.push_back(last ? 1 : 0);
        s.push_back(static_cast<uint8_t>(n));
        s.push_back(static_cast<uint8_t>(n >> 8));
        s.push_back(static_cast<uint8_t>(~n));
        s.push_back(static_cast<uint8_t>(~n >> 8));
        s.insert(s.end(), d.begin() + pos, d.begin() + pos + n);
        pos += n;
    } while (pos < d.size());
    const uint32_t a = adler32(d);
    s.push_back(static_cast<uint8_t>(a >> 24));
    s.push_back(static_cast<uint8_t>(a >> 16));
    s.push_back(static_cast<uint8_t>(a >> 8));
    s.push_back(static_cast<uint8_t>(a));
    return s;
}

static std::vector<uint8_t> build(const Opts& o, std::vector<TFile> files) {
    struct Folder {
        std::string name;
        uint64_t hash;
        std::vector<size_t> idx;
    };
    std::vector<Folder> folders;
    for (size_t i = 0; i < files.size(); ++i) {
        auto it = std::find_if(folders.begin(), folders.end(), [&](const Folder& f) { return f.name == files[i].folder; });
        if (it == folders.end()) {
            folders.push_back({files[i].folder, bsa::hash_folder(files[i].folder, o.version), {}});
            it = folders.end() - 1;
        }
        it->idx.push_back(i);
    }
    auto fileHash = [&](size_t i) { return bsa::hash_file(files[i].name, o.version); };
    if (o.sortTables) {
        std::sort(folders.begin(), folders.end(), [](const Folder& a, const Folder& b) { return a.hash < b.hash; });
        for (auto& f : folders) std::sort(f.idx.begin(), f.idx.end(), [&](size_t a, size_t b) { return fileHash(a) < fileHash(b); });
    }
    const bool folderNames = o.flags & 1, fileNames = o.flags & 2;
    const bool embed = (o.flags & 0x100) && o.version >= 104;
    const uint32_t recSize = o.version == 105 ? 24 : 16;

    uint32_t folderNamesLength = 0, fileNamesLength = 0;
    for (const auto& f : folders) folderNamesLength += static_cast<uint32_t>(f.name.size() + 1);
    for (const auto& f : files) fileNamesLength += static_cast<uint32_t>(f.name.size() + 1);

    // Data blocks first, so the records can point at them.
    std::vector<std::vector<uint8_t>> blocks(files.size());
    for (const auto& f : folders)
        for (size_t i : f.idx) {
            auto& b = blocks[i];
            if (embed) {
                const std::string full = files[i].folder + "\\" + files[i].name;
                b.push_back(static_cast<uint8_t>(full.size()));
                b.insert(b.end(), full.begin(), full.end());
            }
            if (files[i].compress) {
                put32(b, static_cast<uint32_t>(files[i].data.size()));
                const auto s = o.version == 105 ? fake_lz4(files[i].data) : fake_zlib(files[i].data);
                b.insert(b.end(), s.begin(), s.end());
            } else {
                b.insert(b.end(), files[i].data.begin(), files[i].data.end());
            }
        }

    size_t tables = 36 + folders.size() * recSize;
    for (const auto& f : folders) tables += (folderNames ? 2 + f.name.size() : 0) + 16 * f.idx.size();
    const size_t dataStart = tables + (fileNames ? fileNamesLength : 0);

    std::vector<uint8_t> out;
    out.insert(out.end(), {'B', 'S', 'A', 0});
    put32(out, o.version);
    put32(out, 36);
    put32(out, o.flags);
    put32(out, static_cast<uint32_t>(folders.size()));
    put32(out, static_cast<uint32_t>(files.size()));
    put32(out, folderNamesLength);
    put32(out, fileNamesLength);
    put32(out, 1);

    size_t block = 36 + folders.size() * recSize;
    for (const auto& f : folders) {
        put64(out, f.hash);
        put32(out, static_cast<uint32_t>(f.idx.size()));
        if (o.version == 105) put32(out, 0);
        put32(out, static_cast<uint32_t>(block + fileNamesLength));
        if (o.version == 105) put32(out, 0);
        block += (folderNames ? 2 + f.name.size() : 0) + 16 * f.idx.size();
    }
    size_t dataPos = dataStart;
    for (const auto& f : folders) {
        if (folderNames) {
            out.push_back(static_cast<uint8_t>(f.name.size() + 1));
            out.insert(out.end(), f.name.begin(), f.name.end());
            out.push_back(0);
        }
        for (size_t i : f.idx) {
            put64(out, fileHash(i));
            uint32_t size = static_cast<uint32_t>(blocks[i].size());
            if (files[i].compress != ((o.flags & 4) != 0)) size |= 0x40000000u;
            put32(out, size);
            put32(out, static_cast<uint32_t>(dataPos));
            dataPos += blocks[i].size();
        }
    }
    if (fileNames)
        for (const auto& f : folders)
            for (size_t i : f.idx) {
                out.insert(out.end(), files[i].name.begin(), files[i].name.end());
                out.push_back(0);
            }
    for (const auto& f : folders)
        for (size_t i : f.idx) out.insert(out.end(), blocks[i].begin(), blocks[i].end());
    return out;
}

static int g_tmpCounter = 0;
struct TempFile {
    fs::path path;
    explicit TempFile(const std::vector<uint8_t>& bytes) {
        path = fs::temp_directory_path() / ("bsa_reader_test_" + std::to_string(g_tmpCounter++) + ".bsa");
        FILE* f = std::fopen(path.string().c_str(), "wb");
        if (f) {
            if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

static std::vector<uint8_t> bytes(const char* s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }

static std::vector<TFile> sample_files() {
    std::vector<uint8_t> big(200000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    return {
        {"strings", "skyrim_english.strings", bytes("strings-data"), false},
        {"strings", "skyrim_english.dlstrings", bytes("dl"), true},
        {"meshes\\actors\\character", "skeleton.nif", bytes("NIF?"), true},
        {"meshes\\actors\\character", "skeleton.kf", bytes("KF"), false},
        {"textures\\actors", "body.dds", big, true},
        {"textures\\actors", "empty.dds", {}, true},
        {"sound\\voice\\skyrim.esm\\malenord", "line_000123ab_1.fuz", bytes("fuz"), false},
    };
}

// Opens, then exercises every entry; must never crash whatever the status.
static void exercise(const fs::path& p) {
    bsa::Reader r;
    if (r.open(p) != Status::Ok) return;
    std::vector<uint8_t> buf;
    for (uint32_t i = 0; i < r.header().fileCount; ++i) {
        bsa::Entry e;
        if (r.entry(i, e) != Status::Ok) continue;
        std::string_view f, n;
        r.folder_name(e.folderIndex, f);
        r.file_name(e, n);
        uint32_t sz;
        r.uncompressed_size(e, sz);
        r.read(e, buf, 1u << 20);
        bsa::Entry e2;
        r.find(e.folderHash, e.fileHash, e2);
    }
}

// ---------------------------------------------------------------- tests

static void test_hash_vectors() {
    struct V {
        uint32_t version;
        const char* folder;
        uint64_t folderHash;
        const char* file;
        uint64_t fileHash;
    };
    // Taken from the folder/file records of the shipped archives.
    static const V v[] = {
        {105, "interface\\inventory components", 0x526B1C47691E7473ULL, "invertedinventorylists.swf", 0x022B930C69167473ULL},
        {105, "interface\\inventory components", 0x526B1C47691E7473ULL, "bottombar.swf", 0x45D7386962096172ULL},
        {105, "interface\\controls\\prospero\\vita", 0xF6E33FDD69207461ULL, "controlmap.txt", 0xAFB8AF35630A6170ULL},
        {105, "meshes\\dlc01\\architecture\\snowelfruins\\animated\\sersecretdooraltar", 0x1C6E52CC6D426172ULL, "seruinshallbgdoorsecretaltar01.nif", 0x44992315731EB031ULL},
        {105, "meshes\\creationclub\\_shared\\dungeons\\ayleidruins\\interior\\triggers", 0xD5F112D16D427273ULL, "artrigpressureplate01.nif", 0xD68AFC606115B031ULL},
        {103, "menus\\prefabs", 0x023157096D0D6273ULL, "scroll_line.txt", 0x19D0C43E730B6E65ULL},
        {103, "menus\\prefabs", 0x023157096D0D6273ULL, "scroll_line.xml", 0x1BBDFF85730B6E65ULL},
        {103, "menus\\generic", 0x4676F1A86D0D6963ULL, "test_menu.xml", 0x1EBA5FFA74096E75ULL},
        {103, "menus\\generic", 0x4676F1A86D0D6963ULL, "skill_perk.xml", 0xABB3769F730A726BULL},
        {103, "sound\\fx\\ambient\\regions\\wind\\cloudplanegust", 0x9F81737E732C7374ULL, "ambr_cloudplanegust_01.wav", 0xB8758CC5E1163031ULL},
        {103, "sound\\fx\\ambient\\regions\\wind\\cloudplanegust", 0x9F81737E732C7374ULL, "ambr_cloudplanegust_02.wav", 0xB8758CC5E1163032ULL},
        {103, "meshes\\dungeons\\fortruins\\dungeon\\switchesandtraps", 0xF8AF697B6D327073ULL, "ndjulianospressureplate.nif", 0x374BBA606E17F465ULL},
        // Names with bytes above 0x7F, from third-party SSE archives: the
        // extension marker is XORed in and the sums are unsigned.
        {105, "meshes\\oaristys\\alchemy", 0xA75743916D176D79ULL, "alchbag_coffee\xC2\xB0.nif", 0x54F85E71611042B0ULL},
        {105, "textures\\dem_v2\\leggings", 0x316E7ABB74186773ULL, "demv2leggings01 - \xE5\x89\xAF\xE6\x9C\xAC.dds", 0x62745EE464181C2CULL},
    };
    for (const auto& t : v) {
        CHECK(bsa::hash_folder(t.folder, t.version) == t.folderHash);
        CHECK(bsa::hash_file(t.file, t.version) == t.fileHash);
    }
    // Normalisation: case, separators, trailing separator.
    CHECK(bsa::hash_folder("Interface/Inventory Components/") == 0x526B1C47691E7473ULL);
    CHECK(bsa::hash_file("BottomBar.SWF") == 0x45D7386962096172ULL);
    // v103 treats high bytes as signed, so the two versions must differ for them.
    // (only the middle of the stem and the extension feed the signed sums)
    CHECK(bsa::hash_file("ca\xE9" "fe.nif", 103) != bsa::hash_file("ca\xE9" "fe.nif", 104));
    CHECK(bsa::hash_file("cafe.nif", 103) == bsa::hash_file("cafe.nif", 104));
}

static void test_roundtrip(uint32_t version, uint32_t flags) {
    const auto files = sample_files();
    TempFile t(build({version, flags}, files));
    bsa::Reader r;
    CHECK_STATUS(r.open(t.path), Status::Ok);
    CHECK(r.header().version == version);
    CHECK(r.header().fileCount == files.size());
    CHECK(r.header().folderCount == 4);

    std::vector<uint8_t> buf;
    for (const auto& f : files) {
        bsa::Entry e;
        CHECK_STATUS(r.find(f.folder + "\\" + f.name, e), Status::Ok);
        CHECK(e.compressed == f.compress);
        uint32_t sz = 0;
        CHECK_STATUS(r.uncompressed_size(e, sz), Status::Ok);
        CHECK(sz == f.data.size());
        CHECK_STATUS(r.read(e, buf), Status::Ok);
        CHECK(buf == f.data);

        std::vector<uint8_t> fixedBuf(f.data.size() + 1);
        size_t written = 99;
        CHECK_STATUS(r.read(e, fixedBuf.data(), fixedBuf.size(), written), Status::Ok);
        CHECK(written == f.data.size());
        if (!f.data.empty()) CHECK_STATUS(r.read(e, fixedBuf.data(), f.data.size() - 1, written), Status::BufferTooSmall);
        if (!f.data.empty()) CHECK_STATUS(r.read(e, buf, f.data.size() - 1), Status::TooLarge);

        std::string_view folder, name;
        CHECK_STATUS(r.folder_name(e.folderIndex, folder), Status::Ok);
        CHECK_STATUS(r.file_name(e, name), Status::Ok);
        CHECK(folder == f.folder);
        CHECK(name == f.name);

        bsa::Entry e2;
        CHECK_STATUS(r.entry(e.fileIndex, e2), Status::Ok);
        CHECK(e2.fileHash == e.fileHash && e2.dataOffset == e.dataOffset);

        // Mixed case and forward slashes resolve too.
        std::string alt = f.folder + "/" + f.name;
        for (char& c : alt) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        CHECK_STATUS(r.find(alt, e2), Status::Ok);
        CHECK(e2.fileIndex == e.fileIndex);
    }
    bsa::Entry e;
    CHECK_STATUS(r.find("strings\\missing.strings", e), Status::NotFound);
    CHECK_STATUS(r.find("nofolder.strings", e), Status::NotFound);
    CHECK_STATUS(r.find("strings\\", e), Status::NotFound);
    CHECK_STATUS(r.entry(r.header().fileCount, e), Status::NotFound);
}

static void test_unsorted_tables() {
    // Folder "zzz" hashes above "aaa"; inserting in either order must work.
    for (int order = 0; order < 2; ++order) {
        std::vector<TFile> files = {{"zzz", "b.txt", bytes("1")}, {"zzz", "a.txt", bytes("2")}, {"aaa", "y.txt", bytes("3")}, {"aaa", "x.txt", bytes("4")}};
        if (order) std::reverse(files.begin(), files.end());
        TempFile t(build({105, 0x3, false}, files));
        bsa::Reader r;
        CHECK_STATUS(r.open(t.path), Status::Ok);
        for (const auto& f : files) {
            bsa::Entry e;
            CHECK_STATUS(r.find(f.folder + "\\" + f.name, e), Status::Ok);
            std::vector<uint8_t> buf;
            CHECK_STATUS(r.read(e, buf), Status::Ok);
            CHECK(buf == f.data);
        }
    }
}

static void test_no_names() {
    TempFile t(build({105, 0x0}, sample_files()));
    bsa::Reader r;
    CHECK_STATUS(r.open(t.path), Status::Ok);
    bsa::Entry e;
    CHECK_STATUS(r.find("strings\\skyrim_english.strings", e), Status::Ok);
    std::string_view s;
    CHECK_STATUS(r.folder_name(0, s), Status::NoNames);
    CHECK_STATUS(r.file_name(e, s), Status::NoNames);
    std::vector<uint8_t> buf;
    CHECK_STATUS(r.read(e, buf), Status::Ok);
    CHECK(buf == bytes("strings-data"));
}

static void test_rejections() {
    bsa::Reader r;
    CHECK_STATUS(r.open(fs::temp_directory_path() / "bsa_reader_does_not_exist.bsa"), Status::CannotOpen);
    CHECK(!r.is_open());
    {
        TempFile t({});
        CHECK_STATUS(r.open(t.path), Status::NotBsa);
    }
    {
        TempFile t(bytes("BTDX\x01\x00\x00\x00GNRL"));
        CHECK_STATUS(r.open(t.path), Status::NotBsa);
    }
    {
        TempFile t(bytes("BSA"));  // 3 bytes
        CHECK_STATUS(r.open(t.path), Status::NotBsa);
    }
    {
        auto a = build({105, 0x3}, sample_files());
        patch32(a, 4, 106);
        TempFile t(a);
        CHECK_STATUS(r.open(t.path), Status::UnsupportedVersion);
    }
    {
        auto a = build({105, 0x3 | 0x40}, sample_files());
        TempFile t(a);
        CHECK_STATUS(r.open(t.path), Status::Unsupported);
    }
    {
        auto a = build({105, 0x3}, sample_files());
        a.resize(20);
        TempFile t(a);
        CHECK_STATUS(r.open(t.path), Status::Corrupt);
    }
}

static void test_corrupt_payloads() {
    // Find the compressed "dl" entry and tamper with its size prefix / stream.
    auto a = build({105, 0x103}, sample_files());
    bsa::Reader r;
    TempFile t0(a);
    CHECK_STATUS(r.open(t0.path), Status::Ok);
    bsa::Entry e;
    CHECK_STATUS(r.find("strings\\skyrim_english.dlstrings", e), Status::Ok);
    const size_t nameLen = a[e.dataOffset];
    const size_t sizeAt = static_cast<size_t>(e.dataOffset) + 1 + nameLen;
    r.close();

    {
        auto b = a;
        patch32(b, sizeAt, 0xFFFFFFF0u);
        TempFile t(b);
        CHECK_STATUS(r.open(t.path), Status::Ok);
        CHECK_STATUS(r.find("strings\\skyrim_english.dlstrings", e), Status::Ok);
        std::vector<uint8_t> buf;
        CHECK_STATUS(r.read(e, buf), Status::TooLarge);
        uint8_t small[16];
        size_t w;
        CHECK_STATUS(r.read(e, small, sizeof small, w), Status::BufferTooSmall);
    }
    {
        auto b = a;
        patch32(b, sizeAt, get32(a, sizeAt) + 1);  // claims one byte more than the stream holds
        TempFile t(b);
        CHECK_STATUS(r.open(t.path), Status::Ok);
        CHECK_STATUS(r.find("strings\\skyrim_english.dlstrings", e), Status::Ok);
        std::vector<uint8_t> buf;
        CHECK_STATUS(r.read(e, buf), Status::DecompressFailed);
    }
    {
        auto b = a;
        b[sizeAt + 4] ^= 0xFF;  // break the LZ4 magic
        TempFile t(b);
        CHECK_STATUS(r.open(t.path), Status::Ok);
        CHECK_STATUS(r.find("strings\\skyrim_english.dlstrings", e), Status::Ok);
        std::vector<uint8_t> buf;
        CHECK_STATUS(r.read(e, buf), Status::DecompressFailed);
    }
    {
        auto b = a;
        b[e.dataOffset] = 0xFF;  // embedded name longer than the block
        TempFile t(b);
        CHECK_STATUS(r.open(t.path), Status::Ok);
        CHECK_STATUS(r.find("strings\\skyrim_english.dlstrings", e), Status::Ok);
        std::vector<uint8_t> buf;
        CHECK_STATUS(r.read(e, buf), Status::Corrupt);
    }
    {
        // v104 with the zlib stored stream: flip the adler trailer.
        auto c = build({104, 0x7}, sample_files());
        TempFile t(c);
        CHECK_STATUS(r.open(t.path), Status::Ok);
        CHECK_STATUS(r.find("strings\\skyrim_english.dlstrings", e), Status::Ok);
        std::vector<uint8_t> buf;
        CHECK_STATUS(r.read(e, buf), Status::Ok);
        CHECK(buf == bytes("dl"));
        const uint8_t* raw;
        size_t n;
        CHECK_STATUS(r.raw(e, raw, n), Status::Ok);
        CHECK(n >= 2 && raw[0] == 0x78);  // raw() hands back the zlib stream itself
        r.close();
        auto d = build({104, 0x7}, {{"strings", "skyrim_english.dlstrings", bytes("dl"), true}});
        d[d.size() - 1] ^= 1;  // last byte of the archive is the adler trailer of the only file
        TempFile t2(d);
        CHECK_STATUS(r.open(t2.path), Status::Ok);
        CHECK_STATUS(r.find("strings\\skyrim_english.dlstrings", e), Status::Ok);
        CHECK_STATUS(r.read(e, buf), Status::DecompressFailed);
    }
}

static void test_truncation_and_fuzz() {
    const auto a = build({105, 0x103}, {{"strings", "a.strings", bytes("hello"), true}, {"meshes", "b.nif", bytes("nif"), false}});
    // Every truncation must be refused (the last byte belongs to a file's data).
    for (size_t n = 0; n < a.size(); ++n) {
        std::vector<uint8_t> cut(a.begin(), a.begin() + n);
        TempFile t(cut);
        bsa::Reader r;
        CHECK(r.open(t.path) != Status::Ok);
    }
    // Header fields set to extremes: any status, no crash, and whatever opens stays usable.
    static const uint32_t extremes[] = {0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 36, 0x40000000u};
    for (size_t field = 1; field < 9; ++field)
        for (uint32_t x : extremes) {
            auto b = a;
            patch32(b, field * 4, x);
            TempFile t(b);
            exercise(t.path);
        }
    // Random byte flips.
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
    for (int iter = 0; iter < 800; ++iter) {
        auto b = a;
        const int flips = 1 + static_cast<int>(rnd() % 4);
        for (int k = 0; k < flips; ++k) b[rnd() % b.size()] = static_cast<uint8_t>(rnd());
        TempFile t(b);
        exercise(t.path);
    }
}

static void test_codecs() {
    // Real LZ4 block: literals "abc" then a match of 9 at offset 3 -> "abcabcabcabc".
    const uint8_t frame[] = {0x04, 0x22, 0x4D, 0x18, 0x60, 0x40, 0x82,
                             0x06, 0x00, 0x00, 0x00, 0x35, 'a', 'b', 'c', 0x03, 0x00,
                             0x00, 0x00, 0x00, 0x00};
    uint8_t out[12];
    CHECK(bsa::detail::lz4_frame_decode(frame, sizeof frame, out, sizeof out));
    CHECK(std::memcmp(out, "abcabcabcabc", 12) == 0);
    CHECK(!bsa::detail::lz4_frame_decode(frame, sizeof frame, out, 11));
    CHECK(!bsa::detail::lz4_frame_decode(frame, sizeof frame - 1, out, 12));
    uint8_t bad[sizeof frame];
    std::memcpy(bad, frame, sizeof frame);
    bad[15] = 0x20;  // offset 32 reaches before the start of the output
    CHECK(!bsa::detail::lz4_frame_decode(bad, sizeof bad, out, 12));

    // Real zlib stream (Python zlib.compress, level 9) of a repetitive text.
    const uint8_t z[] = {0x78, 0xDA, 0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0x48, 0x2A, 0x4E, 0x54, 0x28, 0x4A, 0x4D,
                         0x4C, 0x49, 0x2D, 0x52, 0xC8, 0x18, 0x15, 0x18, 0x0C, 0x02, 0x00, 0x68, 0x37, 0x92, 0xB9};
    std::string want;
    for (int i = 0; i < 12; ++i) want += "hello bsa reader hello bsa reader ";
    std::vector<uint8_t> zout(want.size());
    CHECK(bsa::detail::zlib_inflate(z, sizeof z, zout.data(), zout.size()));
    CHECK(std::string(zout.begin(), zout.end()) == want);
    CHECK(!bsa::detail::zlib_inflate(z, sizeof z, zout.data(), zout.size() - 1));
    CHECK(!bsa::detail::zlib_inflate(z, sizeof z - 1, zout.data(), zout.size()));
    uint8_t zbad[sizeof z];
    std::memcpy(zbad, z, sizeof z);
    zbad[sizeof z - 1] ^= 1;
    CHECK(!bsa::detail::zlib_inflate(zbad, sizeof zbad, zout.data(), zout.size()));
    // Preset dictionary flag is refused.
    zbad[0] = 0x78;
    zbad[1] = 0x20 | (31 - ((0x78 << 8) | 0x20) % 31);
    CHECK(!bsa::detail::zlib_inflate(zbad, sizeof zbad, zout.data(), zout.size()));
}

int main() {
    test_hash_vectors();
    test_roundtrip(105, 0x3);
    test_roundtrip(105, 0x7);
    test_roundtrip(105, 0x103);
    test_roundtrip(104, 0x3);
    test_roundtrip(104, 0x107);
    test_roundtrip(103, 0x703);  // Oblivion sets 0x100 too; v103 must ignore it
    test_roundtrip(103, 0x707);
    test_unsorted_tables();
    test_no_names();
    test_rejections();
    test_corrupt_payloads();
    test_truncation_and_fuzz();
    test_codecs();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
