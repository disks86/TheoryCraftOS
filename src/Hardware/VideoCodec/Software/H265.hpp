/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


// An H.265 / HEVC decoder on the CPU: Main profile video (8-bit 4:2:0) with every coding tool of the Main profile - coding trees of 8x8 to 64x64 blocks, all intra
// modes, merge and AMVP motion prediction with temporal candidates, weighted prediction, transform skip, scaling lists, sign data hiding, PCM, lossless blocks, the
// deblocking filter and sample adaptive offset, slices, dependent slice segments, tiles and wavefront substreams. Not covered: bit depths above 8, other chroma
// formats, the range, multilayer and screen content extensions. Pictures come out in display order, cropped to the conformance window, as I420.
//
// Input is either Annex B (start codes, with VPS/SPS/PPS in the stream) or - after configure() with an hvcC record, as stored in MP4 files - length-prefixed NAL units.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/VideoTypes.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace h265 {
class Core;
}

// MD5 of one plane of samples, for checking decoded picture hashes: the decoder does not carry an MD5 implementation of its own (the OS has one in Crypto, which
// programs that want the check hand in through this interface).
class IPlaneHasher {
 public:
  virtual void md5(const uint8_t* plane, uint32_t width, uint32_t height, uint32_t stride, uint8_t out[16]) = 0;

 protected:
  constexpr IPlaneHasher() = default;
  ~IPlaneHasher() = default;
};

class H265Decoder {
 public:
  explicit H265Decoder(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~H265Decoder();
  H265Decoder(const H265Decoder&) = delete;
  H265Decoder& operator=(const H265Decoder&) = delete;

  // Takes the parameter sets out of an hvcC record (ISO 14496-15) and switches the input to length-prefixed NAL units.
  bool configure(ConstByteView hvcc);
  // Decodes one access unit (all NAL units of a picture, or any number of whole NAL units). False if the data holds nothing decodable.
  bool decode(ConstByteView access_unit, int64_t pts_us);
  // The next picture in display order, as I420 (cropped). The planes stay valid until the next call on the decoder.
  bool receive(VideoFrame* frame);
  // The stream ends: pictures held for reordering become available.
  void flush();
  // Drops everything and waits for an IRAP picture (after a seek).
  void reset();
  // The size of the pictures; zero until a sequence parameter set was seen.
  VideoFormat output_format() const;
  bool ready() const;  // a picture can be received

  // Streams may carry a hash of every decoded picture (SEI "decoded picture hash": MD5, CRC or checksum of each plane). The decoder checks the pictures it makes
  // against them: `checked` counts the pictures that had a hash, `mismatched` those the hash did not agree with. A conforming decoder has none of the latter.
  struct HashStatistics {
    uint64_t checked = 0, mismatched = 0;
  };
  HashStatistics hash_statistics() const;
  // Lets the decoder check MD5 hashes (CRC and checksum hashes need no help). Without it pictures with an MD5 hash are not counted as checked.
  void set_plane_hasher(IPlaneHasher* hasher);

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<h265::Core> core_;
};

}  // namespace hardware
