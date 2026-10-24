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


#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnAvcDecoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnHevcDecoder.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// The check that decides whether the OS trusts the VCN engine to decode H.264: a short stream this OS encodes itself (a 64 x 64 picture, one I picture and four P
// pictures, baseline profile) is decoded by the CPU decoder and by the engine, and the pictures must be byte for byte the same. H.264 decoding is exact, so a
// difference means the engine was not driven correctly (a wrong address, a pitch the firmware reads another way, a reference it did not find) and it is not used.
//
// What passing proves: the engine took a session, a message, the bitstream and the buffers, found its references, and wrote linear NV12 where it was told. What it
// does not: CABAC, B pictures, weighted prediction, the deblocking filter (the stream switches it off), 8x8 transform, and picture sizes other than the test's.
class AmdVcnSelfTest {
 public:
  enum class Result : uint8_t { Passed, NoMemory, StreamFailed, SessionRefused, EngineFailed, Mismatch };
  static constexpr uint32_t kPictures = 5;
  static constexpr uint32_t kSize = 64;

  // The stream: kPictures access units in Annex B, the first with the parameter sets. `ends[i]` is where access unit i ends in `stream` (access unit 0 starts at 0).
  struct Stream {
    explicit Stream(allocators::IAllocator& allocator) : bytes(allocator) {}
    ByteBuffer bytes;
    uint64_t ends[kPictures] = {};
    uint32_t count = 0;
    ConstByteView packet(uint32_t index) const {
      const uint64_t start = index == 0 ? 0 : ends[index - 1];
      return bytes.view().subview(start, ends[index] - start);
    }
  };
  static bool build_stream(allocators::IAllocator& allocator, Stream* stream);

  // Decodes the stream on the engine and on the CPU and compares. The packets go in with presentation times 0, 1000, 2000... microseconds.
  static Result run(allocators::IAllocator& allocator, const AmdVcnAvcDecoder::Engine& engine);
  // The same for H.265: a stream made by libx265 (64 x 64, six pictures, B pictures with reordering; AmdVcnSelfTestHevcStream.inc) is decoded by the CPU decoder and by
  // the engine, and the pictures must be byte for byte the same. What passing proves beyond the above: the reference picture set and slot bookkeeping of the H.265
  // message, 32x32 coding trees with the deblocking filter and SAO the encoder chose, bi-prediction. Not: scaling lists, tiles, slices, weighted prediction, other sizes.
  static Result run_hevc(allocators::IAllocator& allocator, const AmdVcnHevcDecoder::Engine& engine);
  static const char* result_name(Result result);
};

}  // namespace hardware
