// zlib inflate into a fixed buffer. Canonical-Huffman decoder in the style
// of zlib's puff.c; adapted from the author's Plugin Shell Extension.
// SPDX-License-Identifier: GPL-3.0-only
#include "codecs.hpp"

#include <cstring>

namespace bsa::detail {
namespace {

constexpr int kMaxBits = 15;

struct Huffman {
    short count[kMaxBits + 1];
    short symbol[288];
};

struct State {
    const uint8_t* in;
    size_t inLen;
    size_t inPos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    uint8_t* out;
    size_t outLen;
    size_t outPos = 0;
    bool error = false;

    int bits(int need) noexcept {
        uint32_t val = bitBuf;
        while (bitCnt < need) {
            if (inPos >= inLen) {
                error = true;
                return 0;
            }
            val |= static_cast<uint32_t>(in[inPos++]) << bitCnt;
            bitCnt += 8;
        }
        bitBuf = val >> need;
        bitCnt -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }

    int decode(const Huffman& h) noexcept {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            if (error) return -1;
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

// Builds a canonical decoder; false for an over-subscribed length set.
bool construct(Huffman* h, const short* length, int n) noexcept {
    for (int len = 0; len <= kMaxBits; ++len) h->count[len] = 0;
    for (int s = 0; s < n; ++s) h->count[length[s]]++;
    if (h->count[0] == n) return true;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return false;
    }
    short offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<short>(offs[len] + h->count[len]);
    for (int s = 0; s < n; ++s)
        if (length[s] != 0) h->symbol[offs[length[s]]++] = static_cast<short>(s);
    return true;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                             257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(State& s, const Huffman& lencode, const Huffman& distcode) noexcept {
    for (;;) {
        int symbol = s.decode(lencode);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (s.outPos >= s.outLen) return false;
            s.out[s.outPos++] = static_cast<uint8_t>(symbol);
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            const size_t len = static_cast<size_t>(kLenBase[symbol] + s.bits(kLenExtra[symbol]));
            const int dsym = s.decode(distcode);
            if (dsym < 0 || dsym >= 30) return false;
            const size_t dist = static_cast<size_t>(kDistBase[dsym] + s.bits(kDistExtra[dsym]));
            if (s.error || dist > s.outPos || len > s.outLen - s.outPos) return false;
            const uint8_t* from = s.out + s.outPos - dist;
            for (size_t k = 0; k < len; ++k) s.out[s.outPos + k] = from[k];  // overlap-safe
            s.outPos += len;
        }
    }
}

bool stored(State& s) noexcept {
    s.bitBuf = 0;
    s.bitCnt = 0;
    if (s.inLen - s.inPos < 4) return false;
    const unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
    const unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xFFFF)) return false;
    if (len > s.inLen - s.inPos || len > s.outLen - s.outPos) return false;
    std::memcpy(s.out + s.outPos, s.in + s.inPos, len);
    s.inPos += len;
    s.outPos += len;
    return true;
}

bool fixed(State& s) noexcept {
    static const struct Tables {
        Huffman len, dist;
        Tables() noexcept {
            short lengths[288];
            int sym = 0;
            for (; sym < 144; ++sym) lengths[sym] = 8;
            for (; sym < 256; ++sym) lengths[sym] = 9;
            for (; sym < 280; ++sym) lengths[sym] = 7;
            for (; sym < 288; ++sym) lengths[sym] = 8;
            construct(&len, lengths, 288);
            for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
            construct(&dist, lengths, 30);
        }
    } t;
    return codes(s, t.len, t.dist);
}

bool dynamic(State& s) noexcept {
    static const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    const int nlen = s.bits(5) + 257;
    const int ndist = s.bits(5) + 1;
    const int ncode = s.bits(4) + 4;
    if (s.error || nlen > 286 || ndist > 30) return false;
    int index = 0;
    for (; index < ncode; ++index) lengths[kOrder[index]] = static_cast<short>(s.bits(3));
    for (; index < 19; ++index) lengths[kOrder[index]] = 0;
    Huffman lencode, distcode;
    if (s.error || !construct(&lencode, lengths, 19)) return false;
    index = 0;
    while (index < nlen + ndist) {
        const int symbol = s.decode(lencode);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<short>(symbol);
        } else {
            short len = 0;
            int repeat;
            if (symbol == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                repeat = 3 + s.bits(2);
            } else if (symbol == 17) {
                repeat = 3 + s.bits(3);
            } else {
                repeat = 11 + s.bits(7);
            }
            if (s.error || index + repeat > nlen + ndist) return false;
            while (repeat--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) return false;
    if (!construct(&lencode, lengths, nlen)) return false;
    if (!construct(&distcode, lengths + nlen, ndist)) return false;
    return codes(s, lencode, distcode);
}

uint32_t adler32(const uint8_t* p, size_t n) noexcept {
    uint32_t a = 1, b = 0;
    while (n > 0) {
        size_t chunk = n < 5552 ? n : 5552;  // largest block before overflow
        n -= chunk;
        while (chunk--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

}  // namespace

bool zlib_inflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) noexcept {
    if (srcLen < 6) return false;
    const unsigned cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;
    State s{src + 2, srcLen - 2};
    s.out = dst;
    s.outLen = dstLen;
    int last;
    do {
        last = s.bits(1);
        const int type = s.bits(2);
        if (s.error) return false;
        const bool ok = type == 0 ? stored(s) : type == 1 ? fixed(s) : type == 2 ? dynamic(s) : false;
        if (!ok || s.error) return false;
    } while (!last);
    if (s.outPos != dstLen) return false;
    // Trailer: big-endian Adler-32 of the output, byte-aligned.
    if (s.inLen - s.inPos < 4) return false;
    const uint8_t* t = s.in + s.inPos;
    const uint32_t want = (uint32_t{t[0]} << 24) | (uint32_t{t[1]} << 16) | (uint32_t{t[2]} << 8) | t[3];
    return adler32(dst, dstLen) == want;
}

}  // namespace bsa::detail
