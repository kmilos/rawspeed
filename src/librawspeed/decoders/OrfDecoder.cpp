/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014-2015 Pedro Côrte-Real

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

#include "decoders/OrfDecoder.h"
#include "common/Common.h"                          // for uint32, ushort16
#include "common/NORangesSet.h"                     // for NORangesSet
#include "common/Point.h"                           // for iPoint2D
#include "decoders/RawDecoderException.h"           // for RawDecoderExcept...
#include "decompressors/OlympusDecompressor.h"      // for OlympusDecompressor
#include "decompressors/UncompressedDecompressor.h" // for UncompressedDeco...
#include "io/BitPumpMSB.h"                          // for BitPumpMSB
#include "io/Buffer.h"                              // for Buffer
#include "io/ByteStream.h"                          // for ByteStream
#include "io/Endianness.h"                          // for Endianness
#include "io/IOException.h"                         // for IOException
#include "metadata/Camera.h"                        // for Hints
#include "metadata/ColorFilterArray.h"              // for ColorFilterArray
#include "tiff/TiffEntry.h"                         // for TiffEntry
#include "tiff/TiffIFD.h"                           // for TiffRootIFD, Tif...
#include "tiff/TiffTag.h"                           // for TiffTag, TiffTag...
#include <algorithm>                                // for min
#include <cassert>                                  // for assert
#include <cmath>                                    // for signbit
#include <cstdlib>                                  // for abs
#include <memory>                                   // for unique_ptr
#include <string>                                   // for operator==, string

namespace rawspeed {

class CameraMetaData;

bool OrfDecoder::isAppropriateDecoder(const TiffRootIFD* rootIFD,
                                      const Buffer* file) {
  const auto id = rootIFD->getID();
  const std::string& make = id.make;

  // FIXME: magic

  return make == "OLYMPUS IMAGING CORP." || make == "OLYMPUS CORPORATION" ||
         make == "OLYMPUS OPTICAL CO.,LTD";
}

RawImage OrfDecoder::decodeRawInternal() {
  auto raw = mRootIFD->getIFDWithTag(STRIPOFFSETS);

  int compression = raw->getEntry(COMPRESSION)->getU32();
  if (1 != compression)
    ThrowRDE("Unsupported compression");

  TiffEntry *offsets = raw->getEntry(STRIPOFFSETS);
  TiffEntry *counts = raw->getEntry(STRIPBYTECOUNTS);

  if (counts->count != offsets->count) {
    ThrowRDE(
        "Byte count number does not match strip size: count:%u, strips:%u ",
        counts->count, offsets->count);
  }

  //TODO: this code assumes that all strips are layed out directly after another without padding and in order
  uint32 off = raw->getEntry(STRIPOFFSETS)->getU32();
  uint32 size = 0;
  for (uint32 i=0; i < counts->count; i++)
    size += counts->getU32(i);

  if (!mFile->isValid(off, size))
    ThrowRDE("Truncated file");

  uint32 width = raw->getEntry(IMAGEWIDTH)->getU32();
  uint32 height = raw->getEntry(IMAGELENGTH)->getU32();

  if (!width || !height || width % 2 != 0 || width > 9280 || height > 6932)
    ThrowRDE("Unexpected image dimensions found: (%u; %u)", width, height);

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  ByteStream input(offsets->getRootIfdData());
  input.setPosition(off);

  if (offsets->count != 1 || hints.has("force_uncompressed"))
    decodeUncompressed(input, width, height, size);
  else {
    OlympusDecompressor o(mRaw);
    o.decompress(std::move(input));
  }

  return mRaw;
}

void OrfDecoder::decodeUncompressed(const ByteStream& s, uint32 w, uint32 h,
                                    uint32 size) {
  UncompressedDecompressor u(s, mRaw);
  if (hints.has("packed_with_control"))
    u.decode12BitRaw<Endianness::little, false, true>(w, h);
  else if (hints.has("jpeg32_bitorder")) {
    iPoint2D dimensions(w, h);
    iPoint2D pos(0, 0);
    u.readUncompressedRaw(dimensions, pos, w * 12 / 8, 12, BitOrder_MSB32);
  } else if (size >= w * h * 2) { // We're in an unpacked raw
    // FIXME: seems fishy
    if (s.getByteOrder() == getHostEndianness())
      u.decodeRawUnpacked<12, Endianness::little>(w, h);
    else
      u.decode12BitRawUnpackedLeftAligned<Endianness::big>(w, h);
  } else if (size >= w*h*3/2) { // We're in one of those weird interlaced packed raws
    u.decode12BitRaw<Endianness::big, true>(w, h);
  } else {
    ThrowRDE("Don't know how to handle the encoding in this file");
  }
}

void OrfDecoder::decodeMetaDataInternal(const CameraMetaData* meta) {
  int iso = 0;
  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_RED, CFA_GREEN, CFA_GREEN, CFA_BLUE);

  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getU32();

  setMetaData(meta, "", iso);

  if (mRootIFD->hasEntryRecursive(OLYMPUSREDMULTIPLIER) &&
      mRootIFD->hasEntryRecursive(OLYMPUSBLUEMULTIPLIER)) {
    mRaw->metadata.wbCoeffs[0] = static_cast<float>(
        mRootIFD->getEntryRecursive(OLYMPUSREDMULTIPLIER)->getU16());
    mRaw->metadata.wbCoeffs[1] = 256.0F;
    mRaw->metadata.wbCoeffs[2] = static_cast<float>(
        mRootIFD->getEntryRecursive(OLYMPUSBLUEMULTIPLIER)->getU16());
  } else if (mRootIFD->hasEntryRecursive(OLYMPUSIMAGEPROCESSING)) {
    // Newer cameras process the Image Processing SubIFD in the makernote
    TiffEntry* img_entry = mRootIFD->getEntryRecursive(OLYMPUSIMAGEPROCESSING);
    // get makernote ifd with containing Buffer
    NORangesSet<Buffer> ifds;

    TiffRootIFD image_processing(nullptr, &ifds, img_entry->getRootIfdData(),
                                 img_entry->getU32());

    // Get the WB
    if (image_processing.hasEntry(static_cast<TiffTag>(0x0100))) {
      TiffEntry* wb = image_processing.getEntry(static_cast<TiffTag>(0x0100));
      if (wb->count == 2 || wb->count == 4) {
        mRaw->metadata.wbCoeffs[0] = wb->getFloat(0);
        mRaw->metadata.wbCoeffs[1] = 256.0F;
        mRaw->metadata.wbCoeffs[2] = wb->getFloat(1);
      }
    }

    // Get the black levels
    if (image_processing.hasEntry(static_cast<TiffTag>(0x0600))) {
      TiffEntry* blackEntry =
          image_processing.getEntry(static_cast<TiffTag>(0x0600));
      // Order is assumed to be RGGB
      if (blackEntry->count == 4) {
        for (int i = 0; i < 4; i++) {
          auto c = mRaw->cfa.getColorAt(i & 1, i >> 1);
          int j;
          switch (c) {
          case CFA_RED:
            j = 0;
            break;
          case CFA_GREEN:
            j = i < 2 ? 1 : 2;
            break;
          case CFA_BLUE:
            j = 3;
            break;
          default:
            ThrowRDE("Unexpected CFA color: %u", c);
          }

          mRaw->blackLevelSeparate[i] = blackEntry->getU16(j);
        }
        // Adjust whitelevel based on the read black (we assume the dynamic
        // range is the same)
        mRaw->whitePoint -= (mRaw->blackLevel - mRaw->blackLevelSeparate[0]);
      }
    }
  }
}

} // namespace rawspeed
