// Engine file/folder hashes for v103-v105 archives ("TES4 hash").
//
// Layout of the 64-bit hash:
//   bits  0..31  hash1: last char | second-to-last << 8 | length << 16 | first << 24,
//                plus a marker for .kf / .nif / .dds / .wav extensions
//   bits 32..63  hash2 + hash3: sdbm-style sums over the middle of the stem and
//                over the extension (dot included)
// Only ASCII letters are lowercased; bytes above 0x7F pass through unchanged
// and enter the sums unsigned (verified on Skyrim SE archives). v103
// (Oblivion) is believed to feed them as signed values, following xEdit; the
// shipped Oblivion archives contain no such names, so that part is unverified.
//
// SPDX-License-Identifier: GPL-3.0-only
#include "bsa/reader.hpp"

namespace bsa {
namespace {

inline uint8_t norm(char c) noexcept {
    const auto b = static_cast<uint8_t>(c);
    if (b == '/') return '\\';
    if (b >= 'A' && b <= 'Z') return static_cast<uint8_t>(b + 32);
    return b;
}

inline uint32_t accumulate(uint32_t h, uint8_t b, bool signedBytes) noexcept {
    uint32_t c = b;
    if (signedBytes && b > 0x7F) c -= 256u;  // wraps, same as adding int8_t(b)
    return c + (h << 6) + (h << 16) - h;
}

inline bool ext_is(std::string_view ext, std::string_view lit) noexcept {
    if (ext.size() != lit.size()) return false;
    for (size_t i = 0; i < ext.size(); ++i)
        if (norm(ext[i]) != static_cast<uint8_t>(lit[i])) return false;
    return true;
}

uint64_t hash_parts(std::string_view stem, std::string_view ext, uint32_t version) noexcept {
    const bool signedBytes = version == 103;
    const size_t len = stem.size();

    uint32_t h1 = 0;
    if (len > 0) h1 |= norm(stem[len - 1]);
    if (len > 2) h1 |= static_cast<uint32_t>(norm(stem[len - 2])) << 8;
    h1 |= static_cast<uint32_t>(len & 0xFF) << 16;
    if (len > 0) h1 |= static_cast<uint32_t>(norm(stem[0])) << 24;

    // XOR, not OR: identical for ASCII names, but the shipped packers flip the
    // bit when the character already has it set (verified on names with
    // bytes above 0x7F in third-party Skyrim SE archives).
    if (ext_is(ext, ".kf")) h1 ^= 0x80;
    else if (ext_is(ext, ".nif")) h1 ^= 0x8000;
    else if (ext_is(ext, ".dds")) h1 ^= 0x8080;
    else if (ext_is(ext, ".wav")) h1 ^= 0x80000000u;

    uint32_t h2 = 0;
    for (size_t i = 1; i + 2 < len; ++i) h2 = accumulate(h2, norm(stem[i]), signedBytes);
    uint32_t h3 = 0;
    for (char c : ext) h3 = accumulate(h3, norm(c), signedBytes);

    return h1 | (uint64_t{h2 + h3} << 32);
}

}  // namespace

uint64_t hash_folder(std::string_view folder, uint32_t version) noexcept {
    while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/')) folder.remove_suffix(1);
    return hash_parts(folder, {}, version);
}

uint64_t hash_file(std::string_view fileName, uint32_t version) noexcept {
    const size_t dot = fileName.rfind('.');
    if (dot == std::string_view::npos) return hash_parts(fileName, {}, version);
    return hash_parts(fileName.substr(0, dot), fileName.substr(dot), version);
}

}  // namespace bsa
