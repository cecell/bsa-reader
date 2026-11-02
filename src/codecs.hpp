// Internal decoders. Both write exactly dstLen bytes or fail; neither
// allocates, throws, or reads outside [src, src + srcLen).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>

namespace bsa::detail {

// zlib stream (RFC 1950 wrapper around RFC 1951 deflate). Used by v103/v104.
bool zlib_inflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) noexcept;

// LZ4 frame (one or more frames, skippable frames allowed). Used by v105.
bool lz4_frame_decode(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) noexcept;

}  // namespace bsa::detail
