/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2017 Axel Waggershauser

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#pragma once

#include "adt/Array1DRef.h"
#include "adt/Invariant.h"
#include "io/BitStreamer.h"
#include "io/Endianness.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>

namespace rawspeed {

struct JPEGBitStreamerTag;

// The JPEG data is ordered in MSB bit order,
// i.e. we push into the cache from the right and read it from the left
using BitStreamerJPEG =
    BitStreamer<JPEGBitStreamerTag, BitStreamerCacheRightInLeftOut>;

template <> struct BitStreamerTraits<JPEGBitStreamerTag> final {
  static constexpr bool canUseWithPrefixCodeDecoder = true;

  // How many bytes can we read from the input per each fillCache(), at most?
  // Normally, we want to read 4 bytes, but at worst each one of those can be
  // an 0xFF byte, separated by 0x00 byte, signifying that 0xFF is a data byte.
  static constexpr int MaxProcessBytes = 8;
  static_assert(MaxProcessBytes == sizeof(uint64_t));
};

// NOTE: on average, probability of encountering an `0xFF` byte
// is ~0.51% (1 in ~197), only ~2.02% (1 in ~50) of 4-byte blocks will contain
// an `0xFF` byte, and out of *those* blocks, only ~0.77% (1 in ~131)
// will contain more than one `0xFF` byte.

template <>
inline BitStreamerJPEG::size_type
BitStreamerJPEG::fillCache(Array1DRef<const uint8_t> input) {
  static_assert(BitStreamerCacheBase::MaxGetBits >= 32, "check implementation");
  establishClassInvariants();
  invariant(input.size() == BitStreamerTraits<tag>::MaxProcessBytes);

  std::array<uint8_t, BitStreamerTraits<JPEGBitStreamerTag>::MaxProcessBytes>
      prefetch;
  std::copy_n(input.getCrop(0, sizeof(uint64_t)).begin(), prefetch.size(),
              prefetch.begin());

  auto speculativeOptimisticCache = cache;
  speculativeOptimisticCache.push(getBE<uint32_t>(prefetch.data()), 32);

  // short-cut path for the most common case (no FF marker in the next 4 bytes)
  // this is slightly faster than the else-case alone.
  if (std::accumulate(
          &prefetch[0], &prefetch[4], bool(true),
          [](bool b, uint8_t byte) { return b && (byte != 0xFF); })) {
    cache = speculativeOptimisticCache;
    return 4;
  }

  size_type p = 0;
  for (size_type i = 0; i < 4; ++i) {
    // Pre-execute most common case, where next byte is 'normal'/non-FF
    const int c0 = prefetch[p + 0];
    cache.push(c0, 8);
    if (c0 != 0xFF) {
      p += 1;
      continue; // Got normal byte.
    }

    // Found FF -> pre-execute case of FF/00, which represents an FF data byte
    const int c1 = prefetch[p + 1];
    if (c1 == 0x00) {
      // Got FF/00, where 0x00 is a stuffing byte (that should be ignored),
      // so 0xFF is a normal byte. All good.
      p += 2;
      continue;
    }

    // Found FF/xx with xx != 00. This is the end of stream marker.
    // That means we shouldn't have pushed last 8 bits (0xFF, from c0).
    // We need to "unpush" them, and fill the vacant cache bits with zeros.

    // First, recover the cache fill level.
    cache.fillLevel -= 8;
    // Now, this code is incredibly underencapsulated, and the
    // implementation details are leaking into here. Thus, we know that
    // all the fillLevel bits in cache are all high bits. So to "unpush"
    // the last 8 bits, and fill the vacant cache bits with zeros, we only
    // need to keep the high fillLevel bits. So just create a mask with only
    // high fillLevel bits set, and 'and' the cache with it.
    // Caution, we know fillLevel won't be 64, but it may be 0,
    // so pick the mask-creation idiom accordingly.
    cache.cache &= ~((~0ULL) >> cache.fillLevel);
    cache.fillLevel = 64;

    // This buffer has been exhausted. While it is incredibly tempting to
    // signal *that* by claiming that we have consumed all the remaining bytes
    // of the buffer, we can't actually do that, because the caller code
    // may depend on the position of the end-of-stream marker / marker itself.
    break;
  }

  invariant(p >= 0);
  invariant(p <= 8);
  // NOTE: `p` may be `0`!

  return p;
}

template <>
inline BitStreamerJPEG::size_type BitStreamerJPEG::getStreamPosition() const {
  // the current number of bytes we consumed -> at the end of the stream pos, it
  // points to the JPEG marker FF
  return getInputPosition();
}

} // namespace rawspeed