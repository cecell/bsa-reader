// bsatool: info, list, extract and check for BSA archives.
// SPDX-License-Identifier: GPL-3.0-only
#include "bsa/reader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int usage() {
    std::fputs(
        "usage:\n"
        "  bsatool info <archive>\n"
        "  bsatool list <archive>\n"
        "  bsatool extract <archive> <path-in-archive> [out-file]\n"
        "  bsatool check <archive-or-directory> [--read-all]\n"
        "\n"
        "check: opens every archive, recomputes every folder and file hash from the\n"
        "name tables and compares with the stored ones, looks every path up by hash,\n"
        "and decodes a sample of files (all of them with --read-all).\n",
        stderr);
    return 2;
}

bool open_or_report(bsa::Reader& r, const fs::path& p) {
    const bsa::Status s = r.open(p);
    if (s != bsa::Status::Ok) {
        std::fprintf(stderr, "%s: %s\n", p.string().c_str(), bsa::to_string(s));
        return false;
    }
    return true;
}

std::string flag_names(uint32_t f) {
    static const char* names[] = {"folder-names", "file-names", "compressed", "retain-folder-names",
                                  "retain-file-names", "retain-file-name-offsets", "xbox360",
                                  "retain-strings", "embedded-names", "xmem"};
    std::string s;
    for (int i = 0; i < 10; ++i)
        if (f & (1u << i)) s += std::string(s.empty() ? "" : " ") + names[i];
    return s;
}

int cmd_info(const fs::path& p) {
    bsa::Reader r;
    if (!open_or_report(r, p)) return 1;
    const bsa::Header& h = r.header();
    uint32_t compressed = 0;
    for (uint32_t i = 0; i < h.fileCount; ++i) {
        bsa::Entry e;
        if (r.entry(i, e) == bsa::Status::Ok && e.compressed) ++compressed;
    }
    std::printf("version        %u\nfolders        %u\nfiles          %u\ncompressed     %u\narchive flags  0x%03x  %s\nfile flags     0x%03x\nsize           %llu\n",
                h.version, h.folderCount, h.fileCount, compressed, h.archiveFlags, flag_names(h.archiveFlags).c_str(),
                h.fileFlags, static_cast<unsigned long long>(h.archiveSize));
    return 0;
}

int cmd_list(const fs::path& p) {
    bsa::Reader r;
    if (!open_or_report(r, p)) return 1;
    const bsa::Header& h = r.header();
    for (uint32_t i = 0; i < h.fileCount; ++i) {
        bsa::Entry e;
        if (r.entry(i, e) != bsa::Status::Ok) continue;
        std::string_view folder, name;
        uint32_t size = 0;
        r.folder_name(e.folderIndex, folder);
        r.file_name(e, name);
        r.uncompressed_size(e, size);
        std::printf("%c %10u  %.*s\\%.*s\n", e.compressed ? 'c' : ' ', size, static_cast<int>(folder.size()), folder.data(),
                    static_cast<int>(name.size()), name.data());
    }
    return 0;
}

int cmd_extract(const fs::path& p, const char* inner, const char* outArg) {
    bsa::Reader r;
    if (!open_or_report(r, p)) return 1;
    bsa::Entry e;
    bsa::Status s = r.find(inner, e);
    if (s != bsa::Status::Ok) {
        std::fprintf(stderr, "%s: %s\n", inner, bsa::to_string(s));
        return 1;
    }
    std::vector<uint8_t> data;
    s = r.read(e, data, size_t{1} << 31);
    if (s != bsa::Status::Ok) {
        std::fprintf(stderr, "%s: %s\n", inner, bsa::to_string(s));
        return 1;
    }
    fs::path out = outArg ? fs::path(outArg) : fs::path(inner).filename();
    FILE* f = std::fopen(out.string().c_str(), "wb");
    if (!f || std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
        std::fprintf(stderr, "cannot write %s\n", out.string().c_str());
        if (f) std::fclose(f);
        return 1;
    }
    std::fclose(f);
    std::printf("%s: %zu bytes\n", out.string().c_str(), data.size());
    return 0;
}

struct Totals {
    size_t archives = 0, failedOpen = 0, files = 0, hashMismatch = 0, lookupMiss = 0, reads = 0, readFail = 0;
};

void check_one(const fs::path& p, bool readAll, Totals& t) {
    ++t.archives;
    bsa::Reader r;
    if (!open_or_report(r, p)) {
        ++t.failedOpen;
        return;
    }
    const bsa::Header& h = r.header();
    size_t hashMismatch = 0, lookupMiss = 0, reads = 0, readFail = 0, sampled[2] = {0, 0};
    std::vector<uint8_t> buf;
    std::string path;
    for (uint32_t i = 0; i < h.fileCount; ++i) {
        bsa::Entry e;
        if (r.entry(i, e) != bsa::Status::Ok) {
            ++lookupMiss;
            continue;
        }
        std::string_view folder, name;
        if (r.folder_name(e.folderIndex, folder) == bsa::Status::Ok && r.file_name(e, name) == bsa::Status::Ok) {
            if (bsa::hash_folder(folder, h.version) != e.folderHash || bsa::hash_file(name, h.version) != e.fileHash) {
                ++hashMismatch;
                if (hashMismatch <= 3)
                    std::fprintf(stderr, "  hash mismatch: %.*s\\%.*s\n", static_cast<int>(folder.size()), folder.data(),
                                 static_cast<int>(name.size()), name.data());
            }
            path.assign(folder).append("\\").append(name);
            bsa::Entry f;
            if (r.find(path, f) != bsa::Status::Ok || f.fileIndex != e.fileIndex) ++lookupMiss;
        }
        const int kind = e.compressed ? 1 : 0;
        if (readAll || sampled[kind] < 4) {
            ++sampled[kind];
            ++reads;
            const bsa::Status s = r.read(e, buf);
            if (s != bsa::Status::Ok) {
                ++readFail;
                if (readFail <= 3)
                    std::fprintf(stderr, "  read failed (%s): %.*s\\%.*s\n", bsa::to_string(s), static_cast<int>(folder.size()),
                                 folder.data(), static_cast<int>(name.size()), name.data());
            }
        }
    }
    std::printf("%-50s v%u %7u files  hash-mismatch %zu  lookup-miss %zu  reads %zu  read-fail %zu\n",
                p.filename().string().c_str(), h.version, h.fileCount, hashMismatch, lookupMiss, reads, readFail);
    t.files += h.fileCount;
    t.hashMismatch += hashMismatch;
    t.lookupMiss += lookupMiss;
    t.reads += reads;
    t.readFail += readFail;
}

int cmd_check(const fs::path& p, bool readAll) {
    const auto start = std::chrono::steady_clock::now();
    Totals t;
    if (fs::is_directory(p)) {
        std::vector<fs::path> files;
        for (const auto& d : fs::directory_iterator(p)) {
            std::string ext = d.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (d.is_regular_file() && ext == ".bsa") files.push_back(d.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) check_one(f, readAll, t);
    } else {
        check_one(p, readAll, t);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("\narchives %zu (failed to open %zu)  files %zu  hash-mismatch %zu  lookup-miss %zu  reads %zu  read-fail %zu  %.2fs\n",
                t.archives, t.failedOpen, t.files, t.hashMismatch, t.lookupMiss, t.reads, t.readFail, secs);
    return (t.failedOpen || t.hashMismatch || t.lookupMiss || t.readFail) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    const fs::path p = fs::u8path(argv[2]);
    if (cmd == "info") return cmd_info(p);
    if (cmd == "list") return cmd_list(p);
    if (cmd == "extract" && argc >= 4) return cmd_extract(p, argv[3], argc >= 5 ? argv[4] : nullptr);
    if (cmd == "check") return cmd_check(p, argc >= 4 && std::strcmp(argv[3], "--read-all") == 0);
    return usage();
}
