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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

// The LZX decompressor, in the form cabinets use it (Microsoft's "LZX DELTA"-less format of [MS-PATCH], as a folder of a .cab holds it): a window of
// 32 KiB to 2 MiB, the output cut into frames of 32768 bytes of which each data block of the cabinet holds exactly one, with the bit stream (16 bit
// little-endian words, most significant bit first) starting anew at each. Inside, blocks of three kinds - verbatim, aligned offset, uncompressed - that
// need not end where frames do; Huffman codes whose lengths are sent as differences from the block before; three repeated offsets; and, if the header
// says so, the translation of the operand of x86 CALL instructions (E8) from absolute back to relative.
//
// The state lives on from block to block: one decoder decodes one folder, from its first block, in order. The data is untrusted: a block that is damaged
// or cut short makes decode_block() fail (and every call after it, until reset()), it never reads or writes outside its buffers.
namespace media {

class LzxDecoder {
 public:
  static constexpr uint32_t kFrameSize = 32768;
  static constexpr uint32_t kMinWindowBits = 15, kMaxWindowBits = 21;

  explicit LzxDecoder(allocators::IAllocator& allocator) : allocator_(allocator) {}

  // Starts a new stream with a window of 2^window_bits bytes (15 to 21). False for another size, or if memory ran out.
  [[nodiscard]] bool reset(uint32_t window_bits);
  // Decodes the next data block - one frame - appending its `out_size` bytes (1 to 32768; only the last frame of a stream is shorter than 32768) to
  // `out`. False if the data is damaged or ends early, or memory ran out; `out` is then as it was, and the stream cannot be continued.
  [[nodiscard]] bool decode_block(ConstByteView in, uint32_t out_size, ByteBuffer* out);

 private:
  struct State;
  allocators::IAllocator& allocator_;
  UniquePointer<State> state_;
};

}  // namespace media
