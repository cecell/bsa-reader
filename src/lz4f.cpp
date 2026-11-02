// LZ4 frame decoder (spec 1.6.x) into a fixed buffer. Skyrim SE archives
// store each compressed file as one LZ4 frame. Block and content checksums
// are skipped; the output length is checked instead.
// SPDX-License-Identifier: GPL-3.0-only
#include "codecs.hpp"

#include <cstring>

namespace bsa::detail {
namespace {

constexpr uint32_t kFrameMagic = 0x184D2204u;
constexpr uint32_t kSkippableMask = 0xFFFFFFF0u;
constexpr uint32_t kSkippableMagic = 0x184D2A50u;

inline uint32_t rd32(const uint8_t* p) noexcept {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

// One LZ4 block. Matches may reach back into earlier blocks because the
// whole output lives in one buffer (block-dependent frames need that).
bool block(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen, size_t& op) noexcept {
    size_t ip = 0;
    while (ip < srcLen) {
        const uint8_t token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= srcLen) return false;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > srcLen - ip || lit > dstLen - op) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip == srcLen) break;  // final sequence carries literals only
        if (srcLen - ip < 2) return false;
        const size_t off = src[ip] | (static_cast<size_t>(src[ip + 1]) << 8);
        ip += 2;
        if (off == 0 || off > op) return false;
        size_t ml = token & 15;
        if (ml == 15) {
            uint8_t b;
            do {
                if (ip >= srcLen) return false;
                b = src[ip++];
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (ml > dstLen - op) return false;
        const uint8_t* m = dst + op - off;
        for (size_t i = 0; i < ml; ++i) dst[op + i] = m[i];  // overlap-safe
        op += ml;
    }
    return true;
}

}  // namespace

bool lz4_frame_decode(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) noexcept {
    size_t ip = 0, op = 0;
    while (ip < srcLen) {
        if (op == dstLen) return true;  // tolerate trailing bytes once the file is complete
        if (srcLen - ip < 4) return false;
        const uint32_t magic = rd32(src + ip);
        ip += 4;
        if ((magic & kSkippableMask) == kSkippableMagic) {
            if (srcLen - ip < 4) return false;
            const uint32_t n = rd32(src + ip);
            ip += 4;
            if (n > srcLen - ip) return false;
            ip += n;
            continue;
        }
        if (magic != kFrameMagic) return false;
        if (srcLen - ip < 3) return false;  // FLG, BD, HC at minimum
        const uint8_t flg = src[ip], bd = src[ip + 1];
        ip += 2;
        if ((flg >> 6) != 1 || (flg & 0x02) || (flg & 0x01) || (bd & 0x8F)) return false;  // version, reserved, dict id, reserved
        const bool blockChecksum = flg & 0x10, contentSize = flg & 0x08, contentChecksum = flg & 0x04;
        const unsigned bmax = (bd >> 4) & 7;
        if (bmax < 4) return false;
        const size_t maxBlock = size_t{64} << (10 + 2 * (bmax - 4));  // 64 KiB .. 4 MiB
        if (contentSize) {
            if (srcLen - ip < 8) return false;
            const uint64_t cs = uint64_t{rd32(src + ip)} | (uint64_t{rd32(src + ip + 4)} << 32);
            if (cs > dstLen - op) return false;
            ip += 8;
        }
        if (srcLen - ip < 1) return false;
        ip += 1;  // header checksum, not verified
        for (;;) {
            if (srcLen - ip < 4) return false;
            uint32_t bs = rd32(src + ip);
            ip += 4;
            if (bs == 0) break;  // end mark
            const bool raw = bs & 0x80000000u;
            bs &= 0x7FFFFFFFu;
            if (bs > maxBlock || bs > srcLen - ip) return false;
            if (raw) {
                if (bs > dstLen - op) return false;
                std::memcpy(dst + op, src + ip, bs);
                op += bs;
            } else if (!block(src + ip, bs, dst, dstLen, op)) {
                return false;
            }
            ip += bs;
            if (blockChecksum) {
                if (srcLen - ip < 4) return false;
                ip += 4;
            }
        }
        if (contentChecksum) {
            if (srcLen - ip < 4) return false;
            ip += 4;
        }
    }
    return op == dstLen;
}

}  // namespace bsa::detail
